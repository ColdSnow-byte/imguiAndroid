#include "platonic_solid.h"

#include <android/log.h>

#include <cmath>
#include <cstdio>
#include <vector>

#define PS_TAG "PlatonicSolid"
#define PS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PS_TAG, __VA_ARGS__)
#define PS_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PS_TAG, __VA_ARGS__)

namespace
{

// NOTE: GLSL requires '#version' to be the very first line, so the raw string
// literal must start right after R"( with no leading newline.
const char* VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vColor;
out vec3 vNormal;

void main()
{
    vColor  = aColor;
    vNormal = mat3(uModel) * aNormal;
    gl_Position = uMVP * vec4(aPosition, 1.0);
}
)";

// Kept in sync with the CPU rasterizer (cpu_solid.cpp):
//   color = vColor * (0.40 + 0.75 * diffuse) + vec3(0.30, 0.45, 0.65) * rim * 0.45
const char* FRAGMENT_SHADER = R"(#version 300 es
precision mediump float;

in vec3 vColor;
in vec3 vNormal;
out vec4 fragColor;

void main()
{
    vec3 n = normalize(vNormal);
    vec3 lightDir = normalize(vec3(0.35, 0.75, 0.85));
    float diffuse = max(dot(n, lightDir), 0.0);
    float rim = pow(1.0 - max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0), 2.0);

    vec3 color = vColor * (0.40 + 0.75 * diffuse);
    color += vec3(0.30, 0.45, 0.65) * rim * 0.45;   // subtle rim light
    fragColor = vec4(color, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        char log[1024] = { 0 };
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        PS_LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// --------------------------------------------------------------------------
// PlatonicSolidScene
// --------------------------------------------------------------------------
bool PlatonicSolidScene::CreateProgram()
{
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, VERTEX_SHADER);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (vs == 0 || fs == 0)
    {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[1024] = { 0 };
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        PS_LOGE("program link failed: %s", log);
        glDeleteProgram(program);
        return false;
    }

    m_program = program;
    m_uMVP = glGetUniformLocation(program, "uMVP");
    m_uModel = glGetUniformLocation(program, "uModel");
    return true;
}

bool PlatonicSolidScene::EnsureMesh(SolidType type)
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return false;
    if (m_meshes[idx].vao != 0)
        return true;

    std::vector<float> data;
    int faceCount = 0;
    if (!BuildSolidMesh(type, data, faceCount) || data.empty())
        return false;

    Mesh& mesh = m_meshes[idx];

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);

    const GLsizei stride = 9 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    mesh.vertexCount = (GLuint)(data.size() / 9);
    mesh.faceCount = faceCount;
    PS_LOGI("built %s: %d faces, %u vertices", SolidDisplayName(type), faceCount, mesh.vertexCount);
    return true;
}

bool PlatonicSolidScene::EnsureTarget(int size)
{
    if (m_fbo != 0 && m_size == size)
        return true;

    Release();

    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &m_depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRb);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        PS_LOGE("incomplete framebuffer: 0x%X", status);
        Release();
        return false;
    }

    m_size = size;
    return true;
}

void PlatonicSolidScene::CacheGlStrings()
{
    if (m_rendererName[0] != '\0')
        return;   // the strings never change, read them once

    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version  = glGetString(GL_VERSION);
    if (renderer != nullptr)
        std::snprintf(m_rendererName, sizeof(m_rendererName), "%s", (const char*)renderer);
    if (version != nullptr)
        std::snprintf(m_versionName, sizeof(m_versionName), "%s", (const char*)version);
}

ImTextureID PlatonicSolidScene::Render(SolidType type, int size, float timeSeconds)
{
    // Quantize so dragging the window border does not recreate the FBO per frame.
    size = ((size + 7) / 8) * 8;
    if (size < 64)
        size = 64;
    if (size > 1024)
        size = 1024;

    if (!EnsureTarget(size))
        return ImTextureID_Invalid;

    CacheGlStrings();   // a context is guaranteed to be current past this point

    if (m_program == 0 && !CreateProgram())
        return ImTextureID_Invalid;
    if (!EnsureMesh(type))
        return ImTextureID_Invalid;

    const Mesh& mesh = m_meshes[(int)type];

    GLint prevFbo = 0;
    GLint prevVao = 0;
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    GLboolean prevDepthTest = GL_FALSE;
    GLboolean prevCullFace = GL_FALSE;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetBooleanv(GL_DEPTH_TEST, &prevDepthTest);
    glGetBooleanv(GL_CULL_FACE, &prevCullFace);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, size, size);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // transparent: the solid floats on the panel
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const Mat4 model = SolidTumbleMatrix(timeSeconds);
    const Mat4 mvp = Mat4Multiply(SolidProjection(),
                                  Mat4Multiply(SolidViewMatrix(), model));

    glUseProgram(m_program);
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, model.m);

    glBindVertexArray(mesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.vertexCount);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (prevCullFace)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    glBindVertexArray((GLuint)prevVao);

    return (ImTextureID)m_colorTex;
}

int PlatonicSolidScene::FaceCount(SolidType type) const
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return 0;
    return m_meshes[idx].faceCount;
}

bool PlatonicSolidScene::ReadPixels(std::vector<uint32_t>& outPixels, int& outWidth, int& outHeight)
{
    if (m_fbo == 0 || m_size <= 0)
        return false;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    outWidth  = m_size;
    outHeight = m_size;
    outPixels.resize((size_t)m_size * (size_t)m_size);
    // Bottom-up rows: GL's framebuffer origin is the bottom-left corner.
    glReadPixels(0, 0, m_size, m_size, GL_RGBA, GL_UNSIGNED_BYTE, outPixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);

    return true;
}

void PlatonicSolidScene::Release()
{
    for (Mesh& mesh : m_meshes)
    {
        if (mesh.vao != 0)
        {
            glDeleteVertexArrays(1, &mesh.vao);
            mesh.vao = 0;
        }
        if (mesh.vbo != 0)
        {
            glDeleteBuffers(1, &mesh.vbo);
            mesh.vbo = 0;
        }
        mesh.vertexCount = 0;
        mesh.faceCount = 0;
    }
    if (m_program != 0)
    {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    if (m_depthRb != 0)
    {
        glDeleteRenderbuffers(1, &m_depthRb);
        m_depthRb = 0;
    }
    if (m_colorTex != 0)
    {
        glDeleteTextures(1, &m_colorTex);
        m_colorTex = 0;
    }
    if (m_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    m_size = 0;
}
