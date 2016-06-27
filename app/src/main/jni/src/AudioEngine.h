//
// Created by 马永斌 on 16/6/14.
//

#ifndef MYPLAYER_AUDIOENGINE_H
#define MYPLAYER_AUDIOENGINE_H


#include <jni.h>

#include <stdio.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

class AudioEngine {
private:
    // engine interfaces
    SLObjectItf engineObject;
    SLEngineItf engineEngine;

// output mix interfaces
    SLObjectItf outputMixObject;

// buffer queue player interfaces
    SLObjectItf bqPlayerObject;
    SLPlayItf bqPlayerPlay;
    SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
    SLMuteSoloItf bqPlayerMuteSolo;
    SLVolumeItf bqPlayerVolume;
    SLmilliHertz bqPlayerSampleRate;
    int bqPlayerBufSize;
    int outputBufferSize;
    uint8_t *outputBuffer;
    void *mUserData;
    void (*mCallback)(void *userdata,
                      uint8_t *stream,
                      int len);

public:
    AudioEngine();

    void createEngine();

    void createBufferQueueAudioPlayer(int sampleRate, int channels, void *userData,
                                      void (*callback)(void *userdata,
                                                       uint8_t *stream,
                                                       int len));

    void shutdown();

    void enqueueStartBuffer();

    friend void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context);
};


#endif //MYPLAYER_AUDIOENGINE_H
