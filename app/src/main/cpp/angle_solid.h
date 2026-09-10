#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "platonic_solid.h"
#include "solid_geometry.h"

/**
 * ANGLE path: the exact same OpenGL ES 3 renderer as PlatonicSolidScene, but
 * running on an ANGLE display whose backend is Vulkan - so every GLES call the
 * scene makes gets translated into Vulkan by ANGLE.
 *
 * That gives a three-way comparison of the same picture:
 *   OpenGL ES 3 -> native GLES driver
 *   ANGLE       -> GLES calls translated to Vulkan
 *   Vulkan 1.1  -> hand written Vulkan pipeline
 *
 * ANGLE owns a private EGLDisplay / EGLContext / pbuffer surface. Rendering
 * happens offscreen inside it; the finished frame is read back with
 * glReadPixels() and uploaded into a texture owned by the main (ImGui)
 * context, because GL objects can never cross contexts.
 */
class AngleSolidScene
{
public:
    AngleSolidScene() = default;
    ~AngleSolidScene() { Release(); }

    AngleSolidScene(const AngleSolidScene&) = delete;
    AngleSolidScene& operator=(const AngleSolidScene&) = delete;

    /** Must be called while the main GL context is still alive. */
    void Release();

    /** Renders one frame at (size x size) and returns the texture to display. */
    ImTextureID Render(SolidType type, int size, float timeSeconds);

    int FaceCount(SolidType type) const;

    /** False when ANGLE (or its Vulkan backend) is unavailable. */
    bool Available() const { return m_ready; }

    /** Renderer string ANGLE reports, e.g. "ANGLE (Google, Vulkan 1.3 ...)". */
    const char* RendererName() const { return m_rendererName; }
    const char* VersionName() const { return m_versionName; }

    /** Why InitAngle() failed; empty when it succeeded. */
    const char* FailureReason() const { return m_failure; }

    /** Time spent rendering + reading back the last frame, in milliseconds. */
    float LastCpuMs() const { return m_lastGpuMs; }

    /** Resolution actually used for the last frame. */
    int RenderSize() const { return m_size; }

private:
    struct SavedContext
    {
        EGLDisplay dpy  = EGL_NO_DISPLAY;
        EGLSurface draw = EGL_NO_SURFACE;
        EGLSurface read = EGL_NO_SURFACE;
        EGLContext ctx  = EGL_NO_CONTEXT;
    };

    bool InitAngle();
    bool EnsureTexture(int size);
    bool BeginAngle(SavedContext& saved);
    void EndAngle(const SavedContext& saved);

    static const int kMaxSize = 1024;

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig  m_config  = nullptr;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;

    // The GLES3 renderer, running on ANGLE instead of the native driver.
    PlatonicSolidScene m_inner;

    std::vector<uint32_t> m_pixels;
    GLuint m_tex  = 0;   // owned by the main context
    int    m_size = 0;

    bool m_ready     = false;
    bool m_initTried = false;
    char m_rendererName[256] = {};
    char m_versionName[128]  = {};
    char m_failure[256]      = {};
    float m_lastGpuMs = 0.0f;
};
