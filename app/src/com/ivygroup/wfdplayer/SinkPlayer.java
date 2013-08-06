package com.ivygroup.wfdplayer;

import android.view.Surface;
import android.view.SurfaceHolder;

public class SinkPlayer {
    private SurfaceHolder mSurfaceHolder;

    private int mNativeSinkPlayer;  //accessed by native methods
    private int mNativeSurfaceTexture;  // accessed by native methods

    public SinkPlayer() {
    }

    public void release() {
        _release();
    }

    public void setDisplay(SurfaceHolder sh) {
        mSurfaceHolder = sh;
        Surface surface;
        if (sh != null) {
            surface = sh.getSurface();
        } else {
            surface = null;
        }
        _setVideoSurface(surface);
        updateSurfaceScreenOn();
    }

    private void updateSurfaceScreenOn() {
        if (mSurfaceHolder != null) {
            mSurfaceHolder.setKeepScreenOn(true);   // TODO;
        }
    }

    private native static void native_init();
    private native void _release();
    private native void _setVideoSurface(Surface surface);
    public native void native_startSink(String host, int port);

    static {
        System.loadLibrary("wfd");
        System.loadLibrary("wfd_jni");
        native_init();
    }
}
