#include <stdio.h>

#include <jni.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
//#include <JNIHelp.h>
#include <android/log.h>

#include "encodedetect/autodetect.h"

#define LOG_TAG "Encodedetect"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

struct fields_t {
    jfieldID    sinkplayer;
};
static fields_t fields;


static
void startSink(JNIEnv* env, jobject thiz, jstring host, jint port) {

}

static JNINativeMethod nativeMethods[] = {
    // {"native_possibleEncoding", "([B)Ljava/lang/String;", (void*)possibleEncoding}
    {"native_startSink", "(Ljava/lang/String;I)V", (void*)startSink}
};


int registerNativeMethods_Encode(JNIEnv* env) {
    int result = -1;
    jclass clazz = env->FindClass("com/ivy/appshare/engin/im/simpleimp/util/EncodeDetector");

    if (NULL != clazz) {
        if (env->RegisterNatives(clazz, nativeMethods, sizeof(nativeMethods)
                / sizeof(nativeMethods[0])) == JNI_OK) {
            result = 0;
        }
    }

    return result;
}
