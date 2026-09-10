#include "angle_solid.h"

#include <EGL/eglext_angle.h>

#include <android/log.h>

#include <cstdio>
#include <ctime>
#include <unistd.h>   // access()

#define ANGLE_TAG "AngleSolid"
#define ANGLE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ANGLE_TAG, __VA_ARGS__)
#define ANGLE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ANGLE_TAG, __VA_ARGS__)

namespace
{

double NowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// The scene renders into an FBO, so the surface only has to exist - keep it tiny.
const EGLint kPbufferSize = 16;

} // namespace

// ---------------------------------------------------------------------------
// AngleSolidScene
// ---------------------------------------------------------------------------
bool AngleSolidScene::BeginAngle(SavedContext& saved)
{
    saved.dpy  = eglGetCurrentDisplay();
    saved.draw = eglGetCurrentSurface(EGL_DRAW);
    saved.read = eglGetCurrentSurface(EGL_READ);
    saved.ctx  = eglGetCurrentContext();

    if (eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_FALSE)
    {
        ANGLE_LOGE("eglMakeCurrent (ANGLE) failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}

void AngleSolidScene::EndAngle(const SavedContext& saved)
{
    eglMakeCurrent(saved.dpy, saved.draw, saved.read, saved.ctx);
}

bool AngleSolidScene::InitAngle()
{
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay == nullptr)
    {
        std::snprintf(m_failure, sizeof(m_failure), "eglGetPlatformDisplayEXT 不存在");
        ANGLE_LOGE("%s", m_failure);
        return false;
    }

    // What the system EGL loader actually advertises. On most retail phones the
    // loader never exposes ANGLE as a selectable platform + backend: ANGLE there
    // is a *system* GLES driver picked by GraphicsEnvironment (allowlist /
    // developer options), not something an app can instantiate on demand.
    const char* eglExts = eglQueryString(EGL_DEFAULT_DISPLAY, EGL_EXTENSIONS);
    ANGLE_LOGI("EGL extensions: %s", eglExts ? eglExts : "(null)");

    // Try the backends from most to least specific. Asking for the Vulkan
    // backend is what makes ANGLE translate GLES calls into Vulkan; if the
    // loader refuses, let ANGLE pick its own default backend instead.
    struct Candidate { EGLint type; const char* name; };
    const Candidate candidates[] = {
            { EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,  "Vulkan" },
            { EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE, "default" },
    };

    EGLint major = 0, minor = 0;
    EGLint lastErr = EGL_SUCCESS;
    for (const Candidate& c : candidates)
    {
        const EGLint displayAttribs[] = {
                EGL_PLATFORM_ANGLE_TYPE_ANGLE,        c.type,
                EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
                EGL_NONE
        };

        EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE,
                                            EGL_DEFAULT_DISPLAY, displayAttribs);
        if (dpy == EGL_NO_DISPLAY)
        {
            lastErr = eglGetError();
            ANGLE_LOGE("ANGLE(%s): eglGetPlatformDisplayEXT -> EGL_NO_DISPLAY, 0x%x",
                       c.name, lastErr);
            continue;
        }
        if (eglInitialize(dpy, &major, &minor) == EGL_FALSE)
        {
            lastErr = eglGetError();
            ANGLE_LOGE("ANGLE(%s): eglInitialize failed, 0x%x", c.name, lastErr);
            continue;
        }

        ANGLE_LOGI("ANGLE(%s) display ready (EGL %d.%d)", c.name, major, minor);
        m_display = dpy;
        break;
    }

    if (m_display == EGL_NO_DISPLAY)
    {
        // 0x300C (EGL_BAD_PARAMETER) means the loader refused EGL_PLATFORM_ANGLE_ANGLE.
        // Note that "EGL_ANGLE_platform_angle" being advertised in the client
        // extension string does NOT prove ANGLE exists: AOSP advertises it
        // unconditionally. The reliable check is whether the ROM ships the libs.
        if (lastErr == EGL_BAD_PARAMETER)
        {
            static const char* kAngleLibs[] = {
                    "/system/lib64/libEGL_angle.so",
                    "/system/lib/libEGL_angle.so",
                    "/vendor/lib64/libEGL_angle.so",
                    "/vendor/lib/libEGL_angle.so",
            };
            bool shipped = false;
            for (const char* lib : kAngleLibs)
            {
                if (access(lib, F_OK) == 0)
                {
                    shipped = true;
                    ANGLE_LOGI("found %s", lib);
                    break;
                }
            }

            if (!shipped)
                std::snprintf(m_failure, sizeof(m_failure),
                              "系统镜像未打包 ANGLE（找不到 libEGL_angle.so），"
                              "此 ROM 无法启用 ANGLE，开发者选项里的开关因此置灰");
            else
                std::snprintf(m_failure, sizeof(m_failure),
                              "EGL 拒绝创建 ANGLE 显示（0x300C EGL_BAD_PARAMETER）：本进程未启用 ANGLE。"
                              "请在「开发者选项 → 图形驱动程序偏好设置」里把本应用设为 ANGLE，"
                              "或执行 adb shell settings put global angle_gl_driver_selection_pkgs "
                              "com.xxy.imguid 后重启应用");
        }
        else
            std::snprintf(m_failure, sizeof(m_failure),
                          "无法创建 ANGLE 显示（eglGetError=0x%x）", lastErr);
        ANGLE_LOGE("%s", m_failure);
        return false;
    }

    const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,   8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE,  8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
    };

    EGLint numConfigs = 0;
    if (eglChooseConfig(m_display, configAttribs, &m_config, 1, &numConfigs) == EGL_FALSE ||
        numConfigs == 0)
    {
        std::snprintf(m_failure, sizeof(m_failure),
                      "eglChooseConfig(ANGLE) 失败：0x%x", eglGetError());
        ANGLE_LOGE("%s", m_failure);
        return false;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
    if (m_context == EGL_NO_CONTEXT)
    {
        std::snprintf(m_failure, sizeof(m_failure),
                      "eglCreateContext(ANGLE) 失败：0x%x", eglGetError());
        ANGLE_LOGE("%s", m_failure);
        return false;
    }

    const EGLint pbufferAttribs[] = {
            EGL_WIDTH,  kPbufferSize,
            EGL_HEIGHT, kPbufferSize,
            EGL_NONE
    };
    m_surface = eglCreatePbufferSurface(m_display, m_config, pbufferAttribs);
    if (m_surface == EGL_NO_SURFACE)
    {
        std::snprintf(m_failure, sizeof(m_failure),
                      "eglCreatePbufferSurface(ANGLE) 失败：0x%x", eglGetError());
        ANGLE_LOGE("%s", m_failure);
        return false;
    }

    // Read the strings while ANGLE is current - this is also the proof that the
    // backend really is Vulkan: it reports "ANGLE (Google, Vulkan 1.x ...)".
    SavedContext saved;
    if (BeginAngle(saved))
    {
        const GLubyte* renderer = glGetString(GL_RENDERER);
        const GLubyte* version  = glGetString(GL_VERSION);
        if (renderer != nullptr)
            std::snprintf(m_rendererName, sizeof(m_rendererName), "%s", (const char*)renderer);
        if (version != nullptr)
            std::snprintf(m_versionName, sizeof(m_versionName), "%s", (const char*)version);
        ANGLE_LOGI("renderer: %s", m_rendererName);
        ANGLE_LOGI("version : %s", m_versionName);
        EndAngle(saved);
    }

    return true;
}

bool AngleSolidScene::EnsureTexture(int size)
{
    if (m_tex != 0 && m_size == size)
        return true;

    if (m_tex != 0)
    {
        glDeleteTextures(1, &m_tex);
        m_tex = 0;
    }

    m_size = size;
    m_pixels.assign((size_t)size * (size_t)size, 0u);

    // Runs on the main context (Render() switches to ANGLE only afterwards).
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return m_tex != 0;
}

ImTextureID AngleSolidScene::Render(SolidType type, int size, float timeSeconds)
{
    size = ((size + 7) / 8) * 8;
    if (size < 64)
        size = 64;
    if (size > kMaxSize)
        size = kMaxSize;

    if (!m_ready)
    {
        if (m_initTried)
            return ImTextureID_Invalid;   // do not retry (and spam logcat) every frame
        m_initTried = true;
        if (!InitAngle())
        {
            ANGLE_LOGE("ANGLE initialisation failed");
            return ImTextureID_Invalid;
        }
        m_ready = true;
    }

    if (!EnsureTexture(size))
        return ImTextureID_Invalid;

    const double t0 = NowMs();

    SavedContext saved;
    if (!BeginAngle(saved))
        return ImTextureID_Invalid;

    // The very same GLES3 renderer as the "OpenGL ES 3" window - ANGLE turns
    // every one of those calls into Vulkan.
    m_inner.Render(type, size, timeSeconds);

    int w = 0, h = 0;
    m_inner.ReadPixels(m_pixels, w, h);

    EndAngle(saved);

    m_lastGpuMs = (float)(NowMs() - t0);

    // Hand the pixels to the main context. Row 0 of the readback is the bottom
    // row (GL origin), so the window displays the texture with flipped UVs.
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    return (ImTextureID)m_tex;
}

int AngleSolidScene::FaceCount(SolidType type) const
{
    return m_inner.FaceCount(type);
}

void AngleSolidScene::Release()
{
    if (m_tex != 0)
    {
        glDeleteTextures(1, &m_tex);   // belongs to the main context
        m_tex = 0;
    }

    if (m_context != EGL_NO_CONTEXT)
    {
        SavedContext saved;
        if (BeginAngle(saved))
        {
            m_inner.Release();   // its GL objects live in the ANGLE context
            EndAngle(saved);
        }
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_CONTEXT;
    }

    if (m_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(m_display, m_surface);
        m_surface = EGL_NO_SURFACE;
    }
    if (m_display != EGL_NO_DISPLAY)
    {
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
    }

    m_config = nullptr;
    m_pixels.clear();
    m_size = 0;
    m_ready = false;
    m_lastGpuMs = 0.0f;
}
