#include "cpu_solid.h"

#include <android/log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#define CPU_TAG "CpuSolid"
#define CPU_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, CPU_TAG, __VA_ARGS__)

namespace
{

// Light setup, kept in sync with the GLSL fragment shader in platonic_solid.cpp:
//   color = vColor * (0.40 + 0.75 * diffuse) + vec3(0.30, 0.45, 0.65) * rim * 0.45
const float LIGHT_DIR[3] = { 0.35f, 0.75f, 0.85f };
const float RIM_COLOR[3] = { 0.30f, 0.45f, 0.65f };
const float AMBIENT      = 0.40f;
const float DIFFUSE_GAIN = 0.75f;
const float RIM_GAIN     = 0.45f;

/** Camera sits at (0,0,3.2) because the view matrix is Translate(0,0,-3.2). */
const float CAM_Z = 3.2f;

double NowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

inline float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline uint32_t PackRgba(float r, float g, float b)
{
    const uint32_t r8 = (uint32_t)(Clamp01(r) * 255.0f + 0.5f);
    const uint32_t g8 = (uint32_t)(Clamp01(g) * 255.0f + 0.5f);
    const uint32_t b8 = (uint32_t)(Clamp01(b) * 255.0f + 0.5f);
    return 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;   // little endian -> R,G,B,A
}

} // namespace

// --------------------------------------------------------------------------
// CpuSolidScene
// --------------------------------------------------------------------------
bool CpuSolidScene::EnsureMesh(SolidType type)
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return false;
    if (!m_meshes[idx].vertices.empty())
        return true;

    Mesh& mesh = m_meshes[idx];
    if (!BuildSolidMesh(type, mesh.vertices, mesh.faceCount) || mesh.vertices.empty())
        return false;
    return true;
}

bool CpuSolidScene::EnsureBuffers(int size)
{
    if (m_tex != 0 && m_size == size)
        return true;

    Release();

    m_size = size;
    m_pixels.assign((size_t)size * size, 0x00000000u);   // fully transparent
    m_depth.assign((size_t)size * size, 1.0f);           // far plane in NDC

    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (m_rendererName[0] == '\0')
    {
        const unsigned int cores = std::thread::hardware_concurrency();
        if (cores > 0)
            std::snprintf(m_rendererName, sizeof(m_rendererName),
                          u8"CPU 软件光栅化（逐像素）· %u 核可用", cores);
        else
            std::snprintf(m_rendererName, sizeof(m_rendererName), u8"CPU 软件光栅化（逐像素）");
    }

    return m_tex != 0;
}

