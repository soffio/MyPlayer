package com.leon.player;

import android.view.Surface;

/**
 * Created by mayongbin on 16/5/23.
 */
public class FFmpegPlayer {

    static {
        System.loadLibrary("ffmpegPlayer");
        native_init();

    }

    private long mNativePlayer;

    public FFmpegPlayer() {
        native_setup();
    }


    public void start() {
        native_start();
    }

    public void pause() {
        native_pause();
    }

    public void setDataSource(String dataSource) {
        native_setDataSource(dataSource);
    }

    public void prepare() {
        native_prepare();
    }

    public void release() {
        native_release();
    }

    public void seekTo(int millisecond) {
        native_seekTo(millisecond);
    }

    public void setSurface(Surface surface) {
        native_setSurface(surface);
    }

    public int getDuration() {
        return native_getDuration();
    }

    @Override
    protected void finalize() throws Throwable {
        native_finalize();
    }

    private static native void native_init();

    private native void native_setup();

    private native void native_start();

    private native void native_pause();

    private native void native_setDataSource(String dataSource);

    private native void native_release();

    private native void native_prepare();

    private native void native_seekTo(int millisecond);

    private native void native_setSurface(Surface surface);

    private native int native_getDuration();

    private native void native_finalize();
}
