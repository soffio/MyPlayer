//
// Created by 马永斌 on 16/5/24.
//
#include "log.h"
#include "FFPlayer.h"
#include "MessageQueue.h"
#include <unistd.h>

#define ANDROID_AUDIO_CHANNELS 2

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

static unsigned sws_flags = SWS_BICUBIC;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
            AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct SDL_Rect {
    int x, y;
    int w, h;
} SDL_Rect;

/* options specified by the user */
static AVInputFormat *file_iformat;
static const char *input_filename;
static const char *window_title;
static int fs_screen_width;
static int fs_screen_height;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static int display_disable;
static int show_status = 1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum VideoState::ShowMode show_mode = VideoState::SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static int autorotate = 1;

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

static AVPacket flush_pkt;

#define FF_ALLOC_EVENT   1
#define FF_QUIT_EVENT   2

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2) {
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline int64_t get_valid_channel_layout(int64_t channel_layout,
                                               int channels) {
    if (channel_layout
        && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static void free_picture(Frame *vp);

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList *pkt1;

    if (q->abort_request)
        return -1;

    pkt1 = (MyAVPacketList *) av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    /* XXX: should duplicate packet data in DV case */
    pthread_cond_signal(&q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    int ret;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    pthread_mutex_lock(&q->mutex);
    ret = packet_queue_put_private(q, pkt);
    pthread_mutex_unlock(&q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_free_packet(pkt);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index) {
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = PTHREAD_MUTEX_INITIALIZER;
    q->cond = PTHREAD_COND_INITIALIZER;
    q->abort_request = 1;
}

static void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void packet_queue_abort(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    q->abort_request = 1;

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_start(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    pthread_mutex_unlock(&q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block,
                            int *serial) {
    MyAVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (; ;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue,
                         pthread_cond_t *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = *empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int got_frame = 0;

    do {
        int ret = -1;

        if (d->queue->abort_request)
            return -1;

        if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
            AVPacket pkt;
            do {
                if (d->queue->nb_packets == 0)
                    pthread_cond_signal(&d->empty_queue_cond);
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (pkt.data == flush_pkt.data) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            } while (pkt.data == flush_pkt.data
                     || d->queue->serial != d->pkt_serial);
            av_free_packet(&d->pkt);
            d->pkt_temp = d->pkt = pkt;
            d->packet_pending = 1;
        }

        switch (d->avctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                ret = avcodec_decode_video2(d->avctx, frame, &got_frame,
                                            &d->pkt_temp);
                if (got_frame) {
                    if (decoder_reorder_pts == -1) {
                        frame->pts = av_frame_get_best_effort_timestamp(frame);
                    } else if (decoder_reorder_pts) {
                        frame->pts = frame->pkt_pts;
                    } else {
                        frame->pts = frame->pkt_dts;
                    }
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ret = avcodec_decode_audio4(d->avctx, frame, &got_frame,
                                            &d->pkt_temp);
                if (got_frame) {
                    AVRational tb = (AVRational) {1, frame->sample_rate};
                    if (frame->pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(frame->pts, d->avctx->time_base,
                                                  tb);
                    else if (frame->pkt_pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(frame->pkt_pts,
                                                  av_codec_get_pkt_timebase(d->avctx), tb);
                    else if (d->next_pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                    if (frame->pts != AV_NOPTS_VALUE) {
                        d->next_pts = frame->pts + frame->nb_samples;
                        d->next_pts_tb = tb;
                    }
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame,
                                               &d->pkt_temp);
                break;
        }

        if (ret < 0) {
            d->packet_pending = 0;
        } else {
            d->pkt_temp.dts = d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                if (d->avctx->codec_type != AVMEDIA_TYPE_AUDIO)
                    ret = d->pkt_temp.size;
                d->pkt_temp.data += ret;
                d->pkt_temp.size -= ret;
                if (d->pkt_temp.size <= 0)
                    d->packet_pending = 0;
            } else {
                if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                }
            }
        }
    } while (!got_frame && !d->finished);

    return got_frame;
}

static void decoder_destroy(Decoder *d) {
    av_free_packet(&d->pkt);
}

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size,
                            int keep_last) {
    int i;
    memset(f, 0, sizeof(FrameQueue));

    f->mutex = PTHREAD_MUTEX_INITIALIZER;
    f->cond = PTHREAD_COND_INITIALIZER;
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destory(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
        free_picture(vp);
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
}

static void frame_queue_signal(FrameQueue *f) {
    pthread_mutex_lock(&f->mutex);
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f) {
    /* wait until we have space to put a new frame */
    pthread_mutex_lock(&f->mutex);
    while (f->size >= f->max_size && !f->pktq->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f) {
    /* wait until we have a readable a new frame */
    pthread_mutex_lock(&f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size)
        f->windex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size++;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

static void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    pthread_mutex_lock(&f->mutex);
    f->size--;
    pthread_cond_signal(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}

/* jump back to the previous frame if available by resetting rindex_shown */
static int frame_queue_prev(FrameQueue *f) {
    int ret = f->rindex_shown;
    f->rindex_shown = 0;
    return ret;
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f) {
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    pthread_join(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

//static inline void fill_rectangle(SDL_Surface *screen, int x, int y, int w,
//                                  int h, int color, int update) {
//    SDL_Rect rect;
//    rect.x = x;
//    rect.y = y;
//    rect.w = w;
//    rect.h = h;
//    SDL_FillRect(screen, &rect, color);
//	if (update && w > 0 && h > 0)
//		SDL_UpdateRect(screen, x, y, w, h);
//}

/* draw only the border of a rectangle */
static void fill_border(int xleft, int ytop, int width, int height, int x,
                        int y, int w, int h, int color, int update) {
    int w1, w2, h1, h2;

    /* fill the background */
    w1 = x;
    if (w1 < 0)
        w1 = 0;
    w2 = width - (x + w);
    if (w2 < 0)
        w2 = 0;
    h1 = y;
    if (h1 < 0)
        h1 = 0;
    h2 = height - (y + h);
    if (h2 < 0)
        h2 = 0;
//    fill_rectangle(screen, xleft, ytop, w1, height, color, update);
//    fill_rectangle(screen, xleft + width - w2, ytop, w2, height, color, update);
//    fill_rectangle(screen, xleft + w1, ytop, width - w1 - w2, h1, color,
//                   update);
//    fill_rectangle(screen, xleft + w1, ytop + height - h2, width - w1 - w2, h2,
//                   color, update);
}

#define ALPHA_BLEND(a, oldp, newp, s)\
((((oldp << s) * (255 - (a))) + (newp * (a))) / (255 << s))

#define BPP 1

//static void blend_subrect(SDL_Texture *bmp, AVPicture *dst,
//                          const AVSubtitleRect *rect, int imgw, int imgh) {
//    int x, y, Y, U, V, A;
//    uint8_t *lum, *cb, *cr;
//    int dstx, dsty, dstw, dsth;
//    SDL_Rect dstRect;
//    const AVPicture *src = &rect->pict;
//
//    dstw = av_clip(rect->w, 0, imgw);
//    dsth = av_clip(rect->h, 0, imgh);
//    dstx = av_clip(rect->x, 0, imgw - dstw);
//    dsty = av_clip(rect->y, 0, imgh - dsth);
//    dstRect.w = dstw;
//    dstRect.h = dsth;
//    dstRect.x = dstx;
//    dstRect.y = dsty;
//
//    lum = dst->data[0] + dstx + dsty * dst->linesize[0];
//    cb = dst->data[1] + dstx / 2 + (dsty >> 1) * dst->linesize[1];
//    cr = dst->data[2] + dstx / 2 + (dsty >> 1) * dst->linesize[2];
//
//    for (y = 0; y < dsth; y++) {
//        for (x = 0; x < dstw; x++) {
//            Y = src->data[0][x + y * src->linesize[0]];
//            A = src->data[3][x + y * src->linesize[3]];
//            lum[0] = ALPHA_BLEND(A, lum[0], Y, 0);
//            lum++;
//        }
//        lum += dst->linesize[0] - dstw;
//    }
//
//    for (y = 0; y < dsth / 2; y++) {
//        for (x = 0; x < dstw / 2; x++) {
//            U = src->data[1][x + y * src->linesize[1]];
//            V = src->data[2][x + y * src->linesize[2]];
//            A = src->data[3][2 * x + 2 * y * src->linesize[3]]
//                + src->data[3][2 * x + 1 + 2 * y * src->linesize[3]]
//                + src->data[3][2 * x + 1 + (2 * y + 1) * src->linesize[3]]
//                + src->data[3][2 * x + (2 * y + 1) * src->linesize[3]];
//            cb[0] = ALPHA_BLEND(A >> 2, cb[0], U, 0);
//            cr[0] = ALPHA_BLEND(A >> 2, cr[0], V, 0);
//            cb++;
//            cr++;
//        }
//        cb += dst->linesize[1] - dstw / 2;
//        cr += dst->linesize[2] - dstw / 2;
//    }
//
//    SDL_UpdateYUVTexture(bmp, &dstRect, rect->pict.data[0],
//                         rect->pict.linesize[0], NULL, 0, NULL, 0);
//}

static void free_picture(Frame *vp) {
//    if (vp->bmp) {
//        SDL_DestroyTexture(vp->bmp);
//        vp->bmp = NULL;
//    }
    av_frame_free(&vp->pFrameRGBA);
}

static void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop,
                                   int scr_width, int scr_height, int pic_width, int pic_height,
                                   AVRational pic_sar) {
    float aspect_ratio;
    int width, height, x, y;

    if (pic_sar.num == 0)
        aspect_ratio = 0;
    else
        aspect_ratio = av_q2d(pic_sar);

    if (aspect_ratio <= 0.0)
        aspect_ratio = 1.0;
    aspect_ratio *= (float) pic_width / (float) pic_height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = ((int) rint(height * aspect_ratio)) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = ((int) rint(width / aspect_ratio)) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX(width, 1);
    rect->h = FFMAX(height, 1);
}

static void video_image_display(VideoState *is) {
    ALOGD("video_image_display");
    Frame *vp;
    Frame *sp;
    AVPicture pict;
    SDL_Rect rect;
    int i;

    vp = frame_queue_peek(&is->pictq);
    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts
                >= sp->pts
                   + ((float) sp->sub.start_display_time / 1000)) {
                int w, h;
//                SDL_QueryTexture(vp->bmp, NULL, NULL, &w, &h);
//                for (i = 0; i < sp->sub.num_rects; i++)
//                    blend_subrect(vp->bmp, &pict, sp->sub.rects[i], w, h);
            }
        }
    }

    calculate_display_rect(&rect, is->xleft, is->ytop, is->width,
                           is->height, vp->width, vp->height, vp->sar);

    ANativeWindow_Buffer windowBuffer;
    ANativeWindow_lock(is->window, &windowBuffer, NULL);

    // 获取stride
    uint8_t *dst = (uint8_t *) windowBuffer.bits;
    int dstStride = windowBuffer.stride * 4;
    uint8_t *src = (uint8_t * )(vp->pFrameRGBA->data[0]);
    int srcStride = vp->pFrameRGBA->linesize[0];

    // 由于window的stride和帧的stride不同,因此需要逐行复制
    int h;
    for (h = 0; h < vp->height; h++) {
        memcpy(dst + h * dstStride, src + h * srcStride, srcStride);
    }

    ANativeWindow_unlockAndPost(is->window);

//		if (rect.x != is->last_display_rect.x
//				|| rect.y != is->last_display_rect.y
//				|| rect.w != is->last_display_rect.w
//				|| rect.h != is->last_display_rect.h || is->force_refresh) {
//			int bgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
//			fill_border(is->xleft, is->ytop, is->width, is->height, rect.x,
//					rect.y, rect.w, rect.h, bgcolor, 1);
//			is->last_display_rect = rect;
//		}
//    }
}

static inline int compute_mod(int a, int b) {
    return a < 0 ? a % b + b : a % b;
}

static void video_audio_display(VideoState *s) {
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2, bgcolor, fgcolor;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++);
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = s->audio_tgt.channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used =
                s->show_mode == VideoState::SHOW_MODE_WAVES ?
                s->width : (2 * nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
         the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start = x = compute_mod(s->sample_array_index - delay * channels,
                                  SAMPLE_ARRAY_SIZE);
        if (s->show_mode == VideoState::SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b =
                        s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c =
                        s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d =
                        s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

//	bgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
    if (s->show_mode == VideoState::SHOW_MODE_WAVES) {
//        fill_rectangle(screen, s->xleft, s->ytop, s->width, s->height, bgcolor,
//                       0);
//
//        fgcolor = SDL_MapRGB(screen->format, 0xff, 0xff, 0xff);
//
//        /* total height for one channel */
//        h = s->height / nb_display_channels;
//        /* graph height / 2 */
//        h2 = (h * 9) / 20;
//        for (ch = 0; ch < nb_display_channels; ch++) {
//            i = i_start + ch;
//            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
//            for (x = 0; x < s->width; x++) {
//                y = (s->sample_array[i] * h2) >> 15;
//                if (y < 0) {
//                    y = -y;
//                    ys = y1 - y;
//                } else {
//                    ys = y1;
//                }
//                fill_rectangle(screen, s->xleft + x, ys, 1, y, fgcolor, 0);
//                i += channels;
//                if (i >= SAMPLE_ARRAY_SIZE)
//                    i -= SAMPLE_ARRAY_SIZE;
//            }
//        }
//
//        fgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0xff);
//
//        for (ch = 1; ch < nb_display_channels; ch++) {
//            y = s->ytop + ch * h;
//            fill_rectangle(screen, s->xleft, y, s->width, 1, fgcolor, 0);
//        }
//		SDL_UpdateRect(screen, s->xleft, s->ytop, s->width, s->height);
    } else {
        nb_display_channels = FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            av_rdft_end(s->rdft);
            av_free(s->rdft_data);
            s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            s->rdft_bits = rdft_bits;
            s->rdft_data = (FFTSample *) av_malloc_array(nb_freq,
                                                         4 * sizeof(*s->rdft_data));
        }
        if (!s->rdft || !s->rdft_data) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = VideoState::SHOW_MODE_WAVES;
        } else {
            FFTSample *data[2];
            for (ch = 0; ch < nb_display_channels; ch++) {
                data[ch] = s->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x - nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av_rdft_calc(s->rdft, data[ch]);
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            for (y = 0; y < s->height; y++) {
                double w = 1 / sqrt((double) nb_freq);
                int a = sqrt(
                        w
                        * sqrt(
                                data[0][2 * y + 0] * data[0][2 * y + 0]
                                + data[0][2 * y + 1]
                                  * data[0][2 * y + 1]));
                int b = (nb_display_channels == 2) ?
                        sqrt(
                                w
                                * sqrt(
                                        data[1][2 * y + 0]
                                        * data[1][2 * y + 0]
                                        + data[1][2 * y + 1]
                                          * data[1][2 * y
                                                    + 1])) :
                        a;
                a = FFMIN(a, 255);
                b = FFMIN(b, 255);
//                fgcolor = SDL_MapRGB(screen->format, a, b, (a + b) / 2);
//
//                fill_rectangle(screen, s->xpos, s->height - y, 1, 1, fgcolor,
//                               0);
            }
        }
//		SDL_UpdateRect(screen, s->xpos, s->ytop, 1, s->height);
        if (!s->paused)
            s->xpos++;
        if (s->xpos >= s->width)
            s->xpos = s->xleft;
    }
}

static void stream_close(VideoState *is) {
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    pthread_join(is->read_tid, NULL);
    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    pthread_cond_destroy(&is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    delete (is->messageQueue);
    av_free(is);
}

static void do_exit(VideoState *is) {
    if (is) {
        stream_close(is);
    }
    av_lockmgr_register(NULL);
    avformat_network_deinit();
    if (show_status)
        printf("\n");
//    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig) {
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar) {
    SDL_Rect rect;
    calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
    default_width = rect.w;
    default_height = rect.h;
}

static int video_open(VideoState *is, int force_set_video_mode, Frame *vp) {
    int flags = 0;
    int w, h;

    if (vp && vp->width)
        set_default_window_size(vp->width, vp->height, vp->sar);

    if (is_full_screen && fs_screen_width) {
        w = fs_screen_width;
        h = fs_screen_height;
    } else if (!is_full_screen && screen_width) {
        w = screen_width;
        h = screen_height;
    } else {
        w = default_width;
        h = default_height;
    }
    w = FFMIN(16383, w);
//    if (win && is->width == w && is->height == h && !force_set_video_mode)
//        return 0;
    if (!window_title)
        window_title = input_filename;
//    win = SDL_CreateWindow(window_title, SDL_WINDOWPOS_UNDEFINED,
//                           SDL_WINDOWPOS_UNDEFINED, w, h, flags);
//    if (!win) {
//        av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
//        do_exit(is);
//    }
//    render = SDL_CreateRenderer(win, -1, 0);
    is->width = w;
    is->height = h;

    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is) {
//    if (!render)
//        video_open(is, 0, NULL);
    if (is->audio_st && is->show_mode != VideoState::SHOW_MODE_VIDEO)
        video_audio_display(is);
    else if (is->video_st)
        video_image_display(is);
}

static double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed) {
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock)
        && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
    if (is->video_stream
        >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(&is->extclk,
                        FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                              is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0
                || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)
               && (is->audio_stream < 0
                   || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk,
                        FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                              is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk,
                            speed
                            + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed)
                              / fabs(1.0 - speed));
    }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel,
                        int seek_by_bytes) {
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        pthread_cond_signal(&is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is) {
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0
                           - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused =
            !is->paused;
}

static void toggle_pause(VideoState *is) {
    stream_toggle_pause(is);
    is->step = 0;
}

static void step_to_next_frame(VideoState *is) {
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is) {
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
         duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
         delay to compute the threshold. I still don't know
         if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN,
                               FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff
                     >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0
            || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos,
                             int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time) {
    VideoState *is = (VideoState *) opaque;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK
        && is->realtime)
        check_external_clock_speed(is);

    if (!display_disable && is->show_mode != VideoState::SHOW_MODE_VIDEO
        && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time,
                                is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
        int redisplay = 0;
        if (is->force_refresh)
            redisplay = frame_queue_prev(&is->pictq);
        retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                redisplay = 0;
                goto retry;
            }

            if (lastvp->serial != vp->serial && !redisplay)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            if (redisplay)
                delay = 0.0;
            else
                delay = compute_target_delay(last_duration, is);

            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay && !redisplay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time,
                                        *remaining_time);
                return;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            pthread_mutex_lock(&is->pictq.mutex);
            if (!redisplay && !isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            pthread_mutex_unlock(&is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step
                    && (redisplay || framedrop > 0
                        || (framedrop
                            && get_master_sync_type(is)
                               != AV_SYNC_VIDEO_MASTER))
                    && time > is->frame_timer + duration) {
                    if (!redisplay)
                        is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    redisplay = 0;
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial
                        || (is->vidclk.pts
                            > (sp->pts
                               + ((float) sp->sub.end_display_time
                                  / 1000)))
                        || (sp2
                            && is->vidclk.pts
                               > (sp2->pts
                                  + ((float) sp2->sub.start_display_time
                                     / 1000)))) {
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            display:
            /* display picture */
            if (!display_disable
                && is->show_mode == VideoState::SHOW_MODE_VIDEO)
                video_display(is);

            frame_queue_next(&is->pictq);

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
    }
    is->force_refresh = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"
            PRId64
            "/%"
            PRId64
            "   \r",
                    get_master_clock(is),
                    (is->audio_st && is->video_st) ?
                    "A-V" :
                    (is->video_st ?
                     "M-V" : (is->audio_st ? "M-A" : "   ")),
                    av_diff, is->frame_drops_early + is->frame_drops_late,
                    aqsize / 1024, vqsize / 1024, sqsize,
                    is->video_st ?
                    is->video_st->codec->pts_correction_num_faulty_dts :
                    0,
                    is->video_st ?
                    is->video_st->codec->pts_correction_num_faulty_pts :
                    0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

static int do_scale_picture(VideoState *is, Frame *vp, AVFrame *src_frame) {
    AVFrame *pict = NULL;

    pict = av_frame_alloc();
    pict->format = AV_PIX_FMT_RGBA;
    pict->width = vp->width;
    pict->height = vp->height;
    av_frame_get_buffer(pict, 32);

    is->img_convert_ctx = sws_getCachedContext(is->img_convert_ctx, vp->width,
                                               vp->height, AVPixelFormat(src_frame->format),
                                               vp->width, vp->height,
                                               AV_PIX_FMT_RGBA, sws_flags, NULL, NULL, NULL);
    if (!is->img_convert_ctx) {
        av_log(NULL, AV_LOG_FATAL,
               "Cannot initialize the conversion context\n");
        exit(1);
    }
    sws_scale(is->img_convert_ctx, (const uint8_t *const *) src_frame->data, src_frame->linesize, 0,
              vp->height, pict->data, pict->linesize);
//    SDL_UpdateYUVTexture(vp->bmp, NULL, pict->data[0], pict->linesize[0],
//                         pict->data[1], pict->linesize[1], pict->data[2], pict->linesize[2]);
//    if (pict != src_frame) {
//        av_frame_free(&pict);
//    }
    vp->pFrameRGBA = pict;
    return 0;
}

/* allocate a picture (needs to do that in main thread to avoid
 potential locking problems */
static void alloc_picture(VideoState *is, AVFrame *src) {
    Frame *vp;
    int64_t bufferdiff;

    vp = &is->pictq.queue[is->pictq.windex];

//    if (!vp->bmp || vp->width != src->width || vp->height != src->height) {
//
//        free_picture(vp);
//        video_open(is, 0, vp);
//
//        vp->bmp = SDL_CreateTexture(render, SDL_PIXELFORMAT_IYUV,
//                                    SDL_TEXTUREACCESS_STREAMING, vp->width, vp->height);
//        if (!vp->bmp) {
//            av_log(NULL, AV_LOG_FATAL,
//                   "Error: the video system does not support an image\n"
//                           "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
//                           "to reduce the image size.err: %s\n", vp->width,
//                   vp->height, SDL_GetError());
//            do_exit(is);
//        }
//        printf("alloc texture %dx%d\n", vp->width, vp->height);
//    }

//	bufferdiff =
//			vp->bmp ?
//					FFMAX(vp->bmp->pixels[0],
//							vp->bmp->pixels[1]) - FFMIN(vp->bmp->pixels[0], vp->bmp->pixels[1]) :
//					0;
//	if (!vp->bmp || vp->bmp->pitches[0] < vp->width
//			|| bufferdiff < (int64_t) vp->height * vp->bmp->pitches[0]) {
//		/* SDL allocates a buffer smaller than requested if the video
//		 * overlay hardware is unable to support the requested size. */
//		av_log(NULL, AV_LOG_FATAL,
//				"Error: the video system does not support an image\n"
//						"size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
//						"to reduce the image size.\n", vp->width, vp->height);
//		do_exit(is);
//	}
    do_scale_picture(is, vp, src);
    pthread_mutex_lock(&is->pictq.mutex);
    vp->allocated = 1;
    pthread_cond_signal(&is->pictq.cond);
    pthread_mutex_unlock(&is->pictq.mutex);
}

//static void duplicate_right_border_pixels(SDL_Overlay *bmp) {
//	int i, width, height;
//	Uint8 *p, *maxp;
//	for (i = 0; i < 3; i++) {
//		width = bmp->w;
//		height = bmp->h;
//		if (i > 0) {
//			width >>= 1;
//			height >>= 1;
//		}
//		if (bmp->pitches[i] > width) {
//			maxp = bmp->pixels[i] + bmp->pitches[i] * height - 1;
//			for (p = bmp->pixels[i] + width - 1; p < maxp; p += bmp->pitches[i])
//				*(p + 1) = *p;
//		}
//	}
//}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts,
                         double duration, int64_t pos, int serial) {
    Frame *vp;

#if defined(DEBUG_SYNC) && 0
    printf("frame_type=%c pts=%0.3f\n",
            av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;

    /* alloc or resize hardware picture buffer */
//	if (!vp->bmp || vp->reallocate || !vp->allocated
//			|| vp->width != src_frame->width
//			|| vp->height != src_frame->height)
    {

        vp->allocated = 0;
        vp->reallocate = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;

        alloc_picture(is, src_frame);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
//    if (vp->bmp) {
    /* workaround SDL PITCH_WORKAROUND */
//		duplicate_right_border_pixels(vp->bmp);
    /* update the bitmap content */

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    /* now we can update the picture count */
    frame_queue_push(&is->pictq);
//    }
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame) {
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic,
                                                                  is->video_st, frame);

        is->viddec_width = frame->width;
        is->viddec_height = frame->height;

        if (framedrop > 0
            || (framedrop
                && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD
                    && diff - is->frame_last_filter_delay < 0
                    && is->viddec.pkt_serial == is->vidclk.serial
                    && is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static void *audio_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return (void *) AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
            tb = (AVRational) {1, frame->sample_rate};

            if (!(af = frame_queue_peek_writable(&is->sampq)))
                goto the_end;

            af->pts = (frame->pts == AV_NOPTS_VALUE) ?
                      NAN :
                      frame->pts * av_q2d(tb);
            af->pos = av_frame_get_pkt_pos(frame);
            af->serial = is->auddec.pkt_serial;
            af->duration =
                    av_q2d(
                            (AVRational) {frame->nb_samples,
                                          frame->sample_rate});

            av_frame_move_ref(af->frame, frame);
            frame_queue_push(&is->sampq);

        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    the_end:
    av_frame_free(&frame);
    return (void *) ret;
}

static void decoder_start(Decoder *d, void *(*fn)(void *), void *arg) {
    packet_queue_start(d->queue);
    pthread_create(&d->decoder_tid, NULL, fn, arg);
    pthread_setname_np(d->decoder_tid, "decoder");
}

static void *video_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    if (!frame) {
        return (void *) AVERROR(ENOMEM);
    }

    for (; ;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) {
                frame_rate.den, frame_rate.num}) :
                    0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(is, frame, pts, duration,
                            av_frame_get_pkt_pos(frame), is->viddec.pkt_serial);
        av_frame_unref(frame);

        if (ret < 0)
            goto the_end;
    }
    the_end:
    av_frame_free(&frame);
    return 0;
}

static void *subtitle_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    Frame *sp;
    int got_subtitle;
    double pts;
    int i;

    for (; ;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub))
            < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double) AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;

            for (i = 0; i < sp->sub.num_rects; i++) {
                int in_w = sp->sub.rects[i]->w;
                int in_h = sp->sub.rects[i]->h;
                int subw =
                        is->subdec.avctx->width ?
                        is->subdec.avctx->width : is->viddec_width;
                int subh =
                        is->subdec.avctx->height ?
                        is->subdec.avctx->height : is->viddec_height;
                int out_w =
                        is->viddec_width ?
                        in_w * is->viddec_width / subw : in_w;
                int out_h =
                        is->viddec_height ?
                        in_h * is->viddec_height / subh : in_h;
                AVPicture newpic;

                //can not use avpicture_alloc as it is not compatible with avsubtitle_free()
                av_image_fill_linesizes(newpic.linesize, AV_PIX_FMT_YUVA420P,
                                        out_w);
                newpic.data[0] = (uint8_t *) av_malloc(
                        newpic.linesize[0] * out_h);
                newpic.data[3] = (uint8_t *) av_malloc(
                        newpic.linesize[3] * out_h);
                newpic.data[1] = (uint8_t *) av_malloc(
                        newpic.linesize[1] * ((out_h + 1) / 2));
                newpic.data[2] = (uint8_t *) av_malloc(
                        newpic.linesize[2] * ((out_h + 1) / 2));

                is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                           in_w, in_h, AV_PIX_FMT_PAL8, out_w,
                                                           out_h,
                                                           AV_PIX_FMT_YUVA420P, sws_flags, NULL,
                                                           NULL, NULL);
                if (!is->sub_convert_ctx || !newpic.data[0] || !newpic.data[3]
                    || !newpic.data[1] || !newpic.data[2]) {
                    av_log(NULL, AV_LOG_FATAL,
                           "Cannot initialize the sub conversion context\n");
                    exit(1);
                }
                sws_scale(is->sub_convert_ctx,
                          (const uint8_t *const *) sp->sub.rects[i]->pict.data,
                          sp->sub.rects[i]->pict.linesize, 0, in_h, newpic.data,
                          newpic.linesize);

                av_free(sp->sub.rects[i]->pict.data[0]);
                av_free(sp->sub.rects[i]->pict.data[1]);
                sp->sub.rects[i]->pict = newpic;
                sp->sub.rects[i]->w = out_w;
                sp->sub.rects[i]->h = out_h;
                sp->sub.rects[i]->x = sp->sub.rects[i]->x * out_w / in_w;
                sp->sub.rects[i]->y = sp->sub.rects[i]->y * out_h / in_h;
            }

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples,
                                  int samples_size) {
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples,
               len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff
                                 + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples
                                        + (int) (diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples
                                       * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples
                                       * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples,
                                                min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE,
                       "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
                       avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
             reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused
    double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time)
                    > 1000000LL * is->audio_hw_buf_size
                            / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep(1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL,
                                           av_frame_get_channels(af->frame), af->frame->nb_samples,
                                           AVSampleFormat(af->frame->format), 1);

    dec_channel_layout =
            (af->frame->channel_layout
             && av_frame_get_channels(af->frame)
                == av_get_channel_layout_nb_channels(
                    af->frame->channel_layout)) ?
            af->frame->channel_layout :
            av_get_default_channel_layout(
                    av_frame_get_channels(af->frame));
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt
        || dec_channel_layout != is->audio_src.channel_layout
        || af->frame->sample_rate != is->audio_src.freq
        || (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL, is->audio_tgt.channel_layout,
                                         is->audio_tgt.fmt, is->audio_tgt.freq, dec_channel_layout,
                                         AVSampleFormat(af->frame->format), af->frame->sample_rate,
                                         0,
                                         NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate,
                   av_get_sample_fmt_name(AVSampleFormat(af->frame->format)),
                   av_frame_get_channels(af->frame), is->audio_tgt.freq,
                   av_get_sample_fmt_name(is->audio_tgt.fmt),
                   is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels = av_frame_get_channels(af->frame);
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = AVSampleFormat(af->frame->format);
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **) af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t) wanted_nb_samples * is->audio_tgt.freq
                        / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels,
                                                  out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx,
                                     (wanted_nb_samples - af->frame->nb_samples)
                                     * is->audio_tgt.freq / af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq
                                     / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in,
                           af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING,
                   "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels
                              * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts
                          + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
                is->audio_clock - last_clock,
                is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, uint8_t *stream, int len) {
    VideoState *is = (VideoState *) opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = is->silence_buf;
                is->audio_buf_size = sizeof(is->silence_buf)
                                     / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != VideoState::SHOW_MODE_VIDEO)
                    update_sample_display(is, (int16_t *) is->audio_buf,
                                          audio_size);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk,
                     is->audio_clock
                     - (double) (2 * is->audio_hw_buf_size
                                 + is->audio_write_buf_size)
                       / is->audio_tgt.bytes_per_sec,
                     is->audio_clock_serial, audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(VideoState *videoState, int64_t wanted_channel_layout,
                      int wanted_nb_channels, int wanted_sample_rate,
                      struct AudioParams *audio_hw_params) {
    videoState->audioEngine = new AudioEngine();
    videoState->audioEngine->createEngine();
    videoState->audioEngine->createBufferQueueAudioPlayer(wanted_sample_rate,
                                                          ANDROID_AUDIO_CHANNELS, videoState,
                                                          sdl_audio_callback);
    wanted_channel_layout = av_get_default_channel_layout(
            ANDROID_AUDIO_CHANNELS);

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = wanted_sample_rate;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = ANDROID_AUDIO_CHANNELS;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL,
                                                             audio_hw_params->channels, 1,
                                                             audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL,
                                                                audio_hw_params->channels,
                                                                audio_hw_params->freq,
                                                                audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0
        || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return 1;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = ic->streams[stream_index]->codec;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->last_audio_stream = stream_index;
            forced_codec_name = audio_codec_name;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->last_subtitle_stream = stream_index;
            forced_codec_name = subtitle_codec_name;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->last_video_stream = stream_index;
            forced_codec_name = video_codec_name;
            break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(NULL, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n",
                   forced_codec_name);
        else
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with id %d\n",
                   avctx->codec_id);
        return -1;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > av_codec_get_max_lowres(codec)) {
        av_log(avctx, AV_LOG_WARNING,
               "The maximum value for lowres supported by the decoder is %d\n",
               av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if (stream_lowres)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if (codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

//	opts = filter_codec_opts(codec_opts, avctx->codec_id, ic,
//			ic->streams[stream_index], codec);
//	if (!av_dict_get(opts, "threads", NULL, 0))
//		av_dict_set(&opts, "threads", "auto", 0);
//	if (stream_lowres)
//		av_dict_set_int(&opts, "lowres", stream_lowres, 0);
//	if (avctx->codec_type == AVMEDIA_TYPE_VIDEO
//			|| avctx->codec_type == AVMEDIA_TYPE_AUDIO)
//		av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        goto fail;
    }
//	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
//		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
//		ret = AVERROR_OPTION_NOT_FOUND;
//		goto fail;
//	}

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            sample_rate = avctx->sample_rate;
            nb_channels = avctx->channels;
            channel_layout = avctx->channel_layout;

            /* prepare audio output */
            if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate,
                                  &is->audio_tgt)) < 0)
                goto fail;
            is->audio_hw_buf_size = ret;
            is->audio_src = is->audio_tgt;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;

            /* init averaging filter */
            is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
            is->audio_diff_avg_count = 0;
            /* since we do not have a precise anough audio fifo fullness,
             we correct audio sync only if larger than this threshold */
            is->audio_diff_threshold = (double) (is->audio_hw_buf_size)
                                       / is->audio_tgt.bytes_per_sec;

            is->audio_stream = stream_index;
            is->audio_st = ic->streams[stream_index];

            decoder_init(&is->auddec, avctx, &is->audioq, &is->continue_read_thread);
            if ((is->ic->iformat->flags
                 & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK))
                && !is->ic->iformat->read_seek) {
                is->auddec.start_pts = is->audio_st->start_time;
                is->auddec.start_pts_tb = is->audio_st->time_base;
            }
            decoder_start(&is->auddec, audio_thread, is);
            // TODO push data
            is->audioEngine->enqueueStartBuffer();
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];

            is->viddec_width = avctx->width;
            is->viddec_height = avctx->height;

            decoder_init(&is->viddec, avctx, &is->videoq, &is->continue_read_thread);
            decoder_start(&is->viddec, video_thread, is);
            is->queue_attachments_req = 1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];

            decoder_init(&is->subdec, avctx, &is->subtitleq,
                         &is->continue_read_thread);
            decoder_start(&is->subdec, subtitle_thread, is);
            break;
        default:
            break;
    }

