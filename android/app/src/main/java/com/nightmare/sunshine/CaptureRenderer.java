package com.nightmare.sunshine;

import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import java.util.concurrent.FutureTask;
import java.util.concurrent.atomic.AtomicBoolean;

/** One GL owner and latest-frame admission; codec draining remains on the native capture worker. */
class CaptureRenderer {
    private static final String TAG = "SunshineCapture";
    static CaptureRenderer create(Surface codec, int width, int height, int csc) throws Exception {
        if (Build.VERSION.SDK_INT < 33) return null;
        Api33 renderer = new Api33();
        try {
            renderer.call(() -> { renderer.initialize(codec, width, height, csc); return null; });
            return renderer;
        } catch (Exception failure) {
            renderer.close();
            if (failure instanceof UnsupportedOperationException || failure instanceof IllegalArgumentException) {
                Log.w(TAG, "nativeYUV unsupported; explicit directSurface fallback", failure);
                return null;
            }
            throw failure;
        }
    }
    Surface surface;
    volatile Throwable failure;
    void close() { }

    // Isolate API33 verification references from devices running minSdk26.
    private static final class Api33 extends CaptureRenderer {
        final HandlerThread thread = new HandlerThread("sunshine-nativeYUV");
        final Handler handler;
        final AtomicBoolean cancelled = new AtomicBoolean();
        final float[] transform = new float[16];
        android.media.ImageWriter writer;
        SurfaceTexture source;
        long nativeHandle;
        int inFlight;
        boolean pending;
        boolean renderScheduled;
        final Runnable renderTask = () -> { renderScheduled = false; renderLatest(); };
        void scheduleLatest() {
            if (!renderScheduled && pending && inFlight < 2 && !cancelled.get()) {
                renderScheduled = true;
                handler.post(renderTask);
            }
        }
        long frames, notifications, ageSum, ageMax, previousTimestamp, cadenceSum, cadenceCount;
        long reportAt = System.nanoTime();
        Api33() { thread.start(); handler = new Handler(thread.getLooper()); }
        <T> T call(java.util.concurrent.Callable<T> operation) throws Exception {
            FutureTask<T> task = new FutureTask<>(operation);
            if (!handler.post(task)) throw new IllegalStateException("Renderer owner stopped");
            boolean interrupted = false;
            try {
                for (;;) {
                    try { return task.get(2, java.util.concurrent.TimeUnit.SECONDS); }
                    catch (java.util.concurrent.TimeoutException timeout) {
                        Log.w(TAG, "nativeYUV owner stalled >2s in public/vendor operation; retaining resources until owner exits");
                    } catch (InterruptedException interruption) { interrupted = true; }
                    catch (java.util.concurrent.ExecutionException e) {
                        Throwable cause = e.getCause();
                        if (cause instanceof Exception) throw (Exception) cause;
                        throw new IllegalStateException(cause);
                    }
                }
            } finally { if (interrupted) Thread.currentThread().interrupt(); }
        }
        void initialize(Surface codec, int width, int height, int csc) throws java.io.IOException {
            nativeHandle = nativeCreate(csc);
            source = new SurfaceTexture(nativeTexture(nativeHandle));
            source.setDefaultBufferSize(width, height);
            surface = new Surface(source);
            writer = new android.media.ImageWriter.Builder(codec).setMaxImages(2)
                    .setWidthAndHeight(width, height)
                    .setUsage(android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT |
                              android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE |
                              android.hardware.HardwareBuffer.USAGE_VIDEO_ENCODE).build();
            // Keep the codec's default format AND dataspace. Forced YCBCR_420_888 is not equivalent.
            try (android.media.Image probe = writer.dequeueInputImage()) {
                acquire(probe);
                try (android.hardware.HardwareBuffer buffer = probe.getHardwareBuffer()) {
                    nativeRender(nativeHandle, buffer, transform, true);
                }
            }
            writer.setOnImageReleasedListener(ignored -> {
                if (cancelled.get()) return;
                if (inFlight > 0) --inFlight;
                scheduleLatest();
            }, handler);
            source.setOnFrameAvailableListener(ignored -> {
                if (cancelled.get()) return;
                ++notifications;
                pending = true;
                scheduleLatest();
            }, handler);
            Log.i(TAG, "captureMode=nativeYUV buffering=2 latestOnly=true defaultCodecFormat=true");
        }
        void acquire(android.media.Image image) throws java.io.IOException {
            try (android.hardware.SyncFence fence = image.getFence()) {
                if (fence.isValid() && !fence.await(java.time.Duration.ofMillis(100)))
                    throw new IllegalStateException("nativeYUV acquisition fence timeout");
            }
            if (cancelled.get()) throw new IllegalStateException("Renderer cancelled");
        }
        void renderLatest() {
            if (!pending || inFlight >= 2 || cancelled.get() || failure != null) return;
            android.media.Image image = null;
            try {
                pending = false;
                source.updateTexImage();
                source.getTransformMatrix(transform);
                long timestamp = source.getTimestamp();
                if (timestamp <= 0 || timestamp <= previousTimestamp) return;
                image = writer.dequeueInputImage(); // At most two queued images, released callbacks admit reuse.
                acquire(image);
                try (android.hardware.HardwareBuffer buffer = image.getHardwareBuffer()) {
                    nativeRender(nativeHandle, buffer, transform, false);
                }
                if (cancelled.get()) return;
                image.setTimestamp(timestamp); // Preserve SurfaceTexture's original monotonic producer timestamp.
                ++inFlight;
                writer.queueInputImage(image);
                image = null;
                long now = System.nanoTime(), age = now - timestamp;
                if (age >= 0) { ageSum += age; ageMax = Math.max(ageMax, age); }
                if (previousTimestamp != 0) { cadenceSum += timestamp - previousTimestamp; ++cadenceCount; }
                previousTimestamp = timestamp;
                ++frames;
                if (now - reportAt >= 5_000_000_000L) {
                    Log.i(TAG, "nativeYUV frames=" + frames + " callbacks=" + notifications + " inFlight=" + inFlight
                            + " ageMeanUs=" + (frames == 0 ? 0 : ageSum / frames / 1000)
                            + " ageMaxUs=" + ageMax / 1000 + " cadenceMeanUs="
                            + (cadenceCount == 0 ? 0 : cadenceSum / cadenceCount / 1000));
                    frames = notifications = ageSum = ageMax = cadenceSum = cadenceCount = 0;
                    reportAt = now;
                }
            } catch (Throwable error) {
                if (!cancelled.get()) { failure = error; Log.e(TAG, "nativeYUV failed; no runtime fallback", error); }
            } finally { if (image != null) image.close(); }
        }
        @Override void close() {
            if (!cancelled.compareAndSet(false, true)) return;
            if (nativeHandle != 0) nativeCancel(nativeHandle);
            try {
                call(() -> {
                    if (source != null) source.setOnFrameAvailableListener(null);
                    if (writer != null) { writer.setOnImageReleasedListener(null, null); writer.close(); writer = null; }
                    if (surface != null) { surface.release(); surface = null; }
                    if (source != null) { source.release(); source = null; }
                    if (nativeHandle != 0) { nativeDestroy(nativeHandle); nativeHandle = 0; }
                    return null;
                });
            } catch (Exception error) { Log.e(TAG, "Renderer cleanup failed", error); }
            thread.quitSafely();
            try { thread.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        private static native long nativeCreate(int csc);
        private static native int nativeTexture(long handle);
        private static native void nativeRender(long handle, android.hardware.HardwareBuffer buffer, float[] transform, boolean probe);
        private static native void nativeCancel(long handle);
        private static native void nativeDestroy(long handle);
    }
}
