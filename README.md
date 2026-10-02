# sunshine_android

## Provenance

**移植的源代码 / Original extracted source**

- https://gitee.com/connect-screen/connect-screen#https://gitee.com/link?target=https%3A%2F%2Fgithub.com%2FMagicianGuo%2FAndroid-SettingTools

This project extracts the Android Sunshine host from 屏易联 (Connect Screen), so an Android device can serve a [Moonlight](https://moonlight-stream.org/) client. Original Android project: [nightmare-space/sunshine_android](https://github.com/nightmare-space/sunshine_android); update fork: [jmpangilinan/sunshine_android](https://github.com/jmpangilinan/sunshine_android).

The native host builds the copied sources in `android/app/src/main/cpp`. Its recorded Sunshine baseline is `6efd41d3f7f1fab263fe56adc86e2d27e89039cc`; that records provenance, not byte-for-byte upstream parity. This update selectively integrates Android-compatible changes, including upstream input ordering/extended-key fixes ([#5818](https://github.com/LizardByte/Sunshine/pull/5818), [#5821](https://github.com/LizardByte/Sunshine/pull/5821)). It is not a replacement with current desktop [Sunshine](https://github.com/LizardByte/Sunshine). The historical `android/sunshine` submodule is not the active native build. Do not run `scripts/apply_patch.sh` as a build/setup step: it copies source trees and can overwrite the active implementation.

## Android behavior and limitations

- **Android 8.0 / API 26 minimum.** Capture mirrors the physical display using MediaProjection → VirtualDisplay → MediaCodec input Surface, without CPU pixel readback or an intermediate GL renderer.
- **One host and one concurrent stream.** Start requests fresh screen-capture consent. Within a running host, stream reconnects reuse the same projection/display, resize it as needed, and attach a new encoder surface. Disconnect detaches the surface; Stop releases the display/projection. Starting again after Stop needs new consent, including on Android 14+. Consent denial, projection revocation, and privileged-backend loss are surfaced instead of silently restarting or downgrading controls.
- **Actual host state.** The UI exposes IDLE, STARTING, RUNNING, STOPPING and FAILED. RUNNING means the host listeners have started, not that a client is already streaming. Duplicate starts are rejected. Stop cancels the host and its stream workers; vendor codec teardown timing still needs device validation.
- **Video depends on the device encoder.** Hardware surface-input AVC is required; HEVC is available only when probed. Negotiation checks dimensions, frame rate, bitrate and profiles. Supported output is SDR 8-bit 4:2:0; AV1, HDR and 4:4:4 requests are rejected, not silently converted to AVC. There is no universal resolution/FPS guarantee.
- **Video continuity failures terminate the affected stream.** A full/unavailable compressed-frame queue no longer clears reference-dependent frames. Failed or partially failed UDP batches stop that session rather than silently discarding an access unit or resending already-sent shards. Socket backpressure waits are cancellation-aware (25 ms polling, 250 ms per-batch deadline). MediaCodec output-format changes and unsupported reference-invalidation requests request a sync frame. These changes address source-proven failure paths; they do **not** establish the cause of, or a verified cure for, the reported intermittent frozen feed. Reconnect after a terminated stream; no periodic IDR/watchdog is added.
- **Sync-frame recovery uses Android's runtime parameter key.** Encoder IDR requests use `request-sync` with an int32 value of zero, matching Android's `PARAMETER_KEY_REQUEST_SYNC_FRAME`; `request-sync-frame` is not the parameter name and can be silently ignored while returning success. This corrects a source-proven recovery defect; decoded playback recovery still requires device verification.
- **Playback audio, not microphone audio.** Android 10 / API 29+ with RECORD_AUDIO permission can capture eligible MEDIA/GAME/UNKNOWN playback at 48 kHz stereo, encoded with Opus. Source-app/Android capture policy still applies. API 26–28, denied audio permission, or unavailable playback capture produce video-only operation. Surround negotiation is unsupported. Public playback capture does **not** mute the source device; the Moonlight host-audio option does not implement Android source muting.

### Explicit privileged control modes

Choose a mode in the app and press **Request control access** before starting:

- **Shizuku** is the initial default. Install/start [Shizuku](https://shizuku.rikka.app/) and grant this app permission. A Shizuku user service hosts the ordered input worker. The displayed worker UID matters: a shell worker (UID 2000) is not root.
- **Root** uses a separate [libsu](https://github.com/topjohnwu/libsu) root service and requires a root grant. It works independently of Shizuku; selecting Shizuku never silently opts into root, and a failure does not switch modes automatically. The selected mode is saved.

Start is gated on real key/mouse/touch injection probes. The UI shows worker UID, capabilities and denial/device reasons; OEM security/debugging restrictions may still block injection. Touch/pen and fallback keys use privileged Android framework injection into the mirrored physical display. Pen support is conditional on its probe. Successful readiness probes do not establish that every game accepts every input type. No ordinary-app Accessibility backend is provided.

The touch readiness probe uses an explicit virtual-device finger `ACTION_DOWN` followed immediately by `ACTION_CANCEL` at the display center, with matching gesture identity; it never sends `ACTION_UP` to click. A standalone cancel is not a valid readiness check when no pointer is active. Probe failures identify the rejected event/result or underlying exception rather than assuming an OEM permission restriction.
Mouse and pen readiness probes use explicit tool types and matching virtual-device identities for `HOVER_ENTER`/`HOVER_MOVE`/`HOVER_EXIT`; exit is attempted even when injection fails, and rejected cleanup cannot report readiness. Runtime pen hover is tracked separately from touch contact and exited on reset/close, rather than relying on touch `CANCEL` to clear hover.


Genuine relative mouse, kernel keyboard and gamepads depend on kernel nodes, permissions, SELinux policy and Android device mapping:

- Actual UID 0 attempts `/dev/uinput` (then `/dev/input/uinput`) for persistent virtual devices. Mouse/controllers can fall back to `/dev/uhid`.
- Shizuku UID 2000 uses UHID for relative mouse/controllers, not uinput; keys use framework injection.
- Device creation must be followed by Android InputDevice registration/mapping validation. Gamepads are advertised only after a successful probe; unavailable or incorrectly mapped controllers return device-unavailable rather than fake gamepad IDs.
- If a genuine relative mouse is unavailable, ordinary UI cursor movement falls back to maintained/clamped absolute-position emulation. This is **not** genuine relative support for games that require it.
- There is no chmod workaround, global SELinux disabling or other SELinux bypass. Root alone does not guarantee working virtual devices or better performance. Generic gamepads do not advertise DS5 emulation, rumble, motion, controller touch or battery support.

Input is ordered per worker session; reset/disconnect releases held keys/buttons/touches and neutralizes devices. Text is limited to strings fully mapped by Android's virtual `KeyCharacterMap`; unsupported strings are rejected before any text prefix is injected. This is **not arbitrary Unicode** support, and it neither changes the clipboard nor installs an IME.

## Build prerequisites

Use **Flutter stable 3.27.2**, revision `68415ad1d920f6fe5ec284f5c2febf7c4dd5b0b3` (recorded in `.metadata`). The metadata records the revision but does not automatically select your SDK; configure PATH/`android/local.properties` to use that Flutter checkout.

- JDK 17 for the Android Gradle toolchain; Android SDK command-line tools and platform 35 for the recorded Flutter SDK.
- **NDK `28.0.13004108`** and **CMake `3.31.6`**, pinned in `android/app/build.gradle`; native sources use C++17.
- A host C/C++ toolchain, Python, Perl, GNU make and `patch` for native dependency builds.
- Network access or populated caches for Flutter/pub, Gradle, Google/Maven Central, JitPack, Boost and OpenSSL dependencies.
- `adb` and a selected physical Android device for installation and device validation.

Install Android packages using your SDK's `sdkmanager`:

```sh
sdkmanager "platforms;android-35" "ndk;28.0.13004108" "cmake;3.31.6"
flutter doctor -v
flutter pub get
```

From the repository root:

```sh
flutter test
flutter build apk --debug --target-platform android-arm64
flutter build apk --release --target-platform android-arm64
```

The release build retains shrinking/minification to exercise JNI/AIDL keep rules. It currently uses the debug signing configuration: it is not a production-signed distribution.

JNI callbacks require stable JVM descriptors, not just method names. Keep `NativeBridge` and the `VideoCapabilities` return type plus its native-read `avc`/`hevc` fields in `android/app/proguard-rules.pro`; otherwise R8 can rename the snapshot type or remove its fields and break release startup (`Missing media bridge: queryVideoCapabilities`). Keep release shrinking enabled when validating this boundary.

Additional existing behavior checks (not proof of working device injection):

```sh
# Java unit tests, using the configured Android/Flutter SDKs
(cd android && ./gradlew :app:testDebugUnitTest)

# Device-independent native boundary tests; JAVA_HOME must identify a JDK for JNI headers
cmake -S android/app/src/main/cpp -B /tmp/sunshine-android-boundaries \
  -DSUNSHINE_ANDROID_BOUNDARY_TESTS=ON
cmake --build /tmp/sunshine-android-boundaries
ctest --test-dir /tmp/sunshine-android-boundaries --output-on-failure
```

## Install and connect

```sh
adb devices -l
export ANDROID_SERIAL=<selected-device-serial>
adb -s "$ANDROID_SERIAL" install -r build/app/outputs/flutter-apk/app-release.apk
adb -s "$ANDROID_SERIAL" shell monkey -p com.nightmare.sunshine_android 1
```

1. Put the Android host and a current Moonlight client on the same reachable network.
2. Select Shizuku or Root, request access, and check **Controls ready**, the actual UID and controller availability. Resolve the displayed denial/OEM/device reason if not ready.
3. Press **Start Server**. Respond to audio/notification permission prompts where applicable, then grant screen-capture consent. Wait for **RUNNING** or a concrete failure.
4. Add the displayed server address in Moonlight. Enter Moonlight's pairing PIN in the host app and press **Add PIN**.
5. Use AVC SDR stereo first; request HEVC only when available. Press **Stop Server** to end the host. After backend loss, explicitly request access/restart; do not expect an automatic downgrade.

**Minified-release startup smoke:** install the newly built release APK using the commands above, select and grant the intended control mode, then grant fresh capture consent after **Start Server**. Observe **RUNNING** (not merely successful consent), confirm no `Missing media bridge`/`Invalid encoder snapshot` failure in the UI or `adb -s "$ANDROID_SERIAL" logcat -d`, and connect Moonlight for an AVC stream to exercise encoder selection, display creation and playback-capture callbacks. Disconnect, press **Stop Server**, wait for **IDLE**, and start again with fresh consent. Repeat with HEVC only if supported. A successful build or inspection of R8 mapping alone is not this runtime proof.

## Validation status

Vermeer codec traces identified GPU preprocessing contention: direct Surface input reached hardware encoding approximately 897 ms old, while hardware encoding averaged 7.35 ms. Encoder `latency=1` and realtime `priority=0` did not resolve it and were removed. The API33+ native-YUV producer uses public ImageWriter codec-native allocation and latest-frame SurfaceTexture capture, without CPU pixel readback or game injection; unsupported setup reports direct-Surface mode explicitly. Standalone AVC/HEVC test patterns decoded correctly on Android 16. During a live MobFGSR stream, native-YUV HEVC completed approximately 82–84 frames/s with 0–1 outstanding works; renderer source age averaged 8.5–8.7 ms. These are host measurements, not a controlled client-latency benchmark.

After the native-YUV target-Y correction and audio continuity update, the user confirmed the stream works normally. Audio capture previously discarded 61 complete PCM packets in one five-second window. Android audio-thread priority and producer credit gating retain the four-packet bound without greedily overflowing it; four observed post-fix windows reported zero discarded packets. The PCM burst/short-read regression and minified release build passed. This establishes observed recovery on Vermeer, not a cross-device or controlled p95 latency benchmark.

Local verification: debug and minified release APK builds succeeded; all four Flutter lifecycle tests, ten Java input boundary tests and three native boundary test executables passed. The Android instrumentation APK compiled, but was not executed on a device. A browser-rendered Flutter surface was inspected only for layout; its Android MethodChannel is unavailable on web, so this is not an Android runtime check.

Physical-device and loaded-GPU validation remain **pending**. Build/unit-test results alone do not prove input injection, virtual-controller compatibility, decoded image/audio correctness, Android 14+ consent lifecycle, finite vendor teardown, or gameplay responsiveness. Validate both Shizuku shell and independent root modes, revocation/death, held-input disconnects, reconnects and repeated Stop/Start on the target device.

No reduced latency versus scrcpy, root performance advantage or OpenFG-equivalent throughput is claimed. Native-YUV host throughput improved in the observed loaded session, but same-scene client frame pacing/loss, thermal comparisons and decoded latency still require validation. MediaCodec presentation timestamps are not reported as capture latency when their clock domain cannot be verified. API26–32 retain direct Surface capture. Public ImageWriter dequeue/queue calls have no timeout guarantee; renderer teardown waits for its owning thread rather than freeing live resources under a stalled vendor call.

## Flutter

Flutter is used for UI development productivity and familiarity, not to claim cross-platform host support. Android is the implemented host backend.
