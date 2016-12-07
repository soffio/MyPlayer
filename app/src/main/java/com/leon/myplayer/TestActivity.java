package com.leon.myplayer;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import com.leon.player.FFmpegPlayer;

public class TestActivity extends Activity implements SurfaceHolder.Callback {
    FFmpegPlayer mPlayer;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_test);
        SurfaceView surfaceView = (SurfaceView) findViewById(R.id.surface_view);
        surfaceView.getHolder().addCallback(this);

    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        mPlayer = new FFmpegPlayer();
        mPlayer.setDataSource("/sdcard/360.mp4");
        mPlayer.setSurface(holder.getSurface());
        mPlayer.prepare();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {

    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {

    }

    public void seekTest(View view) {
        mPlayer.seekTo(60);
    }

    @Override
    public void onBackPressed() {
        super.onBackPressed();
        if (mPlayer != null)
            mPlayer.release();
    }
}
