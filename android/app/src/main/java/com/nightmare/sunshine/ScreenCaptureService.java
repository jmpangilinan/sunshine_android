package com.nightmare.sunshine;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.IBinder;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Surface;

import androidx.core.app.NotificationCompat;

import com.nightmare.sunshine_android.R;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class ScreenCaptureService extends Service {
    public enum State { IDLE, STARTING, RUNNING, STOPPING, FAILED }
    public interface Listener {
        void onHostState(State state, String reason);
        default void onHostStateRequest(State state, String reason, long requestGeneration) {
            onHostState(state, reason);
        }
    }
    public static final String ACTION_STOP = "com.nightmare.sunshine.STOP";
    private static final String CHANNEL_ID = "ScreenCaptureChannel";
    private static final android.os.Handler MAIN = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final java.util.concurrent.CopyOnWriteArraySet<Listener> LISTENERS = new java.util.concurrent.CopyOnWriteArraySet<>();
    private static volatile State state = State.IDLE;
    private static volatile String failureReason = "";
    private static volatile ScreenCaptureService instance;
    private static final java.util.concurrent.atomic.AtomicLong generation = new java.util.concurrent.atomic.AtomicLong();
    private static volatile long activeGeneration;
    public static long prepareStartRequest() { return generation.incrementAndGet(); }
    private CaptureSession capture;
    private volatile Thread hostThread;
    private final com.nightmare.sunshine.input.InputBackendManager.Listener inputListener =
            new com.nightmare.sunshine.input.InputBackendManager.Listener() {
                @Override public void onInputState(com.nightmare.sunshine.input.InputCapabilities capabilities) {
                    if (hostThread != null && !com.nightmare.sunshine.input.InputBackendManager.isReady()) {
                        publish(State.FAILED, capabilities.failureReason);
                        stopHost();
                    }
                }
                @Override public void onBackendDeath(String reason) {
                    if (hostThread != null) { publish(State.FAILED, reason); stopHost(); }
                }
            };
    public static State getState() { return state; }
    public static boolean isBusy() {
        ScreenCaptureService service = instance;
        return service != null && service.hostThread != null;
    }
    public static String getFailureReason() { return failureReason; }
    public static void addListener(Listener listener) {
        LISTENERS.add(listener);
        State snapshot = state;
        String reason = failureReason;
        long token = activeGeneration;
        MAIN.post(() -> listener.onHostStateRequest(snapshot, reason, token));
    }
    public static void removeListener(Listener listener) { LISTENERS.remove(listener); }
    private static void publish(State next, String reason) {
        state = next;
        failureReason = reason == null ? "" : reason;
        String message = failureReason;
        long token = activeGeneration;
        MAIN.post(() -> { for (Listener listener : LISTENERS) listener.onHostStateRequest(next, message, token); });
    }
    public static void reportConsentDenied() { publish(State.FAILED, "Screen capture consent declined"); }
    public static void requestStop(Context context) {
        generation.incrementAndGet();
        MAIN.post(() -> { ScreenCaptureService service = instance; if (service != null) service.stopHost(); });
    }
    public static void onNativeHostState(String nativeState, String reason) {
        MAIN.post(() -> {
            ScreenCaptureService service = instance;
            if (service == null) return;
            if ("RUNNING".equals(nativeState) && state == State.STARTING) publish(State.RUNNING, "");
            else if ("FAILED".equals(nativeState)) { publish(State.FAILED, reason); service.stopHost(); }
        });
    }
    @Override public void onCreate() {
        super.onCreate();
        instance = this;
        NativeBridge.init(getApplicationContext());
        com.nightmare.sunshine.input.InputBackendManager.addListener(inputListener);
        startMediaProjectionForeground();
    }
    private boolean requireInputReady() {
        if (com.nightmare.sunshine.input.InputBackendManager.isReady()) return true;
        publish(State.FAILED, com.nightmare.sunshine.input.InputBackendManager.getCapabilities().failureReason);
        stopSelf();
        return false;
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) { stopHost(); return START_NOT_STICKY; }
        if (hostThread != null) {
            long token = intent == null ? -1 : intent.getLongExtra("startGeneration", -1);
            MAIN.post(() -> { for (Listener listener : LISTENERS) listener.onHostStateRequest(State.FAILED, "HOST_BUSY: previous host is still active or stopping", token); });
            return START_NOT_STICKY;
        }
        if (intent == null) { stopSelf(); return START_NOT_STICKY; }
        if (intent.getLongExtra("startGeneration", -1) != generation.get()) {
            long token = intent.getLongExtra("startGeneration", -1);
            MAIN.post(() -> { for (Listener listener : LISTENERS) listener.onHostStateRequest(State.IDLE, "", token); });
            stopSelf(); return START_NOT_STICKY;
        }
        activeGeneration = intent.getLongExtra("startGeneration", -1);
        int resultCode = intent.getIntExtra("resultCode", Activity.RESULT_CANCELED);
        Intent data = intent.getParcelableExtra("data");
        if (resultCode != Activity.RESULT_OK || data == null) { reportConsentDenied(); stopSelf(); return START_NOT_STICKY; }
        if (!requireInputReady()) return START_NOT_STICKY;
        publish(State.STARTING, "");
        try {
            NativeBridge.setSunshineName(Build.MANUFACTURER + "-" + Build.MODEL);
            NativeBridge.setFileStatePath(getFilesDir().getAbsolutePath() + "/sunshine_state.json");
            writeCertAndKey(this);
            MediaProjectionManager manager = (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
            MediaProjection projection = manager.getMediaProjection(resultCode, data);
            if (projection == null) throw new IllegalStateException("Projection consent unavailable");
            capture = new CaptureSession(this, projection, this::stopHost);
            NativeBridge.setCaptureSession(capture);
            MDNSHelper.broadcastService(this);
            NativeBridge.prepareStart();
            hostThread = new Thread(() -> {
                try { NativeBridge.start(); }
                catch (Throwable error) { publish(State.FAILED, error.toString()); }
                finally {
                    CaptureSession owner = capture;
                    if (owner != null) owner.close();
                    NativeBridge.setCaptureSession(null);
                    MAIN.post(() -> {
                        capture = null;
                        hostThread = null;
                        if (state == State.FAILED) publish(State.FAILED, failureReason);
                        else publish(State.IDLE, "");
                        stopSelf();
                    });
                }
            }, "SunshineHost");
            hostThread.start();
        } catch (RuntimeException error) {
            if (capture != null) capture.close();
            NativeBridge.setCaptureSession(null);
            publish(State.FAILED, error.toString());
            stopSelf();
        }
        return START_NOT_STICKY;
    }
    private void stopHost() {
        if (hostThread == null) { stopSelf(); return; }
        if (state != State.FAILED) publish(State.STOPPING, "");
        if (capture != null) capture.stopPlaybackCapture(-1);
        NativeBridge.stop();
    }


    public static void writeCertAndKey(Context context) {
        try {
            // 写入证书文件
            try (InputStream certInput = context.getAssets().open("cacert.pem");
                 FileOutputStream certOutput = context.openFileOutput("cacert.pem", Context.MODE_PRIVATE)) {
                byte[] buffer = new byte[1024];
                int length;
                while ((length = certInput.read(buffer)) > 0) {
                    certOutput.write(buffer, 0, length);
                }
                NativeBridge.setCertPath(context.getFilesDir().getAbsolutePath() + "/cacert.pem");
            }

            // 写入密钥文件
            try (InputStream keyInput = context.getAssets().open("cakey.pem");
                 FileOutputStream keyOutput = context.openFileOutput("cakey.pem", Context.MODE_PRIVATE)) {
                byte[] buffer = new byte[1024];
                int length;
                while ((length = keyInput.read(buffer)) > 0) {
                    keyOutput.write(buffer, 0, length);
                }
                NativeBridge.setPkeyPath(context.getFilesDir().getAbsolutePath() + "/cakey.pem");
            }

            android.util.Log.i("TAG", "证书和密钥文件写入成功: " + context.getFilesDir().getAbsolutePath());
        } catch (IOException e) {
            throw new IllegalStateException("Unable to prepare host certificates", e);
        }
    }


    @Override public void onDestroy() {
        stopHost();
        com.nightmare.sunshine.input.InputBackendManager.removeListener(inputListener);
        if (hostThread == null && capture != null) capture.close();
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }


    private void startMediaProjectionForeground() {
        String channelId = "ScreenCaptureChannel";
        NotificationManager notificationManager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    channelId,
                    "Screen Capture Service",
                    NotificationManager.IMPORTANCE_LOW);  // Use LOW to avoid disturbing the user
            channel.setDescription("Screen capture service running");
            notificationManager.createNotificationChannel(channel);
        }
        // Build a proper notification with all required elements
        NotificationCompat.Builder notificationBuilder = new NotificationCompat.Builder(this, channelId)
                .setContentTitle("Screen Capture Active")
                .setContentText("Your screen is being captured")
                .setSmallIcon(R.mipmap.ic_launcher)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setOngoing(true);
        Notification notification = notificationBuilder.build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        } else {
            startForeground(1, notification);
        }
    }
}