//	fail: av_dict_free(&opts);
    fail:;

    return ret;
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            decoder_abort(&is->auddec, &is->sampq);
            is->audioEngine->shutdown();
            decoder_destroy(&is->auddec);
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = NULL;

            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = NULL;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&is->viddec, &is->pictq);
            decoder_destroy(&is->viddec);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            decoder_abort(&is->subdec, &is->subpq);
            decoder_destroy(&is->subdec);
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = NULL;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = NULL;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = NULL;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

static int decode_interrupt_cb(void *ctx) {
    VideoState *is = (VideoState *) ctx;
    return is->abort_request;
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp"))
        return 1;

    if (s->pb
        && (!strncmp(s->filename, "rtp:", 4)
            || !strncmp(s->filename, "udp:", 4)))
        return 1;
    return 0;
}

static void print_error(const char *filename, int err) {
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

/* this thread gets the stream from the disk or the network */
static void *read_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    int orig_nb_streams;
    pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
    int64_t pkt_ts;

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    err = avformat_open_input(&ic, is->filename, is->iformat, NULL);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }

    is->ic = ic;

    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, NULL);

//	for (i = 0; i < orig_nb_streams; i++)
//		av_dict_free (&opts[i]);

    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (seek_by_bytes < 0)
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT)
                        && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration =
            (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not seek to position %0.3f\n", is->filename,
                   (double) timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codec->codec_type;
        st->discard = AVDISCARD_ALL;
        if (wanted_stream_spec[type] && st_index[type] == -1) if (
                avformat_match_stream_specifier(ic, st,
                                                wanted_stream_spec[type]) > 0)
            st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR,
                   "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i],
                   av_get_media_type_string(AVMediaType(i)));
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic,
                                                           AVMEDIA_TYPE_VIDEO,
                                                           st_index[AVMEDIA_TYPE_VIDEO], -1, NULL,
                                                           0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic,
                                                           AVMEDIA_TYPE_AUDIO,
                                                           st_index[AVMEDIA_TYPE_AUDIO],
                                                           st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic,
                                                              AVMEDIA_TYPE_SUBTITLE,
                                                              st_index[AVMEDIA_TYPE_SUBTITLE],
                                                              (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                                               st_index[AVMEDIA_TYPE_AUDIO] :
                                                               st_index[AVMEDIA_TYPE_VIDEO]), NULL,
                                                              0);

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecContext *avctx = st->codec;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (avctx->width)
            set_default_window_size(avctx->width, avctx->height, sar);

        ANativeWindow_setBuffersGeometry(is->window, avctx->width, avctx->height,
                                         WINDOW_FORMAT_RGBA_8888);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == VideoState::SHOW_MODE_NONE)
        is->show_mode =
                ret >= 0 ?
                VideoState::SHOW_MODE_VIDEO :
                VideoState::SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    for (; ;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                        (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min =
                    is->seek_rel > 0 ?
                    seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max =
                    is->seek_rel < 0 ?
                    seek_target - is->seek_rel - 2 : INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target,
                                     seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n",
                       is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double) AV_TIME_BASE,
                              0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st
                && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic))
                    < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer < 1
            && (is->audioq.size + is->videoq.size + is->subtitleq.size
                > MAX_QUEUE_SIZE
                || ((is->audioq.nb_packets > MIN_FRAMES
                     || is->audio_stream < 0
                     || is->audioq.abort_request)
                    && (is->videoq.nb_packets > MIN_FRAMES
                        || is->video_stream < 0
                        || is->videoq.abort_request
                        || (is->video_st->disposition
                            & AV_DISPOSITION_ATTACHED_PIC))
                    && (is->subtitleq.nb_packets > MIN_FRAMES
                        || is->subtitle_stream < 0
                        || is->subtitleq.abort_request)))) {
            /* wait 10 ms */
            struct timeval delta;
            struct timespec abstime;
            gettimeofday(&delta, NULL);
            abstime.tv_sec = delta.tv_sec + (10 / 1000);
            abstime.tv_nsec = (delta.tv_usec + (10 % 1000) * 1000) * 1000;
            if (abstime.tv_nsec > 1000000000) {
                abstime.tv_sec += 1;
                abstime.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&wait_mutex);
            pthread_cond_timedwait(&is->continue_read_thread, &wait_mutex, &abstime);
            pthread_mutex_unlock(&wait_mutex);
            continue;
        }
        if (!is->paused
            && (!is->audio_st
                || (is->auddec.finished == is->audioq.serial
                    && frame_queue_nb_remaining(&is->sampq) == 0))
            && (!is->video_st
                || (is->viddec.finished == is->videoq.serial
                    && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0,
                            0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);

        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq,
                                                is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error)
                break;
            /* wait 10 ms */
            struct timeval delta;
            struct timespec abstime;
            gettimeofday(&delta, NULL);
            abstime.tv_sec = delta.tv_sec + (10 / 1000);
            abstime.tv_nsec = (delta.tv_usec + (10 % 1000) * 1000) * 1000;
            if (abstime.tv_nsec > 1000000000) {
                abstime.tv_sec += 1;
                abstime.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&wait_mutex);
            pthread_cond_timedwait(&is->continue_read_thread, &wait_mutex, &abstime);
            pthread_mutex_unlock(&wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE
                            || (pkt_ts
                                - (stream_start_time != AV_NOPTS_VALUE ?
                                   stream_start_time : 0))
                               * av_q2d(ic->streams[pkt->stream_index]->time_base)
                               - (double) (
                start_time != AV_NOPTS_VALUE ? start_time : 0)
                                 / 1000000 <= ((double) duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            printf("myb packet_queue_put audio\n");
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            printf("myb packet_queue_put video\n");
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream
                   && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!is->abort_request) {
        usleep(100 * 1000);
    }

    ret = 0;
    fail:
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);
    if (ic) {
        avformat_close_input(&ic);
        is->ic = NULL;
    }

    if (ret != 0) {
        Message event;

        event.messageCode = FF_QUIT_EVENT;
        event.p_message = is;
        is->messageQueue->sendMessage(event);
    }
    pthread_mutex_destroy(&wait_mutex);
    return 0;
}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat) {
    VideoState *is;

    is = (VideoState *) av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    av_strlcpy(is->filename, filename, sizeof(is->filename));
    is->iformat = iformat;
    is->ytop = 0;
    is->xleft = 0;
    is->messageQueue = new MessageQueue();

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1)
        < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0)
        < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    packet_queue_init(&is->videoq);
    packet_queue_init(&is->audioq);
    packet_queue_init(&is->subtitleq);

    is->continue_read_thread = PTHREAD_COND_INITIALIZER;

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->av_sync_type = av_sync_type;

    pthread_create(&is->read_tid, NULL, read_thread, is);
    pthread_setname_np(is->read_tid, "read_thread");
    if (!is->read_tid) {
        fail:
        stream_close(is);
        return NULL;
    }
    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type) {
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (; ;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codec->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if (st->codec->sample_rate != 0 && st->codec->channels != 0)
                        goto the_end;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                case AVMEDIA_TYPE_SUBTITLE:
                    goto the_end;
                default:
                    break;
            }
        }
    }
    the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(AVMediaType(codec_type)), old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}

