#define LOG_NDEBUG 0
#define LOG_TAG "ivygroup_wfdplayer_sinkplayer"

#include <stdio.h>
#include <jni.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
//#include <JNIHelp.h>
#include <android/log.h>

#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <utils/threads.h>
#include "jni.h"
#include "JNIHelp.h"
#include "android_runtime/AndroidRuntime.h"
#include "android_runtime/android_view_Surface.h"
#include "utils/Errors.h"  // for status_t
#include "utils/KeyedVector.h"
#include "utils/String8.h"
#include "foundation/AString.h"

#include <gui/ISurfaceTexture.h>
#include <gui/Surface.h>

#include "SinkPlayer.h"


using namespace android;


struct fields_t {
    jfieldID    native_sinkplayer;
    jfieldID    native_surfacetexture;
};
static fields_t fields;
static Mutex sLock;

static void setPlayer(JNIEnv* env, jobject thiz, const sp<SinkPlayer>& player) {
    Mutex::Autolock l(sLock);
    sp<SinkPlayer> old = (SinkPlayer*)env->GetIntField(thiz, fields.native_sinkplayer);
    if (old != 0) {
        old->decStrong(thiz);
    }
    env->SetIntField(thiz, fields.native_sinkplayer, (int)player.get());
}

static sp<SinkPlayer> getPlayer_l(JNIEnv* env, jobject thiz) {
    Mutex::Autolock l(sLock);
    SinkPlayer* const p = (SinkPlayer*)env->GetIntField(thiz, fields.native_sinkplayer);
    return sp<SinkPlayer>(p);
}

static sp<SinkPlayer> getPlayer(JNIEnv* env, jobject thiz) {
    sp<SinkPlayer> mp = getPlayer_l(env, thiz);
    if (mp == NULL) {
        //
        sp<SinkPlayer> player = new SinkPlayer();
        if (player.get()) {
            player->incStrong(thiz);
        }

        setPlayer(env, thiz, player);
        return player;

    } else {
        return mp;
    }
}

static void
ivygroup_wfdplayer_sinkplayer_native_init(JNIEnv *env) {
    jclass clazz;

    clazz = env->FindClass("com/ivygroup/wfdplayer/SinkPlayer");
    if (clazz == NULL) {
        return;
    }

    fields.native_sinkplayer = env->GetFieldID(clazz, "mNativeSinkPlayer", "I");
    if (fields.native_sinkplayer == NULL) {
        return;
    }

    fields.native_surfacetexture = env->GetFieldID(clazz, "mNativeSurfaceTexture", "I");
    if (fields.native_surfacetexture == NULL) {
        return;
    }
}

static void
ivygroup_wfdplayer_sinkplayer_release(JNIEnv *env, jobject thiz) {
    setPlayer(env, thiz, 0);
}

static sp<ISurfaceTexture>
getVideoSurfaceTexture(JNIEnv* env, jobject thiz) {
    ISurfaceTexture * const p = (ISurfaceTexture*)env->GetIntField(thiz, fields.native_surfacetexture);
    return sp<ISurfaceTexture>(p);
}

static void
decVideoSurfaceRef(JNIEnv *env, jobject thiz)
{
    sp<SinkPlayer> mp = getPlayer(env, thiz);
    if (mp == NULL) {
        return;
    }

    sp<ISurfaceTexture> old_st = getVideoSurfaceTexture(env, thiz);
    if (old_st != NULL) {
        old_st->decStrong((void*)decVideoSurfaceRef);
    }
}

static void
setVideoSurface(JNIEnv *env, jobject thiz, jobject jsurface, jboolean mediaPlayerMustBeAlive)
{
    sp<SinkPlayer> mp = getPlayer(env, thiz);
    if (mp == NULL) {
        if (mediaPlayerMustBeAlive) {
            jniThrowException(env, "java/lang/IllegalStateException", NULL);
        }
        return;
    }

    decVideoSurfaceRef(env, thiz);

    sp<ISurfaceTexture> new_st;
    if (jsurface) {
        sp<Surface> surface(android_view_Surface_getSurface(env, jsurface));
        if (surface != NULL) {
            new_st = surface->getSurfaceTexture();
            if (new_st == NULL) {

                jniThrowException(env, "java/lang/IllegalArgumentException",
                    "The surface does not have a binding SurfaceTexture!");
                return;
            }
            new_st->incStrong((void*)decVideoSurfaceRef);

        } else {
            jniThrowException(env, "java/lang/IllegalArgumentException",
                    "The surface has been released");
            return;
        }
    }


    env->SetIntField(thiz, fields.native_surfacetexture, (int)new_st.get());

    // This will fail if the media player has not been initialized yet. This
    // can be the case if setDisplay() on MediaPlayer.java has been called
    // before setDataSource(). The redundant call to setVideoSurfaceTexture()
    // in prepare/prepareAsync covers for this case.
    mp->setSurfaceTexture(new_st);
}

static void
android_media_MediaPlayer_setVideoSurface(JNIEnv *env, jobject thiz, jobject jsurface)
{
    setVideoSurface(env, thiz, jsurface, true /* mediaPlayerMustBeAlive */);
}

static void
ivygroup_wfdplayer_sinkplayer_startSink(JNIEnv* env, jobject thiz, jstring host, jint port) {
    if (host == NULL) {
        jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
        return;
    }

    const char *tmp = env->GetStringUTFChars(host, NULL);
    if (tmp == NULL) {  // Out of memory
        return;
    }

    AString hostStr(tmp);
    env->ReleaseStringUTFChars(host, tmp);
    tmp = NULL;

    sp<SinkPlayer> p = getPlayer(env, thiz);
    p->start(hostStr.c_str(), port);
}

static JNINativeMethod nativeMethods[] = {
    // {"native_possibleEncoding", "([B)Ljava/lang/String;", (void*)possibleEncoding}
    {"native_init", "()V", (void *)ivygroup_wfdplayer_sinkplayer_native_init},
    {"_release", "()V", (void *)ivygroup_wfdplayer_sinkplayer_release},
    {"_setVideoSurface", "(Landroid/view/Surface;)V", (void *)android_media_MediaPlayer_setVideoSurface},
    {"native_startSink", "(Ljava/lang/String;I)V", (void*)ivygroup_wfdplayer_sinkplayer_startSink}
};


int registerNativeMethods(JNIEnv* env) {
    int result = -1;
    jclass clazz = env->FindClass("com/ivygroup/wfdplayer/SinkPlayer");

    if (NULL != clazz) {
        if (env->RegisterNatives(clazz, nativeMethods, sizeof(nativeMethods)
                / sizeof(nativeMethods[0])) == JNI_OK) {
            result = 0;
        }
    }

    return result;
}


jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env = NULL;
    jint result = -1;

    if (vm->GetEnv((void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        return result;
    }

    if (env == NULL) {
        return result;
    }
    if (registerNativeMethods(env) != 0) {
        return result;
    }

    result = JNI_VERSION_1_4;
    return result;
}
