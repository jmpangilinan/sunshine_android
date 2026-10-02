package com.nightmare.sunshine;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Hardware surface encoders discovered once for a host lifetime; no codec is opened here. */
public final class VideoCapabilities {
    public static final int AVC = 0;
    public static final int HEVC = 1;

    public final boolean avc;
    public final boolean hevc;
    private final List<Encoder> encoders;

    private VideoCapabilities(List<Encoder> discovered) {
        encoders = Collections.unmodifiableList(new ArrayList<>(discovered));
        boolean hasAvc = false;
        boolean hasHevc = false;
        for (Encoder encoder : encoders) {
            hasAvc |= encoder.format == AVC;
            hasHevc |= encoder.format == HEVC;
        }
        avc = hasAvc;
        hevc = hasHevc;
    }

    /** Returns a fresh snapshot. Discovery failure means unavailable, never optimistic support. */
    public static VideoCapabilities query() {
        List<Encoder> discovered = new ArrayList<>();
        MediaCodecInfo[] infos;
        try {
            infos = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        } catch (RuntimeException | LinkageError unavailable) {
            return new VideoCapabilities(discovered);
        }
        for (MediaCodecInfo info : infos) {
            try {
                if (!info.isEncoder() || !isHardware(info)) continue;
                for (String type : info.getSupportedTypes()) {
                    int format = formatForMime(type);
                    if (format < 0) continue;
                    try {
                        MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                        if (!hasSurfaceInput(caps)) continue;
                        int profile = supportedProfile(caps, format);
                        MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                        if (profile == 0 || video == null) continue;
                        boolean lowLatency = Build.VERSION.SDK_INT >= 30
                                && caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency);
                        discovered.add(new Encoder(info.getName(), type, format, profile,
                                caps, video, lowLatency));
                    } catch (RuntimeException | LinkageError unavailableType) {
                        // Broken vendor entries must not disable other usable encoders.
                    }
                }
            } catch (RuntimeException | LinkageError unavailableEncoder) {
                // Enumeration can include an encoder whose service is unavailable.
            }
        }
        return new VideoCapabilities(discovered);
    }

    /** Returns a hardware codec name for the exact request, or null (no fallback format). */
    public String selectVideoEncoder(int videoFormat, int width, int height, int fps,
                                     int bitrateKbps) {
        int bitrate = bitrateBitsPerSecond(bitrateKbps);
        if ((videoFormat != AVC && videoFormat != HEVC)
                || width <= 0 || height <= 0 || fps <= 0 || bitrate == 0) return null;
        for (Encoder encoder : encoders) {
            if (encoder.format != videoFormat) continue;
            try {
                MediaCodecInfo.VideoCapabilities video = encoder.video;
                if (width % video.getWidthAlignment() != 0
                        || height % video.getHeightAlignment() != 0
                        || !video.getBitrateRange().contains(bitrate)
                        || !video.areSizeAndRateSupported(width, height, fps)) continue;
                MediaFormat request = MediaFormat.createVideoFormat(encoder.mime, width, height);
                request.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                        MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
                request.setInteger(MediaFormat.KEY_PROFILE, encoder.profile);
                request.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
                request.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
                if (encoder.capabilities.isFormatSupported(request)) return encoder.name;
            } catch (RuntimeException | LinkageError unavailable) {
                // Reject the request if the platform cannot validate it.
            }
        }
        return null;
    }

    /** Android CodecProfileLevel value; zero for an unknown codec or unsupported format. */
    public int profileForEncoder(String codecName, int videoFormat) {
        for (Encoder encoder : encoders) {
            if (encoder.format == videoFormat && encoder.name.equals(codecName)) {
                return encoder.profile;
            }
        }
        return 0;
    }

    /** True only when the framework explicitly advertises the API30 low-latency feature. */
    public boolean lowLatencyForEncoder(String codecName) {
        for (Encoder encoder : encoders) {
            if (encoder.name.equals(codecName) && encoder.lowLatency) return true;
        }
        return false;
    }

    // Package-private pure boundary helpers are usable without loading Android codec services.
    static int bitrateBitsPerSecond(int bitrateKbps) {
        return bitrateKbps > 0 && bitrateKbps <= Integer.MAX_VALUE / 1000
                ? bitrateKbps * 1000 : 0;
    }

    static boolean hardwareNameBeforeApi29(String codecName) {
        if (codecName == null) return false;
        String name = codecName.toLowerCase(Locale.ROOT);
        // Android's documented software namespaces, plus explicitly software-labelled vendors.
        return (name.startsWith("omx.") || name.startsWith("c2."))
                && !name.startsWith("omx.google.") && !name.startsWith("c2.android.")
                && !name.startsWith("c2.google.") && !name.startsWith("omx.ffmpeg.")
                && !name.contains(".sw.") && !name.contains(".software.");
    }

    private static boolean isHardware(MediaCodecInfo info) {
        return Build.VERSION.SDK_INT >= 29 ? info.isHardwareAccelerated()
                : hardwareNameBeforeApi29(info.getName());
    }

    private static int formatForMime(String mime) {
        if (MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(mime)) return AVC;
        if (MediaFormat.MIMETYPE_VIDEO_HEVC.equalsIgnoreCase(mime)) return HEVC;
        return -1;
    }

    private static boolean hasSurfaceInput(MediaCodecInfo.CodecCapabilities caps) {
        for (int color : caps.colorFormats) {
            if (color == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) return true;
        }
        return false;
    }

    private static int supportedProfile(MediaCodecInfo.CodecCapabilities caps, int format) {
        boolean baseline = false;
        for (MediaCodecInfo.CodecProfileLevel level : caps.profileLevels) {
            if (format == HEVC && level.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain) {
                return level.profile;
            }
            if (format == AVC) {
                if (level.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh) return level.profile;
                baseline |= level.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline;
            }
        }
        return baseline ? MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline : 0;
    }

    private static final class Encoder {
        final String name;
        final String mime;
        final int format;
        final int profile;
        // These platform capability objects expose query methods only; never returned to callers.
        final MediaCodecInfo.CodecCapabilities capabilities;
        final MediaCodecInfo.VideoCapabilities video;
        final boolean lowLatency;

        Encoder(String name, String mime, int format, int profile,
                MediaCodecInfo.CodecCapabilities capabilities,
                MediaCodecInfo.VideoCapabilities video, boolean lowLatency) {
            this.name = name;
            this.mime = mime;
            this.format = format;
            this.profile = profile;
            this.capabilities = capabilities;
            this.video = video;
            this.lowLatency = lowLatency;
        }
    }
}