ImTextureID CpuSolidScene::Render(SolidType type, int size, float timeSeconds)
{
    size = ((size + 7) / 8) * 8;
    if (size < 64)
        size = 64;
    if (size > kMaxSize)
        size = kMaxSize;   // CPU cost is O(pixels), keep it interactive

    if (!EnsureMesh(type))
        return ImTextureID_Invalid;
    if (!EnsureBuffers(size))
        return ImTextureID_Invalid;

    const double t0 = NowMs();

    const Mesh& mesh = m_meshes[(int)type];
    const Mat4  model = SolidTumbleMatrix(timeSeconds);
    const Mat4  mvp = Mat4Multiply(SolidProjection(),
                                   Mat4Multiply(SolidViewMatrix(), model));

    // Clear colour + z buffer
    std::memset(m_pixels.data(), 0, m_pixels.size() * sizeof(uint32_t));
    const size_t pixelCount = m_pixels.size();
    for (size_t i = 0; i < pixelCount; i++)
        m_depth[i] = 1.0f;

    const float S = (float)size;

    // Pre-normalized light direction
    float lx = LIGHT_DIR[0], ly = LIGHT_DIR[1], lz = LIGHT_DIR[2];
    const float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
    lx /= lLen; ly /= lLen; lz /= lLen;

    const float* v = mesh.vertices.data();
    const int triCount = (int)(mesh.vertices.size() / 27);   // 3 verts * 9 floats

    for (int t = 0; t < triCount; t++)
    {
        const float* base = v + t * 27;

        // ---- Flat face normal + back-face culling (model space) ------------
        float nw[3];
        Mat4TransformDirection(model, base[3], base[4], base[5], nw);
        const float nLen = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
        if (nLen <= 0.0f)
            continue;
        nw[0] /= nLen; nw[1] /= nLen; nw[2] /= nLen;

        const float cxm = (base[0] + base[9] + base[18]) / 3.0f;
        const float cym = (base[1] + base[10] + base[19]) / 3.0f;
        const float czm = (base[2] + base[11] + base[20]) / 3.0f;
        float cw[4];
        Mat4TransformPoint(model, cxm, cym, czm, cw);
        // Eye is at (0, 0, CAM_Z): a face is visible when it points back at it.
        if (nw[0] * (0.0f - cw[0]) + nw[1] * (0.0f - cw[1]) + nw[2] * (CAM_Z - cw[2]) <= 0.0f)
            continue;

        // ---- Flat shading terms (constant across the triangle) -------------
        const float diffuse = (nw[0] * lx + nw[1] * ly + nw[2] * lz) > 0.0f
                            ? (nw[0] * lx + nw[1] * ly + nw[2] * lz) : 0.0f;
        const float facing = nw[2] > 0.0f ? nw[2] : 0.0f;
        const float rim = (1.0f - facing) * (1.0f - facing);
        const float amb = AMBIENT + DIFFUSE_GAIN * diffuse;
        const float addR = RIM_COLOR[0] * rim * RIM_GAIN;
        const float addG = RIM_COLOR[1] * rim * RIM_GAIN;
        const float addB = RIM_COLOR[2] * rim * RIM_GAIN;

        // ---- Transform the three corners to screen space -------------------
        float sx[3], sy[3], sz[3], iw[3];
        float pr[3], pg[3], pb[3];   // color * 1/w, ready for interpolation
        bool visible = true;

        for (int k = 0; k < 3; k++)
        {
            const float* p = base + k * 9;
            float clip[4];
            Mat4TransformPoint(mvp, p[0], p[1], p[2], clip);
            if (clip[3] <= 1.0e-6f)      // behind / on the eye plane
            {
                visible = false;
                break;
            }
            const float invW = 1.0f / clip[3];
            iw[k] = invW;
            sx[k] = (clip[0] * invW * 0.5f + 0.5f) * S;
            sy[k] = (0.5f - clip[1] * invW * 0.5f) * S;   // Y flipped: image origin is top-left
            sz[k] = clip[2] * invW;

            pr[k] = p[6] * invW;
            pg[k] = p[7] * invW;
            pb[k] = p[8] * invW;
        }
        if (!visible)
            continue;

        // ---- Bounding box ---------------------------------------------------
        int minX = (int)floorf(fminf(fminf(sx[0], sx[1]), sx[2]));
        int maxX = (int)ceilf(fmaxf(fmaxf(sx[0], sx[1]), sx[2]));
        int minY = (int)floorf(fminf(fminf(sy[0], sy[1]), sy[2]));
        int maxY = (int)ceilf(fmaxf(fmaxf(sy[0], sy[1]), sy[2]));
        if (minX < 0) minX = 0;
        if (minY < 0) minY = 0;
        if (maxX > size - 1) maxX = size - 1;
        if (maxY > size - 1) maxY = size - 1;
        if (minX > maxX || minY > maxY)
            continue;

        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1.0e-9f)
            continue;
        const float invArea = 1.0f / area;

        // ---- Rasterize ------------------------------------------------------
        for (int y = minY; y <= maxY; y++)
        {
            const float py = (float)y + 0.5f;
            uint32_t*       rowPix = &m_pixels[(size_t)y * size];
            float*          rowDep = &m_depth[(size_t)y * size];

            for (int x = minX; x <= maxX; x++)
            {
                const float px = (float)x + 0.5f;

                // Barycentric weights (screen space)
                const float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) * invArea;
                if (w0 < 0.0f) continue;
                const float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) * invArea;
                if (w1 < 0.0f) continue;
                const float w2 = 1.0f - w0 - w1;
                if (w2 < 0.0f) continue;

                // z is linear in screen space -> plain interpolation is exact
                const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                if (z >= rowDep[x])
                    continue;
                rowDep[x] = z;

                // Perspective-correct colour: divide by the interpolated 1/w
                const float inv = 1.0f / (w0 * iw[0] + w1 * iw[1] + w2 * iw[2]);
                const float r = (w0 * pr[0] + w1 * pr[1] + w2 * pr[2]) * inv * amb + addR;
                const float g = (w0 * pg[0] + w1 * pg[1] + w2 * pg[2]) * inv * amb + addG;
                const float b = (w0 * pb[0] + w1 * pb[1] + w2 * pb[2]) * inv * amb + addB;

                rowPix[x] = PackRgba(r, g, b);
            }
        }
    }

    m_lastCpuMs = (float)(NowMs() - t0);

    // ---- Hand the pixels to the GPU (upload only, no rendering) -------------
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    return (ImTextureID)m_tex;
}

int CpuSolidScene::FaceCount(SolidType type) const
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return 0;
    return m_meshes[idx].faceCount;
}

void CpuSolidScene::Release()
{
    if (m_tex != 0)
    {
        glDeleteTextures(1, &m_tex);
        m_tex = 0;
    }
    m_pixels.clear();
    m_depth.clear();
    m_size = 0;
    m_lastCpuMs = 0.0f;
}
