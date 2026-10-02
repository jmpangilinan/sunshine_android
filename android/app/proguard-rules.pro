-dontwarn org.slf4j.impl.StaticLoggerBinder
-keep class com.nightmare.sunshine.NativeBridge {
    *;
}
# JNI uses this return-type name and reads these fields by literal name.
# Keeping NativeBridge methods alone does not preserve their descriptor classes.
-keep class com.nightmare.sunshine.VideoCapabilities {
    public final boolean avc;
    public final boolean hevc;
}
-keep class com.nightmare.sunshine.input.InputBackendManager { *; }
-keep class com.nightmare.sunshine.input.ShizukuInputService { *; }
-keep class com.nightmare.sunshine.input.RootInputService { *; }
-keep class com.nightmare.sunshine.input.VirtualInputDevices { *; }
-keep class com.nightmare.sunshine.input.InputEventData { *; }
-keep class com.nightmare.sunshine.input.InputCapabilities { *; }
-keep class com.nightmare.sunshine.input.InputResult { *; }
-keep class com.nightmare.sunshine.CaptureRenderer { *; }
-keep class com.nightmare.sunshine.CaptureRenderer$Api33 { *; }