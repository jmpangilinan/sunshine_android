package com.nightmare.sunshine;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;

import com.nightmare.sunshine.input.InputBackendManager;
import com.nightmare.sunshine.input.InputCapabilities;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

import io.flutter.embedding.android.FlutterActivity;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;

public class MainActivity extends FlutterActivity {
    private static final int CAPTURE_REQUEST = 1000;
    private static final int PERMISSION_REQUEST = 1001;
    private MethodChannel channel;
    private MethodChannel.Result pendingStart;
    private boolean awaitingConsent;
    private boolean awaitingPermissions;
    private long pendingStartGeneration = -1;
    private boolean destroyed;

    private final ScreenCaptureService.Listener hostListener = new ScreenCaptureService.Listener() {
        @Override
        public void onHostState(ScreenCaptureService.State state, String reason) {
            onHostStateRequest(state, reason, -1);
        }

        @Override
        public void onHostStateRequest(ScreenCaptureService.State state, String reason, long generation) {
            if (destroyed) return;
            Map<String, Object> snapshot = hostState();
            snapshot.put("state", state.name());
            snapshot.put("reason", reason == null ? "" : reason);
            channel.invokeMethod("hostState", snapshot);
            if (pendingStart == null || pendingStartGeneration < 0
                    || generation != pendingStartGeneration) return;
            if (state == ScreenCaptureService.State.RUNNING) finishStart(null, null);
            else if (state == ScreenCaptureService.State.FAILED) finishStart("HOST_FAILED", reason);
            else if (state == ScreenCaptureService.State.IDLE) {
                finishStart("START_CANCELLED", "Host stopped before startup completed");
            }
        }
    };
    private final InputBackendManager.Listener inputListener = new InputBackendManager.Listener() {
        @Override
        public void onInputState(InputCapabilities capabilities) {
            if (!destroyed) channel.invokeMethod("inputState", inputState(capabilities));
        }

        @Override
        public void onBackendDeath(String reason) {
            ScreenCaptureService.requestStop(getApplicationContext());
            if (!destroyed) {
                finishStart("INPUT_BACKEND_DIED", reason);
                channel.invokeMethod("inputState", inputState(InputBackendManager.getCapabilities()));
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Context app = getApplicationContext();
        NativeBridge.init(app);
        InputBackendManager.initialize(app);
        channel = new MethodChannel(getFlutterEngine().getDartExecutor().getBinaryMessenger(),
                "com.nightmare.sunshine");
        channel.setMethodCallHandler(this::onMethodCall);
        ScreenCaptureService.addListener(hostListener);
        InputBackendManager.addListener(inputListener);
    }

    private void onMethodCall(MethodCall call, MethodChannel.Result result) {
        switch (call.method) {
            case "start":
                ScreenCaptureService.State state = ScreenCaptureService.getState();
                if (pendingStart != null || awaitingConsent || awaitingPermissions
                        || ScreenCaptureService.isBusy()
                        || state == ScreenCaptureService.State.STARTING
                        || state == ScreenCaptureService.State.RUNNING
                        || state == ScreenCaptureService.State.STOPPING) {
                    result.error("HOST_BUSY", "Host is already starting, running or stopping", null);
                } else if (!InputBackendManager.isReady()) {
                    result.error("INPUT_NOT_READY", "Grant control access before starting the host", null);
                } else {
                    pendingStart = result;
                    pendingStartGeneration = -1;
                    requestStartPermissions();
                }
                break;
            case "stop":
                finishStart("START_CANCELLED", "Startup cancelled by Stop");
                ScreenCaptureService.requestStop(getApplicationContext());
                result.success(null);
                break;
            case "getHostState":
                result.success(hostState());
                break;
            case "setInputMode":
                if (!(call.arguments instanceof Number)) {
                    result.error("INVALID_ARGUMENT", "Input mode must be ROOT (1) or SHIZUKU (2)", null);
                    break;
                }
                int mode = ((Number) call.arguments).intValue();
                if (mode != 1 && mode != 2) {
                    result.error("INVALID_ARGUMENT", "Unknown input mode", null);
                } else if (pendingStart != null || awaitingConsent || awaitingPermissions
                        || isHostActive()) {
                    result.error("HOST_BUSY", "Stop the host before changing control mode", null);
                } else {
                    InputBackendManager.setMode(mode);
                    channel.invokeMethod("inputState", inputState(InputBackendManager.getCapabilities()));
                    result.success(null);
                }
                break;
            case "requestInputAccess":
                if (pendingStart != null || awaitingConsent || awaitingPermissions || isHostActive()) {
                    result.error("HOST_BUSY", "Stop the host before reconnecting control access", null);
                } else {
                    InputBackendManager.requestAccess();
                    result.success(null); // Readiness arrives only through inputState.
                }
                break;
            case "pin":
                if (!(call.arguments instanceof String)) {
                    result.error("INVALID_ARGUMENT", "PIN must be a string", null);
                } else {
                    NativeBridge.submitPin((String) call.arguments);
                    result.success(call.arguments);
                }
                break;
            case "address":
                result.success(MDNSHelper.getWifiIpAddress(getApplicationContext()));
                break;
            default:
                result.notImplemented();
        }
    }

    private boolean isHostActive() {
        ScreenCaptureService.State state = ScreenCaptureService.getState();
        return state == ScreenCaptureService.State.STARTING
                || state == ScreenCaptureService.State.RUNNING
                || state == ScreenCaptureService.State.STOPPING
                || ScreenCaptureService.isBusy();
    }

    private void requestStartPermissions() {
        ArrayList<String> permissions = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 29
                && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.RECORD_AUDIO);
        }
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (permissions.isEmpty()) requestCapture();
        else {
            awaitingPermissions = true;
            try {
                requestPermissions(permissions.toArray(new String[0]), PERMISSION_REQUEST);
            } catch (RuntimeException e) {
                awaitingPermissions = false;
                finishStart("PERMISSION_REQUEST_FAILED", e.getMessage());
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST) {
            awaitingPermissions = false;
            // Audio denial permits video-only capture; notifications are not a foreground-service prerequisite.
            if (pendingStart != null) requestCapture();
        }
    }

    private void requestCapture() {
        if (pendingStart == null) return;
        try {
            MediaProjectionManager manager = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            if (manager == null) throw new IllegalStateException("Screen capture unavailable");
            awaitingConsent = true;
            startActivityForResult(manager.createScreenCaptureIntent(), CAPTURE_REQUEST);
        } catch (RuntimeException e) {
            awaitingConsent = false;
            finishStart("CAPTURE_UNAVAILABLE", e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != CAPTURE_REQUEST) return;
        awaitingConsent = false;
        if (pendingStart == null) return; // Stop/teardown invalidates a late consent result.
        if (resultCode != Activity.RESULT_OK || data == null) {
            ScreenCaptureService.reportConsentDenied();
            finishStart("CONSENT_DENIED", "Screen capture consent declined");
            return;
        }
        if (!InputBackendManager.isReady()) {
            finishStart("INPUT_NOT_READY", "Control access was lost during screen capture consent");
            return;
        }
        Intent serviceIntent = new Intent(getApplicationContext(), ScreenCaptureService.class);
        serviceIntent.putExtra("resultCode", resultCode);
        serviceIntent.putExtra("data", data);
        try {
            pendingStartGeneration = ScreenCaptureService.prepareStartRequest();
            serviceIntent.putExtra("startGeneration", pendingStartGeneration);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(serviceIntent);
            else startService(serviceIntent);
        } catch (RuntimeException e) {
            finishStart("START_FAILED", e.getMessage());
        }
    }

    private void finishStart(String code, String message) {
        MethodChannel.Result result = pendingStart;
        pendingStart = null;
        pendingStartGeneration = -1;
        if (result == null) return;
        if (code == null) result.success(hostState());
        else result.error(code, message == null ? code : message, null);
    }

    private Map<String, Object> hostState() {
        Map<String, Object> map = new HashMap<>();
        map.put("state", ScreenCaptureService.getState().name());
        map.put("reason", ScreenCaptureService.getFailureReason());
        map.put("busy", ScreenCaptureService.isBusy());
        map.put("audioAvailable", Build.VERSION.SDK_INT >= 29
                && checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED);
        map.put("input", inputState(InputBackendManager.getCapabilities()));
        return map;
    }

    private Map<String, Object> inputState(InputCapabilities c) {
        Map<String, Object> map = new HashMap<>();
        map.put("mode", InputBackendManager.getMode());
        map.put("ready", InputBackendManager.isReady());
        if (c != null) {
            map.put("uid", c.uid);
            map.put("backendKind", c.backendKind);
            map.put("supportedEventMask", c.supportedEventMask);
            map.put("maxControllers", c.maxControllers);
            map.put("relativeMouse", c.relativeMouse);
            map.put("multitouch", c.multitouch);
            map.put("pen", c.pen);
            map.put("mappedText", c.mappedText);
            map.put("controllerRumble", c.controllerRumble);
            map.put("controllerMotion", c.controllerMotion);
            map.put("controllerTouch", c.controllerTouch);
            map.put("controllerBattery", c.controllerBattery);
            map.put("failureReason", c.failureReason);
        }
        return map;
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        if (pendingStart != null) {
            finishStart("ACTIVITY_DESTROYED", "Activity closed during startup; request fresh capture consent");
            ScreenCaptureService.requestStop(getApplicationContext());
        }
        ScreenCaptureService.removeListener(hostListener);
        InputBackendManager.removeListener(inputListener);
        if (channel != null) channel.setMethodCallHandler(null);
        super.onDestroy();
    }
}
