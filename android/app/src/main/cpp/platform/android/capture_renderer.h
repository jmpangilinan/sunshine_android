#pragma once
// JNI implementation of CaptureRenderer. All EGL/GL operations belong to its Java GL owner;
// only cancellation crosses threads. Uses public AHardwareBuffer imports, never private producer ABI.