static void toggle_full_screen(VideoState *is) {
//#if defined(__APPLE__) && SDL_VERSION_ATLEAST(1, 2, 14)
//    /* OS X needs to reallocate the SDL overlays */
//    int i;
//    for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
//    is->pictq.queue[i].reallocate = 1;
//#endif
//    is_full_screen = !is_full_screen;
//    video_open(is, 1, NULL);
}

static void toggle_audio_display(VideoState *is) {
//    int bgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
//    int next = is->show_mode;
//    do {
//        next = (next + 1) % VideoState::SHOW_MODE_NB;
//    } while (next != is->show_mode
//             && (next == VideoState::SHOW_MODE_VIDEO && !is->video_st
//                 || next != VideoState::SHOW_MODE_VIDEO && !is->audio_st));
//    if (is->show_mode != next) {
//        fill_rectangle(screen, is->xleft, is->ytop, is->width, is->height,
//                       bgcolor, 1);
//        is->force_refresh = 1;
//        is->show_mode = VideoState::ShowMode(next);
//    }
}

static void refresh_loop_wait_event(VideoState *is, Message &msg) {
    double remaining_time = 0.0;
    while (!is->messageQueue->popMessage(msg)) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != VideoState::SHOW_MODE_NONE
            && (!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
    }
}

