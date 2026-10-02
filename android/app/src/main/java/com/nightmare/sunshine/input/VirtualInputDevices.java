package com.nightmare.sunshine.input;

import android.os.Process;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.HashSet;
import java.util.Set;

/** Worker-thread-only persistent kernel devices. FD creation alone is never readiness. */
public final class VirtualInputDevices implements AutoCloseable {
    private static final boolean LOADED;
    static {
        boolean loaded;
        try { System.loadLibrary("sunshine_input"); loaded = true; }
        catch (UnsatisfiedLinkError | SecurityException e) { loaded = false; }
        LOADED = loaded;
    }
    private long handle;
    private boolean mouseReady, keyboardReady;
    private int buttons;
    private String failure = "Virtual input native library unavailable";
    private final Set<Integer> controllers = new HashSet<>();

    public VirtualInputDevices() {
        if (LOADED) {
            handle = nativeOpen(Process.myUid());
            failure = nativeLastError(handle);
        }
    }
    public boolean openMouse() {
        if (mouseReady) return true;
        if (handle == 0 || (nativeCapabilities(handle) & 1) == 0) return false;
        InputDevice device = awaitDevice(nativeDeviceName(handle, 1));
        mouseReady = device != null && device.supportsSource(InputDevice.SOURCE_MOUSE);
        if (!mouseReady) failure = "Kernel mouse did not register as an Android mouse";
        return mouseReady;
    }
    public boolean openKeyboard() {
        if (keyboardReady) return true;
        if (handle == 0 || (nativeCapabilities(handle) & 2) == 0) return false;
        InputDevice device = awaitDevice(nativeDeviceName(handle, 2));
        keyboardReady = device != null && device.supportsSource(InputDevice.SOURCE_KEYBOARD)
                && allKeys(device, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_ENTER,
                           KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT);
        if (!keyboardReady) failure = "Kernel keyboard did not register with Android key mapping";
        return keyboardReady;
    }
    public boolean hasRelativeMouse() { return mouseReady && handle != 0; }
    public boolean hasKeyboard() { return keyboardReady && handle != 0; }
    public String failureReason() { return failure; }
    public boolean moveMouse(int dx, int dy) { return mouse(dx, dy, 0, 0); }
    public boolean mouseButton(int button, boolean release) {
        if (button < 1 || button > 5) return false;
        int previous = buttons;
        if (release) buttons &= ~(1 << (button - 1)); else buttons |= 1 << (button - 1);
        if (mouse(0, 0, 0, 0)) return true;
        buttons = previous;
        return false;
    }
    public boolean scroll(int vertical120, int horizontal120) { return mouse(0, 0, vertical120, horizontal120); }
    private boolean mouse(int dx, int dy, int vertical, int horizontal) {
        if (!hasRelativeMouse()) return false;
        boolean ok = nativeMouse(handle, dx, dy, buttons, vertical, horizontal);
        if (!ok) { failure = nativeLastError(handle); mouseReady = false; }
        return ok;
    }
    /** Linux evdev key code; Windows VK translation is performed by the ordered worker. */
    public boolean key(int linuxCode, boolean release) {
        if (!hasKeyboard()) return false;
        boolean ok = nativeKey(handle, linuxCode, release);
        if (!ok) failure = nativeLastError(handle);
        return ok;
    }
    public InputResult createController(int id) {
        if (id < 0 || id >= 16 || controllers.contains(id))
            return new InputResult(InputResult.INVALID_ARGUMENT, "Invalid or duplicate controller ID");
        if (handle == 0) return new InputResult(InputResult.DEVICE_UNAVAILABLE, failure);
        String name = nativeCreateController(handle, id);
        if (name == null) {
            failure = nativeLastError(handle);
            return new InputResult(InputResult.DEVICE_UNAVAILABLE, failure);
        }
        InputDevice device = awaitDevice(name);
        if (device == null || !validController(device)) {
            nativeRemoveController(handle, id);
            failure = "Virtual controller did not register with the required Android keys/axes; kernel or OEM mapping unavailable";
            return new InputResult(InputResult.DEVICE_UNAVAILABLE, failure);
        }
        controllers.add(id);
        return new InputResult(InputResult.OK, "Android controller registered: " + name);
    }
    public boolean controller(int id, int stateButtons, int lt, int rt, int lx, int ly, int rx, int ry) {
        if (handle == 0 || !controllers.contains(id)) return false;
        boolean ok = nativeController(handle, id, stateButtons, lt, rt, lx, ly, rx, ry);
        if (!ok) { failure = nativeLastError(handle); removeController(id); }
        return ok;
    }
    public void removeController(int id) {
        if (handle != 0) nativeRemoveController(handle, id);
        controllers.remove(id);
    }
    public void reset() {
        if (handle != 0) nativeReset(handle);
        buttons = 0;
    }
    @Override public void close() {
        if (handle != 0) { nativeClose(handle); handle = 0; }
        controllers.clear(); mouseReady = keyboardReady = false; buttons = 0;
    }
    private static InputDevice awaitDevice(String name) {
        if (name == null) return null;
        long deadline = SystemClock.uptimeMillis() + 1500;
        do {
            for (int id : InputDevice.getDeviceIds()) {
                InputDevice d = InputDevice.getDevice(id);
                if (d != null && name.equals(d.getName())) return d;
            }
            SystemClock.sleep(20);
        } while (SystemClock.uptimeMillis() < deadline);
        return null;
    }
    private static boolean allKeys(InputDevice d, int... keys) {
        for (boolean present : d.hasKeys(keys)) if (!present) return false;
        return true;
    }
    private static boolean axis(InputDevice d, int axis, float min, float max) {
        InputDevice.MotionRange r = d.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK);
        return r != null && Math.abs(r.getMin() - min) < 0.01f && Math.abs(r.getMax() - max) < 0.01f;
    }
    private static boolean validController(InputDevice d) {
        return d.supportsSource(InputDevice.SOURCE_GAMEPAD) && d.supportsSource(InputDevice.SOURCE_JOYSTICK)
            && allKeys(d, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
                KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
                KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
                KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
                KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_BUTTON_THUMBL,
                KeyEvent.KEYCODE_BUTTON_THUMBR)
            && axis(d, MotionEvent.AXIS_X, -1, 1) && axis(d, MotionEvent.AXIS_Y, -1, 1)
            && axis(d, MotionEvent.AXIS_Z, -1, 1) && axis(d, MotionEvent.AXIS_RZ, -1, 1)
            && axis(d, MotionEvent.AXIS_LTRIGGER, 0, 1) && axis(d, MotionEvent.AXIS_RTRIGGER, 0, 1)
            && axis(d, MotionEvent.AXIS_HAT_X, -1, 1) && axis(d, MotionEvent.AXIS_HAT_Y, -1, 1);
    }
    private static native long nativeOpen(int uid);
    private static native int nativeCapabilities(long handle);
    private static native String nativeLastError(long handle);
    private static native String nativeDeviceName(long handle, int kind);
    private static native String nativeCreateController(long handle, int id);
    private static native void nativeRemoveController(long handle, int id);
    private static native boolean nativeMouse(long handle, int dx, int dy, int buttons, int vertical120, int horizontal120);
    private static native boolean nativeKey(long handle, int linuxCode, boolean release);
    private static native boolean nativeController(long handle, int id, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    private static native void nativeReset(long handle);
    private static native void nativeClose(long handle);
}
