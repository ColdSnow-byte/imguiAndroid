#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "solid_geometry.h"

/**
 * CPU path: a small software rasterizer that draws a tumbling Platonic solid
 * into a plain RGBA pixel buffer - no shaders, no framebuffer, no draw calls.
 *
 * Everything happens on the CPU:
 *   - model/view/projection transform + perspective divide
 *   - back-face culling
 *   - flat shading (diffuse + rim light), same formula as the GLSL version
 *   - bounding-box + barycentric triangle rasterization
 *   - perspective-correct colour interpolation
 *   - z-buffer
 *
 * OpenGL ES is used for exactly one thing: uploading the finished pixels with
 * glTexSubImage2D() so ImGui::Image() can display them.
 */
class CpuSolidScene
{
public:
    CpuSolidScene() = default;
    ~CpuSolidScene() { Release(); }

    CpuSolidScene(const CpuSolidScene&) = delete;
    CpuSolidScene& operator=(const CpuSolidScene&) = delete;

    /** Must be called while the GL context is still alive. */
    void Release();

    /** Rasterizes one frame at (size x size) and returns the texture. */
    ImTextureID Render(SolidType type, int size, float timeSeconds);

    int FaceCount(SolidType type) const;

    /** Time spent rasterizing the last frame, in milliseconds. */
    float LastCpuMs() const { return m_lastCpuMs; }

    /** Resolution actually used for the last frame (capped, see kMaxSize). */
    int RenderSize() const { return m_size; }

    /** Description of this software renderer, e.g. the core count it can use. */
    const char* RendererName() const { return m_rendererName; }

private:
    struct Mesh
    {
        std::vector<float> vertices;   // pos3 + normal3 + color3, non-indexed
        int                faceCount = 0;
    };

    bool EnsureMesh(SolidType type);
    bool EnsureBuffers(int size);

    // Upper bound for the resolution slider. The cost is O(pixels), so above
    // ~512 the software rasterizer starts to dominate the frame time.
    static const int kMaxSize = 1024;

    Mesh                m_meshes[(int)SolidType::Count];

    std::vector<uint32_t> m_pixels;
    std::vector<float>    m_depth;
    int                   m_size = 0;

    GLuint              m_tex = 0;
    float               m_lastCpuMs = 0.0f;
    char                m_rendererName[128] = {};
};
