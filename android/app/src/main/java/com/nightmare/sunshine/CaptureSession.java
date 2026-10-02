package com.nightmare.sunshine;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;

/** Resources belonging to one projection consent, including reconnects. */
public final class CaptureSession implements AutoCloseable {
    private final Context context;
    private final MediaProjection projection;
    private final MediaProjection.Callback callback;
    private VirtualDisplay display;
    private AudioRecord audio;
    private int audioSession = -1;
    private boolean closed;

    CaptureSession(Context context, MediaProjection projection, Runnable revoked) {
        this.context = context.getApplicationContext();
        this.projection = projection;
        callback = new MediaProjection.Callback() {
            @Override public void onStop() { revoked.run(); }
        };
        projection.registerCallback(callback, new Handler(Looper.getMainLooper()));
    }
    public int[] getGeometry() {
        DisplayManager manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        android.view.Display physical = manager.getDisplay(android.view.Display.DEFAULT_DISPLAY);
        if (physical == null) return null;
        android.graphics.Point size = new android.graphics.Point();
        physical.getRealSize(size);
        return new int[] { physical.getDisplayId(), size.x, size.y, physical.getRotation() };
    }

    public synchronized boolean createVirtualDisplay(int width, int height, Surface surface) {
        if (closed || width <= 0 || height <= 0 || surface == null || !surface.isValid()) return false;
        try {
            int density = context.getResources().getDisplayMetrics().densityDpi;
            if (display == null) {
                display = projection.createVirtualDisplay("sunshine", width, height, density,
                        DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null);
            } else {
                display.resize(width, height, density);
                display.setSurface(surface);
            }
            return display != null;
        } catch (RuntimeException e) {
            android.util.Log.e("CaptureSession", "Display unavailable", e);
            return false;
        }
    }

    public synchronized void detachDisplay() {
        if (display != null) {
            try { display.setSurface(null); } catch (RuntimeException ignored) { }
        }
    }

    public synchronized AudioRecord startPlaybackCapture(int sessionId, int packetDurationMs) {
        if (closed || Build.VERSION.SDK_INT < 29 || packetDurationMs <= 0 || packetDurationMs > 120
                || context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return null;
        if (audio != null) return audioSession == sessionId ? audio : null;
        AudioRecord candidate = null;
        try {
            int minimum = AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT);
            if (minimum <= 0) return null;
            candidate = new AudioRecord.Builder()
                    .setAudioPlaybackCaptureConfig(new AudioPlaybackCaptureConfiguration.Builder(projection)
                            .addMatchingUsage(AudioAttributes.USAGE_MEDIA).addMatchingUsage(AudioAttributes.USAGE_GAME)
                            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO).setEncoding(AudioFormat.ENCODING_PCM_FLOAT).build())
                    .setBufferSizeInBytes(Math.max(minimum * 2, 48 * packetDurationMs * 2 * 4 * 4)).build();
            if (candidate.getState() != AudioRecord.STATE_INITIALIZED) { candidate.release(); return null; }
            candidate.startRecording();
            if (candidate.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) { candidate.release(); return null; }
            audio = candidate;
            audioSession = sessionId;
            return audio;
        } catch (RuntimeException e) {
            if (candidate != null) candidate.release();
            android.util.Log.e("CaptureSession", "Playback capture unavailable", e);
            return null;
        }
    }

    /** Stops blocking reads; native reader joins before releasePlaybackCapture. */
    public synchronized void stopPlaybackCapture(int sessionId) {
        if (audio != null && (sessionId == audioSession || sessionId == -1)) {
            try { audio.stop(); } catch (IllegalStateException ignored) { }
        }
    }

    public synchronized void releasePlaybackCapture(int sessionId) {
        if (audio != null && (sessionId == audioSession || sessionId == -1)) {
            stopPlaybackCapture(sessionId);
            audio.release();
            audio = null;
            audioSession = -1;
        }
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        releasePlaybackCapture(-1);
        if (display != null) { display.release(); display = null; }
        projection.unregisterCallback(callback);
        projection.stop();
    }
}
