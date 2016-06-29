//
// Created by 马永斌 on 16/5/25.
//
#include "JNIHelp.h"
#include "log.h"

void jniThrowException(JNIEnv *env, const char *className, const char *msg) {
    jclass clazz = env->FindClass(className);
    if (!clazz) {
        ALOGE("Unable to find exception class %s", className);
        return;
    }

    if (env->ThrowNew(clazz, msg) != JNI_OK) {
        ALOGE("Failed throwing %s %s", className, msg);
    }
    env->DeleteLocalRef(clazz);
}