static void seek_chapter(VideoState *is, int incr) {
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is,
                av_rescale_q(is->ic->chapters[i]->start,
                             is->ic->chapters[i]->time_base,
                             AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void* event_loop(void *arg) {
    VideoState* cur_stream = (VideoState*)arg;
    Message event;
    double incr, pos, frac;

    for (; ;) {
        double x;
        refresh_loop_wait_event(cur_stream, event);
        switch (event.messageCode) {
//            case SDL_KEYDOWN:
//                if (exit_on_keydown) {
//                    do_exit(cur_stream);
//                    break;
//                }
//                switch (event.key.keysym.sym) {
//                    case SDLK_ESCAPE:
//                    case SDLK_q:
//                        do_exit(cur_stream);
//                        break;
//                    case SDLK_f:
//                        toggle_full_screen(cur_stream);
//                        cur_stream->force_refresh = 1;
//                        break;
//                    case SDLK_p:
//                    case SDLK_SPACE:
//                        toggle_pause(cur_stream);
//                        break;
//                    case SDLK_s: // S: Step to next frame
//                        step_to_next_frame(cur_stream);
//                        break;
//                    case SDLK_a:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
//                        break;
//                    case SDLK_v:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
//                        break;
//                    case SDLK_c:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
//                        break;
//                    case SDLK_t:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
//                        break;
//                    case SDLK_w:
//                        toggle_audio_display(cur_stream);
//                        break;
//                    case SDLK_PAGEUP:
//                        if (cur_stream->ic->nb_chapters <= 1) {
//                            incr = 600.0;
//                            goto do_seek;
//                        }
//                        seek_chapter(cur_stream, 1);
//                        break;
//                    case SDLK_PAGEDOWN:
//                        if (cur_stream->ic->nb_chapters <= 1) {
//                            incr = -600.0;
//                            goto do_seek;
//                        }
//                        seek_chapter(cur_stream, -1);
//                        break;
//                    case SDLK_LEFT:
//                        incr = -10.0;
//                        goto do_seek;
//                    case SDLK_RIGHT:
//                        incr = 10.0;
//                        goto do_seek;
//                    case SDLK_UP:
//                        incr = 60.0;
//                        goto do_seek;
//                    case SDLK_DOWN:
//                        incr = -60.0;
//                    do_seek:
//                        if (seek_by_bytes) {
//                            pos = -1;
//                            if (pos < 0 && cur_stream->video_stream >= 0)
//                                pos = frame_queue_last_pos(&cur_stream->pictq);
//                            if (pos < 0 && cur_stream->audio_stream >= 0)
//                                pos = frame_queue_last_pos(&cur_stream->sampq);
//                            if (pos < 0)
//                                pos = avio_tell(cur_stream->ic->pb);
//                            if (cur_stream->ic->bit_rate)
//                                incr *= cur_stream->ic->bit_rate / 8.0;
//                            else
//                                incr *= 180000.0;
//                            pos += incr;
//                            stream_seek(cur_stream, pos, incr, 1);
//                        } else {
//                            pos = get_master_clock(cur_stream);
//                            if (isnan(pos))
//                                pos = (double) cur_stream->seek_pos / AV_TIME_BASE;
//                            pos += incr;
//                            if (cur_stream->ic->start_time != AV_NOPTS_VALUE
//                                && pos
//                                   < cur_stream->ic->start_time
//                                     / (double) AV_TIME_BASE)
//                                pos =
//                                        cur_stream->ic->start_time
//                                        / (double) AV_TIME_BASE;
//                            stream_seek(cur_stream, (int64_t) (pos * AV_TIME_BASE),
//                                        (int64_t) (incr * AV_TIME_BASE), 0);
//                        }
//                        break;
//                    default:
//                        break;
//                }
//                break;
//            case SDL_WINDOWEVENT:
//                switch (event.window.event) {
//                    case SDL_WINDOWEVENT_EXPOSED:
//                        cur_stream->force_refresh = 1;
//                        break;
//                    case SDL_WINDOWEVENT_RESIZED:
//                        int w, h;
////                        SDL_SetWindowSize(win, FFMIN(16383, (int) event.window.data1),
////                                          (int) event.window.data2);
////                        SDL_SetWindowFullscreen(win,
////                                                (is_full_screen ? SDL_WINDOW_FULLSCREEN : 0));
////                        SDL_GetWindowSize(win, &w, &h);
//                        screen_width = cur_stream->width = w;
//                        screen_height = cur_stream->height = h;
//                        cur_stream->force_refresh = 1;
//                        break;
//                    default:
//                        break;
//                }
//                break;
//            case SDL_MOUSEBUTTONDOWN:
//                if (exit_on_mousedown) {
//                    do_exit(cur_stream);
//                    break;
//                }
//            case SDL_MOUSEMOTION:
//                if (cursor_hidden) {
//                    SDL_ShowCursor(1);
//                    cursor_hidden = 0;
//                }
//                cursor_last_shown = av_gettime_relative();
//                if (event.type == SDL_MOUSEBUTTONDOWN) {
//                    x = event.button.x;
//                } else {
//                    if (event.motion.state != SDL_PRESSED)
//                        break;
//                    x = event.motion.x;
//                }
//                if (seek_by_bytes || cur_stream->ic->duration <= 0) {
//                    uint64_t size = avio_size(cur_stream->ic->pb);
//                    stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
//                } else {
//                    int64_t ts;
//                    int ns, hh, mm, ss;
//                    int tns, thh, tmm, tss;
//                    tns = cur_stream->ic->duration / 1000000LL;
//                    thh = tns / 3600;
//                    tmm = (tns % 3600) / 60;
//                    tss = (tns % 60);
//                    frac = x / cur_stream->width;
//                    ns = frac * tns;
//                    hh = ns / 3600;
//                    mm = (ns % 3600) / 60;
//                    ss = (ns % 60);
//                    av_log(NULL, AV_LOG_INFO,
//                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n",
//                           frac * 100, hh, mm, ss, thh, tmm, tss);
//                    ts = frac * cur_stream->ic->duration;
//                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
//                        ts += cur_stream->ic->start_time;
//                    stream_seek(cur_stream, ts, 0, 0);
//                }
//                break;
            case FF_QUIT_EVENT:
                do_exit(cur_stream);
                break;
            case FF_ALLOC_EVENT:
//                alloc_picture((VideoState *) event.user.data1,
//                              (AVFrame *) event.user.data2);
                break;
            default:
                break;
        }
    }
}

static int opt_format(void *optctx, const char *opt, const char *arg) {
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_sync(void *optctx, const char *opt, const char *arg) {
    if (!strcmp(arg, "audio"))
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static void opt_input_file(void *optctx, const char *filename) {
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
               filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}

static int opt_codec(void *optctx, const char *opt, const char *arg) {
    const char *spec = strchr(opt, ':');
    if (!spec) {
        av_log(NULL, AV_LOG_ERROR,
               "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
        return AVERROR(EINVAL);
    }
    spec++;
    switch (spec[0]) {
        case 'a':
            audio_codec_name = arg;
            break;
        case 's':
            subtitle_codec_name = arg;
            break;
        case 'v':
            video_codec_name = arg;
            break;
        default:
            av_log(NULL, AV_LOG_ERROR,
                   "Invalid media specifier '%s' in option '%s'\n", spec, opt);
            return AVERROR(EINVAL);
    }
    return 0;
}

static int dummy;

static int lockmgr(void **mutex, enum AVLockOp op) {
    int ret = 0;
    switch (op) {
        case AV_LOCK_CREATE:
            *mutex = av_malloc(sizeof(pthread_mutex_t));
            if (!mutex)
                return AVERROR(ENOMEM);
            ret = pthread_mutex_init((pthread_mutex_t * )(*mutex), NULL);
            break;
        case AV_LOCK_OBTAIN:
            ret = pthread_mutex_lock((pthread_mutex_t * )(*mutex));
            break;
        case AV_LOCK_RELEASE:
            ret = pthread_mutex_unlock((pthread_mutex_t * )(*mutex));
            break;
        case AV_LOCK_DESTROY:
            ret = pthread_mutex_destroy((pthread_mutex_t * )(*mutex));
            av_freep(mutex);
            break;
    }
    return AVERROR(ret);
}

int main(int argc, char **argv) {
    ALOGD("main");
}

using namespace ffplayer;

FFPlayer::FFPlayer() { }

FFPlayer::~FFPlayer() {

}

void FFPlayer::start() {

}

void FFPlayer::pause() {

}

void FFPlayer::setDataSource(const char *path) {
    mPath = path;
}

void ffmpeg_log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    int prio = ANDROID_LOG_VERBOSE;
    if (level <= AV_LOG_ERROR)
        prio = ANDROID_LOG_ERROR;
    else if (level <= AV_LOG_WARNING)
        prio = ANDROID_LOG_WARN;
    else if (level <= AV_LOG_INFO)
        prio = ANDROID_LOG_INFO;
    else if (level <= AV_LOG_VERBOSE)
        prio = ANDROID_LOG_VERBOSE;
    else if (level <= AV_LOG_TRACE)
        prio = ANDROID_LOG_DEBUG;
    // TODO use log.h
    __android_log_vprint(prio, "ffmpeg", fmt, vl);
}


void FFPlayer::prepare() {
    ALOGV("main");
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_callback(ffmpeg_log_callback);

    ALOGV("main3");


    av_register_all();
    avformat_network_init();

    if (av_lockmgr_register(lockmgr)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize lock manager!\n");
        do_exit(NULL);
    }
    ALOGV("main4");
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t * ) & flush_pkt;

    is = stream_open(mPath.c_str(), NULL);
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }
    ALOGV("main5");
    is->window = mWindow;
    ALOGV("main6");
    pthread_t event_tid;
    pthread_create(&event_tid, NULL, event_loop, is);
    pthread_setname_np(event_tid, "event_thread");
}

void FFPlayer::release() {

}

void FFPlayer::seekTo(int64_t seekTimeUs) {

}

void FFPlayer::setWindow(ANativeWindow *window) {
    mWindow = window;
}

void FFPlayer::getDuration(int64_t *timeUs) {

}
