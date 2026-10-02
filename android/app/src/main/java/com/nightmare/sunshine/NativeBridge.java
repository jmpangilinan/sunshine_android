package com.nightmare.sunshine;

import android.content.Context;
import android.media.AudioRecord;
import android.view.Surface;

public final class NativeBridge {
    static { System.loadLibrary("sunshine"); }
    private static volatile CaptureSession capture;
    public static native void start();
    static native void prepareStart();
    public static native void stop();
    public static native void setSunshineName(String name);
    public static native void setPkeyPath(String path);
    public static native void setCertPath(String path);
    public static native void setFileStatePath(String path);
    public static native void submitPin(String pin);
    private static volatile VideoCapabilities videoCapabilities;
    public static VideoCapabilities queryVideoCapabilities() {
        VideoCapabilities snapshot = VideoCapabilities.query();
        videoCapabilities = snapshot;
        return snapshot;
    }
    public static String selectVideoEncoder(int format, int width, int height, int fps, int bitrateKbps) {
        VideoCapabilities snapshot = videoCapabilities;
        return snapshot == null ? null : snapshot.selectVideoEncoder(format, width, height, fps, bitrateKbps);
    }
    public static int profileForEncoder(String name, int format) {
        VideoCapabilities snapshot = videoCapabilities;
        return snapshot == null ? 0 : snapshot.profileForEncoder(name, format);
    }
    public static boolean lowLatencyForEncoder(String name) {
        VideoCapabilities snapshot = videoCapabilities;
        return snapshot != null && snapshot.lowLatencyForEncoder(name);
    }
    public static int[] getCaptureGeometry() {
        CaptureSession owner = capture;
        return owner == null ? null : owner.getGeometry();
    }
    public static void init(Context context) {
        com.nightmare.sunshine.input.InputBackendManager.initialize(context.getApplicationContext());
    }
    static void setCaptureSession(CaptureSession owner) { capture = owner; }
    private static volatile CaptureRenderer renderer;
    public static Surface prepareCaptureRenderer(Surface codec, int width, int height, int csc) throws Exception {
        CaptureRenderer next = CaptureRenderer.create(codec, width, height, csc);
        renderer = next;
        if (next == null) android.util.Log.w("SunshineCapture", "captureMode=directSurface nativeYUV unavailable API=" + android.os.Build.VERSION.SDK_INT);
        return next == null ? codec : next.surface;
    }
    public static boolean captureRendererHealthy() {
        CaptureRenderer current = renderer;
        return current == null || current.failure == null;
    }
    public static void stopCaptureRenderer() {
        CaptureRenderer current = renderer;
        renderer = null;
        if (current != null) current.close();
    }
    public static boolean createVirtualDisplay(int width, int height, Surface surface) {
        CaptureSession owner = capture;
        return owner != null && owner.createVirtualDisplay(width, height, surface);
    }
    public static void stopVirtualDisplay() {
        CaptureSession owner = capture;
        if (owner != null) owner.detachDisplay();
    }
    public static AudioRecord startPlaybackCapture(int sessionId, int packetDurationMs) {
        CaptureSession owner = capture;
        return owner == null ? null : owner.startPlaybackCapture(sessionId, packetDurationMs);
    }
    public static void stopPlaybackCapture(int sessionId) {
        CaptureSession owner = capture;
        if (owner != null) owner.stopPlaybackCapture(sessionId);
    }
    public static void releasePlaybackCapture(int sessionId) {
        CaptureSession owner = capture;
        if (owner != null) owner.releasePlaybackCapture(sessionId);
    }
    public static void onNativeHostState(String state, String reason) {
        ScreenCaptureService.onNativeHostState(state, reason);
    }
    static void onPinRequested() { }
}
