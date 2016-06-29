#include <jni.h>
#include <stdio.h>


#include "JNIHelp.h"
#include "FFPlayer.h"
#include "log.h"
#include <pthread.h>
#include <android/native_window_jni.h>

using namespace ffplayer;
//
// Created by 马永斌 on 16/5/23.
//

struct java_fields {
    jfieldID playerReference;
};

static void setMediaPlayer(JNIEnv *pEnv, jobject pJobject, FFPlayer *pPlayer);

static java_fields fields;
static JavaField nativePlayer = {"mNativePlayer", "J"};

pthread_mutex_t mJniPlayerMutex = PTHREAD_MUTEX_INITIALIZER;


static void nativeInit(JNIEnv *env, jclass clazz) {
    ALOGD("nativeInit");
    fields.playerReference = env->GetFieldID(clazz, nativePlayer.name, nativePlayer.signature);
    if (fields.playerReference == NULL) {
        return;
    }
}

static void nativeSetup(JNIEnv *env, jobject thiz) {
    ALOGD("nativeSetup");
    FFPlayer *player = new FFPlayer();
    setMediaPlayer(env, thiz, player);
}

static void setMediaPlayer(JNIEnv *env, jobject thiz, FFPlayer *pPlayer) {
    pthread_mutex_lock(&mJniPlayerMutex);
    env->SetLongField(thiz, fields.playerReference, (jlong) pPlayer);
    pthread_mutex_unlock(&mJniPlayerMutex);
}

static FFPlayer *getMediaPlayer(JNIEnv *env, jobject thiz) {
    pthread_mutex_lock(&mJniPlayerMutex);
    FFPlayer *player = (FFPlayer *) env->GetLongField(thiz, fields.playerReference);
    pthread_mutex_unlock(&mJniPlayerMutex);
    return player;
}

static void nativeStart(JNIEnv *env, jobject thiz) {

}

static void nativePause(JNIEnv *env, jobject thiz) {

}

static void nativeSetDataSource(JNIEnv *env, jobject thiz, jstring jPath) {
    const char *path = env->GetStringUTFChars(jPath, NULL);
    if (path == NULL) // Out of memory
        return;
    ALOGD("nativeSetDataSource path %s", path);
    FFPlayer *player = getMediaPlayer(env, thiz);
    if (player == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    player->setDataSource(path);
    env->ReleaseStringUTFChars(jPath, path);

}

static void nativeRelease(JNIEnv *env, jobject thiz) {
    ALOGD("nativeRelease");
    FFPlayer *player = getMediaPlayer(env, thiz);
    // TODO use smart pointer
    delete player;
    setMediaPlayer(env, thiz, 0);
    // TODO setListener null
}

static void nativePrepare(JNIEnv *env, jobject thiz) {
    ALOGD("nativePrepare");
    FFPlayer *player = getMediaPlayer(env, thiz);
    if (player == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    player->prepare();
}

static void nativeSeekTo(JNIEnv *env, jobject thiz, jint msec) {
    ALOGD("nativeSeekTo %d",msec);
    FFPlayer *player = getMediaPlayer(env, thiz);
    if (player == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    player->seekTo(msec);
    ALOGD("nativeSeekTo done");
}

static void nativeSetSurface(JNIEnv *env, jobject thiz, jobject jsurface) {
    ALOGD("nativeSetSurface");
    FFPlayer *player = getMediaPlayer(env, thiz);
    if (player == NULL) {
        jniThrowException(env,"java/lang/IllegalStateException",NULL);
        return;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env,jsurface);
    ANativeWindow_acquire(window);
    player->setWindow(window);
}

static jint nativeGetDuration(JNIEnv *env, jobject thiz) {
    return 0;
}

static void nativeFinalize(JNIEnv *env, jobject thiz) {
    ALOGD("nativeFinalize");
    FFPlayer *player = getMediaPlayer(env, thiz);
    if (player != NULL) {
        ALOGW("MediaPlayer finalized without being released");
        nativeRelease(env, thiz);
    }
}

static JNINativeMethod gMethods[]{
        {"native_init",          "()V",                       (void *) nativeInit},
        {"native_setup",         "()V",                       (void *) nativeSetup},
        {"native_start",         "()V",                       (void *) nativeStart},
        {"native_pause",         "()V",                       (void *) nativePause},
        {"native_setDataSource", "(Ljava/lang/String;)V",     (void *) nativeSetDataSource},
        {"native_release",       "()V",                       (void *) nativeRelease},
        {"native_prepare",       "()V",                       (void *) nativePrepare},
        {"native_seekTo",        "(I)V",                      (void *) nativeSeekTo},
        {"native_setSurface",    "(Landroid/view/Surface;)V", (void *) nativeSetSurface},
        {"native_getDuration",   "()I",                       (void *) nativeGetDuration},
        {"native_finalize",      "()V",                       (void *) nativeFinalize},
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4) != JNI_OK) {
        return -1;
    }
    jclass clazz;
    clazz = env->FindClass("com/leon/player/FFmpegPlayer");
    if (clazz == NULL)
        return -1;
    if ((*env).RegisterNatives(clazz, gMethods, NELEM(gMethods)) != JNI_OK) {
        return -1;
    }
    // Get jclass with env->FindClass.
    // Register methods with env->RegisterNatives.

    return JNI_VERSION_1_4;
}

