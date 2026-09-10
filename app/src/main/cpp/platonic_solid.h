#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "solid_geometry.h"

/**
 * GPU path: renders a tumbling Platonic solid with OpenGL ES 3 into an
 * offscreen framebuffer, ready to be displayed with ImGui::Image().
 *
 * Geometry and colouring come from BuildSolidMesh() (see solid_geometry.h).
 */
class PlatonicSolidScene
{
public:
    PlatonicSolidScene() = default;
    ~PlatonicSolidScene() { Release(); }

    PlatonicSolidScene(const PlatonicSolidScene&) = delete;
    PlatonicSolidScene& operator=(const PlatonicSolidScene&) = delete;

    /** Must be called while the GL context is still alive. */
    void Release();

    /** Renders one frame at (size x size) and returns the texture to display. */
    ImTextureID Render(SolidType type, int size, float timeSeconds);

    /** Face count of an already built mesh (0 if it has not been built yet). */
    int FaceCount(SolidType type) const;

    /** GPU name from glGetString(GL_RENDERER) (empty before the first frame). */
    const char* RendererName() const { return m_rendererName; }

    /** Version string from glGetString(GL_VERSION). */
    const char* VersionName() const { return m_versionName; }

    /**
     * Copies the last rendered frame out of the offscreen FBO.
     * Must be called while the context that owns this scene is current. Rows
     * come back bottom-up, exactly like the FBO's colour texture.
     */
    bool ReadPixels(std::vector<uint32_t>& outPixels, int& outWidth, int& outHeight);

private:
    /** Reads the GL strings once; needs a current context. */
    void CacheGlStrings();

    struct Mesh
    {
        GLuint vao         = 0;
        GLuint vbo         = 0;
        GLuint vertexCount = 0;
        int    faceCount   = 0;
    };

    bool CreateProgram();
    bool EnsureMesh(SolidType type);
    bool EnsureTarget(int size);

    Mesh   m_meshes[(int)SolidType::Count];

    GLuint m_program  = 0;
    GLint  m_uMVP     = -1;
    GLint  m_uModel   = -1;

    GLuint m_fbo      = 0;
    GLuint m_colorTex = 0;
    GLuint m_depthRb  = 0;
    int    m_size     = 0;

    char   m_rendererName[256] = {};
    char   m_versionName[128]  = {};
};
