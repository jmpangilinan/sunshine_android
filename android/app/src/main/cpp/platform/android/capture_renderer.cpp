#include "capture_renderer.h"
#include "../../video_colorspace.h"
#include <jni.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
bool extension(const char *list, const char *name) {
    if (!list) return false;
    size_t n = std::strlen(name);
    for (auto p = list; (p = std::strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}
void report(JNIEnv *env, const char *type, const char *text) {
    auto cls = env->FindClass(type); if (cls) { env->ThrowNew(cls, text); env->DeleteLocalRef(cls); }
}
GLuint shader(GLenum type, const char *text) {
    GLuint id = glCreateShader(type); glShaderSource(id, 1, &text, nullptr); glCompileShader(id);
    GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(id, sizeof(log), nullptr, log); glDeleteShader(id); throw std::runtime_error(log); }
    return id;
}
struct Import {
    AHardwareBuffer *buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
};
struct Renderer {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint program = 0, probeProgram = 0, input = 0;
    GLint transform = -1, probe = -1;
    std::atomic<bool> cancelled {false};
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImage = nullptr;
    std::array<Import, 2> imports {};
    void clear(Import &i) {
        if (i.fbo) glDeleteFramebuffers(1, &i.fbo);
        if (i.texture) glDeleteTextures(1, &i.texture);
        if (i.image != EGL_NO_IMAGE_KHR) destroyImage(display, i.image);
        if (i.buffer) AHardwareBuffer_release(i.buffer);
        i = {};
    }
    ~Renderer() {
        if (context != EGL_NO_CONTEXT) {
            for (auto &i : imports) clear(i);
            if (program) glDeleteProgram(program);
            if (probeProgram) glDeleteProgram(probeProgram);
            if (input) glDeleteTextures(1, &input);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (display != EGL_NO_DISPLAY) eglTerminate(display);
    }
    void initialize(int csc) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialization failed");
        require(eglBindAPI(EGL_OPENGL_ES_API), "EGL API binding failed");
        const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config; EGLint count = 0;
        require(eglChooseConfig(display, configAttrs, &config, 1, &count) && count, "ES3 config unavailable");
        bool priority = extension(eglQueryString(display, EGL_EXTENSIONS), "EGL_IMG_context_priority");
        const EGLint high[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG, EGL_NONE};
        const EGLint normal[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, priority ? high : normal);
        if (context == EGL_NO_CONTEXT && priority) context = eglCreateContext(display, config, EGL_NO_CONTEXT, normal);
        require(context != EGL_NO_CONTEXT, "ES3 context unavailable");
        const EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pbuffer);
        require(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context), "EGL current failed");
        EGLint actual = 0;
        if (priority && !eglQueryContext(display, context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &actual)) actual = 0;
        __android_log_print(ANDROID_LOG_INFO, "SunshineCapture", "nativeYUV GPUpriority=0x%x renderer=%s", actual, glGetString(GL_RENDERER));
        const auto extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        if (!extension(extensions, "GL_EXT_YUV_target") || !extension(extensions, "GL_OES_EGL_image_external_essl3"))
            throw std::invalid_argument("Required public nativeYUV GL extensions unavailable");
        client = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        bindImage = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!client || !createImage || !destroyImage || !bindImage) throw std::invalid_argument("Public EGL image entrypoints unavailable");
        // Imported codec buffers are top-down, unlike an EGL window surface.
        // Flip destination Y before the source transform so its crop/rotation stays intact.
        const char *vertex = "#version 300 es\nprecision highp float; uniform mat4 transform; out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); uv=(transform*vec4(p.x,1.0-p.y,0,1)).xy; gl_Position=vec4(p*2.0-1.0,0,1);}";
        const char *fragment = "#version 300 es\n#extension GL_EXT_YUV_target : require\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float; uniform samplerExternalOES source; uniform vec4 cy,cu,cv; uniform vec2 ry,ruv; uniform bool probe; in vec2 uv; layout(yuv) out vec4 color; void main(){vec4 rgb=vec4(probe?vec3(0.5):texture(source,uv).rgb,1); vec3 yuv=vec3(dot(rgb,cy),dot(rgb,cu),dot(rgb,cv)); color=vec4(yuv*vec3(ry.x,ruv.x,ruv.x)+vec3(ry.y,ruv.y,ruv.y),1);}";
        GLuint v = shader(GL_VERTEX_SHADER, vertex), f = 0;
        try { f = shader(GL_FRAGMENT_SHADER, fragment); } catch (...) { glDeleteShader(v); throw; }
        program = glCreateProgram(); glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
        glDeleteShader(v); glDeleteShader(f);
        GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok); require(ok, "YUV shader link failed");
        glUseProgram(program);
        auto colorspace = ((csc >> 1) == 0) ? video::colorspace_e::rec601 : video::colorspace_e::rec709;
        const auto *vectors = video::color_vectors_from_colorspace(colorspace, (csc & 1) != 0);
        glUniform4fv(glGetUniformLocation(program, "cy"), 1, vectors->color_vec_y);
        glUniform4fv(glGetUniformLocation(program, "cu"), 1, vectors->color_vec_u);
        glUniform4fv(glGetUniformLocation(program, "cv"), 1, vectors->color_vec_v);
        glUniform2fv(glGetUniformLocation(program, "ry"), 1, vectors->range_y);
        glUniform2fv(glGetUniformLocation(program, "ruv"), 1, vectors->range_uv);
        transform = glGetUniformLocation(program, "transform"); probe = glGetUniformLocation(program, "probe");
        glUniform1i(glGetUniformLocation(program, "source"), 0);
        // Capability draw must not sample an uninitialized ingress SurfaceTexture.
        std::string probeSource(fragment);
        const std::string expression = "probe?vec3(0.5):texture(source,uv).rgb";
        probeSource.replace(probeSource.find(expression), expression.size(), "vec3(0.5)");
        v = shader(GL_VERTEX_SHADER, vertex); f = 0;
        try { f = shader(GL_FRAGMENT_SHADER, probeSource.c_str()); } catch (...) { glDeleteShader(v); throw; }
        probeProgram = glCreateProgram(); glAttachShader(probeProgram, v); glAttachShader(probeProgram, f);
        glLinkProgram(probeProgram); glDeleteShader(v); glDeleteShader(f);
        glGetProgramiv(probeProgram, GL_LINK_STATUS, &ok); require(ok, "YUV capability shader link failed");
        glUseProgram(probeProgram);
        glUniform4fv(glGetUniformLocation(probeProgram, "cy"), 1, vectors->color_vec_y);
        glUniform4fv(glGetUniformLocation(probeProgram, "cu"), 1, vectors->color_vec_u);
        glUniform4fv(glGetUniformLocation(probeProgram, "cv"), 1, vectors->color_vec_v);
        glUniform2fv(glGetUniformLocation(probeProgram, "ry"), 1, vectors->range_y);
        glUniform2fv(glGetUniformLocation(probeProgram, "ruv"), 1, vectors->range_uv);
        glGenTextures(1, &input); glBindTexture(GL_TEXTURE_EXTERNAL_OES, input);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        require(glGetError() == GL_NO_ERROR, "YUV initialization GL error");
    }
    void render(AHardwareBuffer *buffer, const float *matrix, bool test) {
        require(buffer != nullptr, "Missing image hardware buffer");
        AHardwareBuffer_Desc desc {}; AHardwareBuffer_describe(buffer, &desc);
        require((desc.usage & AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT) != 0, "Codec buffer lacks GPU output usage");
        if (test) __android_log_print(ANDROID_LOG_INFO, "SunshineCapture",
            "nativeYUV capabilityDraw format=%u width=%u height=%u usage=0x%llx",
            desc.format, desc.width, desc.height, static_cast<unsigned long long>(desc.usage));
        Import *target = nullptr;
        for (auto &i : imports) if (i.buffer == buffer) target = &i;
        if (!target) {
            for (auto &i : imports) if (!i.buffer) { target = &i; break; }
            if (!target) { target = &imports[0]; clear(*target); }
            target->buffer = buffer; AHardwareBuffer_acquire(buffer);
            const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            target->image = createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client(buffer), attrs);
            require(target->image != EGL_NO_IMAGE_KHR, "Codec default buffer EGL import failed");
            glGenTextures(1, &target->texture); glBindTexture(GL_TEXTURE_EXTERNAL_OES, target->texture);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            bindImage(GL_TEXTURE_EXTERNAL_OES, target->image);
            glGenFramebuffers(1, &target->fbo); glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_EXTERNAL_OES, target->texture, 0);
            require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Codec default buffer is not a YUV render target");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, input);
        glViewport(0, 0, desc.width, desc.height); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glUseProgram(test ? probeProgram : program);
        if (!test) { glUniformMatrix4fv(transform, 1, GL_FALSE, matrix); glUniform1i(probe, GL_FALSE); }
        glDrawArrays(GL_TRIANGLES, 0, 3); require(glGetError() == GL_NO_ERROR, "nativeYUV draw rejected (no fallback)");
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); require(fence != nullptr, "GPU completion fence creation failed");
        glFlush();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        bool completed = false;
        while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
            GLenum status = glClientWaitSync(fence, 0, 2000000);
            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) { completed = true; break; }
            if (status == GL_WAIT_FAILED) break;
        }
        glDeleteSync(fence);
        require(completed, cancelled.load() ? "Renderer cancelled" : "GPU completion fence timeout");
        require(glGetError() == GL_NO_ERROR, "nativeYUV completion error");
    }
};
Renderer *get(jlong handle) { return reinterpret_cast<Renderer *>(handle); }
}
extern "C" JNIEXPORT jlong JNICALL Java_com_nightmare_sunshine_CaptureRenderer_00024Api33_nativeCreate(JNIEnv *env, jclass, jint csc) {
    try { auto r = std::make_unique<Renderer>(); r->initialize(csc); return reinterpret_cast<jlong>(r.release()); }
    catch (const std::invalid_argument &e) { report(env, "java/lang/UnsupportedOperationException", e.what()); }
    catch (const std::exception &e) { report(env, "java/lang/IllegalStateException", e.what()); }
    return 0;
}
extern "C" JNIEXPORT jint JNICALL Java_com_nightmare_sunshine_CaptureRenderer_00024Api33_nativeTexture(JNIEnv *, jclass, jlong handle) { return get(handle)->input; }
extern "C" JNIEXPORT void JNICALL Java_com_nightmare_sunshine_CaptureRenderer_00024Api33_nativeRender(JNIEnv *env, jclass, jlong handle, jobject buffer, jfloatArray transform, jboolean probe) {
    try { float matrix[16]; env->GetFloatArrayRegion(transform, 0, 16, matrix); if (env->ExceptionCheck()) return;
        get(handle)->render(AHardwareBuffer_fromHardwareBuffer(env, buffer), matrix, probe); }
    catch (const std::exception &e) { report(env, "java/lang/IllegalStateException", e.what()); }
}
extern "C" JNIEXPORT void JNICALL Java_com_nightmare_sunshine_CaptureRenderer_00024Api33_nativeCancel(JNIEnv *, jclass, jlong handle) { get(handle)->cancelled.store(true); }
extern "C" JNIEXPORT void JNICALL Java_com_nightmare_sunshine_CaptureRenderer_00024Api33_nativeDestroy(JNIEnv *, jclass, jlong handle) { delete get(handle); }
