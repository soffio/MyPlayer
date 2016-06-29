//
// Created by 马永斌 on 16/6/12.
//
#include <jni.h>
#include <stdio.h>
#include <android/log.h>

#include "log.h"
#include <assert.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
}

#ifndef NELEM
#define NELEM(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

/* Number of decoded samples produced by one AAC frame; defined by the standard */
#define SAMPLES_PER_AAC_FRAME 1024

/* Size of the decoded PCM buffer queue */
#define NB_BUFFERS_IN_PCM_QUEUE 2  // 2 to 4 is typical
/* Size of each PCM buffer in the queue */
#define BUFFER_SIZE_IN_BYTES   (2*sizeof(short)*SAMPLES_PER_AAC_FRAME)

static int outputBufferSize;


static void *buffer;
static size_t bufferSize;

static uint8_t *outputBuffer;

static int sampleRate;

static int channel;

static AVFormatContext *fmt_ctx = NULL;
static AVPacket packet;
static AVCodecContext *pCodecCtx;
static int audioStream;
static AVFrame *aFrame;
static SwrContext *swr;

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static jint   bqPlayerBufSize = 0;


void createEngine() {
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_NULL};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

}


static void initFFmpeg(){
    ALOGD("BUFFER_SIZE_IN_BYTES:%d",BUFFER_SIZE_IN_BYTES);

    av_register_all();

    if (avformat_open_input(&fmt_ctx, "/sdcard/360.mp4", NULL, NULL) != 0) {
        ALOGE("avformat_open_input fail");
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        ALOGE("avformat_find_stream_info fail");
        return;
    }

    audioStream = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            audioStream < 0) {
            audioStream = i;
        }
    }

    if (audioStream == -1) {
        ALOGE("Couldn't find audio stream!");
    }


    pCodecCtx = fmt_ctx->streams[audioStream]->codec;
    AVCodec *aCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if (!aCodec) {
        ALOGE("Unsupported codec!");
        return ;
    }

    if (avcodec_open2(pCodecCtx, aCodec, NULL) < 0) {
        ALOGE("Could not open codec.");
        return; // Could not open codec
    }

    aFrame = av_frame_alloc();

    swr = swr_alloc();
    av_opt_set_int(swr, "in_channel_layout",  pCodecCtx->channel_layout, 0);
    av_opt_set_int(swr, "out_channel_layout", pCodecCtx->channel_layout,  0);
    av_opt_set_int(swr, "in_sample_rate",     pCodecCtx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate",    pCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",  pCodecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);
    swr_init(swr);

    outputBufferSize = 8192;
    outputBuffer = (uint8_t *) malloc(sizeof(uint8_t) * outputBufferSize);

    // 返回sample rate和channels
    sampleRate = pCodecCtx->sample_rate;
    channel = pCodecCtx->channels;


//    avformat_close_input(&fmt_ctx);
}

int getPCM(void **pcm, size_t *pcmSize) {
    ALOGD("getPcm");
    while (av_read_frame(fmt_ctx, &packet) >= 0) {

        int frameFinished = 0;
        // Is this a packet from the audio stream?
        if (packet.stream_index == audioStream) {
            avcodec_decode_audio4(pCodecCtx, aFrame, &frameFinished, &packet);

            if (frameFinished) {
                // data_size为音频数据所占的字节数
                int data_size = av_samples_get_buffer_size(
                        aFrame->linesize, pCodecCtx->channels,
                        aFrame->nb_samples, pCodecCtx->sample_fmt, 1);

                // 这里内存再分配可能存在问题
                if (data_size > outputBufferSize) {
                    outputBufferSize = data_size;
                    outputBuffer = (uint8_t *) realloc(outputBuffer,
                                                       sizeof(uint8_t) * outputBufferSize);
                }

                // 音频格式转换
                int len = swr_convert(swr, &outputBuffer, aFrame->nb_samples,
                            (uint8_t const **) (aFrame->extended_data),
                            aFrame->nb_samples);

                // 返回pcm数据
                *pcm = outputBuffer;
                *pcmSize = len * channel *av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
                return 0;
            }
        }
    }
    return -1;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    ALOGD("bqPlayerCallback");
    assert(bq == bqPlayerBufferQueue);
//    assert(NULL == context);
    bufferSize = 0;
    getPCM(&buffer, &bufferSize);
    ALOGV("bufferSize=%d",bufferSize);
    // for streaming playback, replace this test by logic to find and fill the next buffer
    if (NULL != buffer && 0 != bufferSize) {
        SLresult result;
        // enqueue another buffer
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, buffer, bufferSize);
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        if (SL_RESULT_SUCCESS != result) {
            ALOGW("Enqueue fail");
        }
        (void)result;
    }
}

static void createBufferQueueAudioPlayer() {
    SLresult result;
    if (sampleRate >= 0 && outputBufferSize >= 0 ) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
        bqPlayerBufSize = outputBufferSize;
    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, channel, SL_SAMPLINGRATE_8,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if(bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    if (channel == 2)
        format_pcm.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
    else
        format_pcm.channelMask = SL_SPEAKER_FRONT_CENTER;
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME,
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
                                                2, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    ALOGD("SetPlayState");
}

static void shutdown(){
    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
}

static void testFFmpeg(JNIEnv *env, jobject thiz) {

    initFFmpeg();

    createEngine();

    createBufferQueueAudioPlayer();

    bqPlayerCallback(bqPlayerBufferQueue,NULL);
//    (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, NULL, bufferSize);
}



static JNINativeMethod gMethods[]{
        {"testFFmpeg", "()V", (void *) testFFmpeg},
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4) != JNI_OK) {
        return -1;
    }
    jclass clazz;
    clazz = env->FindClass("com/leon/myapplication/MainActivity");
    if (clazz == NULL)
        return -1;
    if ((*env).RegisterNatives(clazz, gMethods, NELEM(gMethods)) != JNI_OK) {
        return -1;
    }
    // Get jclass with env->FindClass.
    // Register methods with env->RegisterNatives.

    return JNI_VERSION_1_4;
}