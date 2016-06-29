//
// Created by 马永斌 on 16/5/23.
//

#ifndef MYPLAYER_JHIHELP_H
#define MYPLAYER_JHIHELP_H

#include <jni.h>

#ifndef NELEM
# define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))
#endif

struct JavaField {
    const char *name;
    const char *signature;
};

void jniThrowException(JNIEnv *env, const char *className, const char *msg);

#endif //MYPLAYER_JHIHELP_H
