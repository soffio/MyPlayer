//
// Created by 马永斌 on 16/6/14.
//

#include "AudioEngine.h"
#include "log.h"
#include <stdlib.h>

#include <assert.h>

/* Number of decoded samples produced by one AAC frame; defined by the standard */
#define SAMPLES_PER_AAC_FRAME 1024

/* Size of the decoded PCM buffer queue */
#define NB_BUFFERS_IN_PCM_QUEUE 2  // 2 to 4 is typical
/* Size of each PCM buffer in the queue */
#define BUFFER_SIZE_IN_BYTES   (2*sizeof(short)*SAMPLES_PER_AAC_FRAME)

void AudioEngine::createEngine() {
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    ALOGD("bqPlayerCallback");
    AudioEngine *audioEngine = static_cast<AudioEngine *>(context);
    assert(NULL != context);
    assert(bq == audioEngine->bqPlayerBufferQueue);
    ALOGV("bufferSize=%d", audioEngine->outputBufferSize);
    // for streaming playback, replace this test by logic to find and fill the next buffer
    if (NULL != audioEngine->outputBuffer) {
        audioEngine->mCallback(audioEngine->mUserData, audioEngine->outputBuffer,
                               audioEngine->outputBufferSize);
        SLresult result;
        // enqueue another buffer
        result = (*audioEngine->bqPlayerBufferQueue)->Enqueue(audioEngine->bqPlayerBufferQueue,
                                                              audioEngine->outputBuffer,
                                                              audioEngine->outputBufferSize);
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        if (SL_RESULT_SUCCESS != result) {
            ALOGW("Enqueue fail");
        }
        (void) result;
    }
}

void AudioEngine::createBufferQueueAudioPlayer(int sampleRate, unsigned int channels,
                                               void *userData, void (*callback)(void *userdata,
                                                                                uint8_t *stream,
                                                                                int len)) {
    ALOGD("createBufferQueueAudioPlayer sampleRate=%d, channels=%d", sampleRate, channels);
    SLresult result;
    if (sampleRate >= 0) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
//        bqPlayerBufSize = outputBufferSize;
        // TODO use outputBufferSize
    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, channels, SL_SAMPLINGRATE_8,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if (bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    if (channels == 2)
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
    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, /*SL_IID_EFFECTSEND*/
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
                                                2, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, this);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    outputBufferSize = BUFFER_SIZE_IN_BYTES;
    outputBuffer = (uint8_t *) malloc(sizeof(uint8_t) * outputBufferSize);
    mUserData = userData;
    mCallback = callback;
}

void AudioEngine::shutdown() {
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

    if (outputBuffer != NULL)
        free(outputBuffer);

    mCallback = NULL;
    mUserData = NULL;

}

AudioEngine::AudioEngine() : engineObject(NULL), engineEngine(NULL), outputMixObject(NULL),
                             bqPlayerObject(NULL), bqPlayerPlay(NULL), bqPlayerBufferQueue(NULL),
                             bqPlayerMuteSolo(NULL), bqPlayerVolume(NULL), bqPlayerSampleRate(0),
                             bqPlayerBufSize(0), outputBufferSize(0), outputBuffer(NULL),
                             mUserData(NULL), mCallback(NULL) {

}

void AudioEngine::enqueueStartBuffer() {
    uint8_t *silent = (uint8_t *) malloc(sizeof(uint8_t) * outputBufferSize);
    memset(silent, 0, sizeof(uint8_t) * outputBufferSize);
    (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue,
                                    silent,
                                    outputBufferSize);
}
