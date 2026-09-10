// ImGui overlay for Android: rendered into a Surface owned by a
// TYPE_APPLICATION_OVERLAY WindowManager window.
//
// Rendering model:
//  - Java side owns the WindowManager window (move / resize via LayoutParams).
//  - Java side hands the SurfaceView's Surface to this library.
//  - A dedicated render thread owns EGL + the Dear ImGui context and draws at ~60 FPS.
//  - Touch events are queued from the UI thread and drained by the render thread.

#include <jni.h>

#include <dirent.h>
#include <unistd.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "platonic_solid.h"
#include "cpu_solid.h"
#include "vulkan_solid.h"
#include "angle_solid.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Set from Java in nativeInit(); lets us read a bundled TrueType font from the
// APK assets regardless of what the system ships.
static AAssetManager* g_assetManager = nullptr;

#define LOG_TAG "ImguiOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace
{

// Must stay in sync with ImguiBridge.ACTION_*
enum TouchAction
{
    TOUCH_DOWN = 0,
    TOUCH_MOVE = 1,
    TOUCH_UP = 2,
    TOUCH_CANCEL = 3
};

struct TouchEvent
{
    int   action;
    float x;
    float y;
};

// ---------------------------------------------------------------------------
// Global state. Everything is guarded by g_mutex; every EGL / GL / ImGui call
// happens on the render thread only.
// ---------------------------------------------------------------------------
std::mutex              g_mutex;
std::condition_variable g_cv;

std::atomic<bool>       g_running{false};
bool                    g_surfacePending = false;   // (re)create the EGLSurface
bool                    g_destroyPending = false;   // tear down the EGLSurface

ANativeWindow*          g_window  = nullptr;
EGLDisplay              g_display = EGL_NO_DISPLAY;
EGLContext              g_context = EGL_NO_CONTEXT;
EGLSurface              g_surface = EGL_NO_SURFACE;
EGLConfig               g_config  = nullptr;
int                     g_width   = 0;
int                     g_height  = 0;

bool                    g_imguiReady = false;
double                  g_lastFrame  = 0.0;
std::string             g_iniPath;
float                   g_scale      = 2.0f;

std::vector<TouchEvent> g_touchQueue;
std::thread             g_renderThread;

// Demo UI state
bool                    g_showDemo  = false;
bool                    g_showSolid = false;
bool                    g_showCpuSolid = false;
bool                    g_showVulkanSolid = false;
bool                    g_showAngleSolid = false;
int                     g_solidIndex = (int)SolidType::Icosahedron;
int                     g_cpuSolidIndex = (int)SolidType::Icosahedron;
int                     g_vulkanSolidIndex = (int)SolidType::Icosahedron;
int                     g_angleSolidIndex = (int)SolidType::Icosahedron;
float                   g_spinSpeed = 1.0f;
float                   g_spinPhase = 0.0f;
int                     g_counter   = 0;
float                   g_slider    = 0.35f;
float                   g_accent[4] = { 0.20f, 0.60f, 0.95f, 1.00f };

// Frame pacing: when true the render thread sleeps to hold 60 FPS and
// eglSwapInterval(1) keeps the buffer swap vsync-locked. When false the thread
// never sleeps and the swap interval drops to 0, so it runs as fast as it can.
bool                    g_lockFps   = true;
// Internal render resolution of each scene, in pixels. The finished image is
// then scaled to fit the window, exactly like a render-scale setting.
int                     g_glSize     = 512;
int                     g_cpuSize    = 512;
int                     g_vulkanSize = 512;
int                     g_angleSize  = 512;
// Smoothed frame rate, shown by the render windows.
float                   g_fps       = 0.0f;

// OpenGL ES scene rendered into an offscreen FBO and shown via ImGui::Image().
PlatonicSolidScene      g_solidScene;
// Same scene, rasterized on the CPU into a pixel buffer.
CpuSolidScene           g_cpuSolidScene;
// Same scene, rendered with a real Vulkan 1.1 pipeline.
VulkanSolidScene        g_vulkanSolidScene;
// Same GLES3 scene again, but on ANGLE's Vulkan backend.
AngleSolidScene         g_angleSolidScene;

double NowSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------
// CJK font lookup: the embedded ImGui font has no Chinese glyphs, so we borrow
// the font that the device already ships.
//
// ImGui rasterizes with stb_truetype, which only understands TrueType outlines
// (a 'glyf' table). Plenty of devices ship their CJK face as a CFF based .otf
// or .ttc (magic 'OTTO'), and handing one of those to AddFontFromFileTTF()
// fails the "stbtt_InitFont(): failed to parse FontData" assertion and aborts
// the whole app. So every candidate is validated before it is ever passed to
// ImGui, and a failure just moves on to the next one.
// ---------------------------------------------------------------------------
std::string ToLower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

uint32_t ReadU32BE(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/** True when the file is an sfnt that stb_truetype can actually parse. */
bool FontUsesTrueTypeOutlines(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;

    uint8_t       head[16] = {};
    const size_t  got = std::fread(head, 1, sizeof(head), f);

    uint32_t dirOffset = 0;
    bool     isSfnt    = false;
    if (got >= 4 && head[0] == 0x00 && head[1] == 0x01 && head[2] == 0x00 && head[3] == 0x00)
    {
        dirOffset = 0;                       // plain TrueType
        isSfnt    = true;
    }
    else if (got >= 16 && head[0] == 't' && head[1] == 't' && head[2] == 'c' && head[3] == 'f')
    {
        dirOffset = ReadU32BE(head + 12);    // table directory of the first face
        isSfnt    = true;
    }

    if (!isSfnt)
    {
        // 'OTTO' (CFF outlines), 'true', 'typ1', 'wOFF' ... stb cannot load these.
        std::fclose(f);
        return false;
    }

    // Table directory: sfntVersion(4) numTables(2) searchRange(2) ...
    if (std::fseek(f, (long)dirOffset + 4, SEEK_SET) != 0)
    {
        std::fclose(f);
        return false;
    }

    uint8_t countRaw[2] = {};
    if (std::fread(countRaw, 1, 2, f) != 2)
    {
        std::fclose(f);
        return false;
    }

    const uint32_t numTables = ((uint32_t)countRaw[0] << 8) | (uint32_t)countRaw[1];

    bool hasGlyf = false;
    for (uint32_t i = 0; i < numTables && i < 4096; i++)
    {
        uint8_t entry[16] = {};
        if (std::fread(entry, 1, 16, f) != 16)
            break;
        if (entry[0] == 'g' && entry[1] == 'l' && entry[2] == 'y' && entry[3] == 'f')
        {
            hasGlyf = true;
            break;
        }
    }

    std::fclose(f);
    return hasGlyf;
}

std::vector<std::string> CollectChineseFontCandidates()
{
    std::vector<std::string> out;

    // Sorted by size: the single-language SC fonts are ~10 MB while the full
    // .ttc collections are ~30 MB, so try the cheap ones first.
    static const char* candidates[] = {
            "/system/fonts/NotoSansSC-Regular.ttf",
            "/system/fonts/NotoSansSC-Regular.otf",
            "/system/fonts/NotoSansCJKsc-Regular.otf",
            "/system/fonts/SourceHanSansSC-Regular.otf",
            "/system/fonts/HarmonyOS_Sans_SC_Regular.ttf",
            "/system/fonts/MiSans-Regular.ttf",
            "/system/fonts/Miui-Regular.ttf",
            "/system/fonts/DroidSansFallback.ttf",
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSerifCJK-Regular.ttc",
    };
    for (const char* path : candidates)
    {
        if (access(path, R_OK) == 0)
            out.emplace_back(path);
    }

    // Fallback: scan the system font folder for anything CJK-ish.
    DIR* dir = opendir("/system/fonts");
    if (dir != nullptr)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            const std::string lower = ToLower(entry->d_name);
            if (lower.find("bold") != std::string::npos)
                continue;
            const bool match = lower.find("cjk") != std::string::npos
                            || lower.find("sc.") != std::string::npos
                            || lower.find("hans") != std::string::npos
                            || lower.find("fallback") != std::string::npos
                            || lower.find("miui") != std::string::npos
                            || lower.find("misans") != std::string::npos;
            if (match)
                out.emplace_back(std::string("/system/fonts/") + entry->d_name);
        }
        closedir(dir);
    }

    // De-duplicate (the scan repeats the explicit paths) but keep the order.
    std::vector<std::string> unique;
    for (const std::string& path : out)
    {
        if (std::find(unique.begin(), unique.end(), path) == unique.end())
            unique.push_back(path);
    }
    return unique;
}

/** Same validation as FontUsesTrueTypeOutlines(), but on an in-memory buffer. */
bool FontUsesTrueTypeOutlinesMem(const unsigned char* d, size_t n)
{
    if (n < 16)
        return false;

    uint32_t dirOffset = 0;
    bool     isSfnt    = false;
    if (d[0] == 0x00 && d[1] == 0x01 && d[2] == 0x00 && d[3] == 0x00)
    {
        dirOffset = 0;
        isSfnt    = true;
    }
    else if (d[0] == 't' && d[1] == 't' && d[2] == 'c' && d[3] == 'f')
    {
        dirOffset = ReadU32BE(d + 12);
        isSfnt    = true;
    }
    if (!isSfnt)
        return false;

    const unsigned char* p = d + dirOffset + 4;   // skip sfntVersion
    if ((size_t)(p - d) + 2 > n)
        return false;
    const uint32_t numTables = ((uint32_t)p[0] << 8) | p[1];
    p += 8;   // numTables(2) + searchRange/entrySelector/rangeShift(6) => records start at +12
    for (uint32_t i = 0; i < numTables && i < 4096; i++)
    {
        if ((size_t)(p - d) + 16 > n)
            break;
        if (p[0] == 'g' && p[1] == 'l' && p[2] == 'y' && p[3] == 'f')
            return true;
        p += 16;
    }
    return false;
}

// Kept alive for the lifetime of the ImGui atlas (FontDataOwnedByAtlas=false).
std::vector<uint8_t> g_cjkFontData;
static bool          g_cjkFontProvided = false;

/** Primary path: a CJK font buffer supplied from Java (read from the APK
 *  assets there, which is more reliable than reading them from native). */
bool LoadCjkFontFromProvidedBytes(ImFontAtlas* atlas, float size)
{
    if (!g_cjkFontProvided || g_cjkFontData.size() < 12)
        return false;
    if (!FontUsesTrueTypeOutlinesMem(g_cjkFontData.data(), g_cjkFontData.size()))
    {
        LOGI("skip provided CJK font: not a TrueType outline font");
        return false;
    }

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // kept alive in g_cjkFontData
    cfg.FontNo               = 0;
    ImFont* font = atlas->AddFontFromMemoryTTF(
            g_cjkFontData.data(), (int)g_cjkFontData.size(), size, &cfg,
            atlas->GetGlyphRangesChineseSimplifiedCommon());
    if (font == nullptr)
        return false;
    LOGI("loaded CJK font from Java-provided buffer (%zu bytes)", g_cjkFontData.size());
    return true;
}

/** Fallback: read a bundled TrueType CJK font straight from the native AssetManager. */
bool LoadCjkFontFromAsset(ImFontAtlas* atlas, const char* name, float size)
{
    if (g_assetManager == nullptr)
        return false;

    AAsset* asset = AAssetManager_open(g_assetManager, name, AASSET_MODE_BUFFER);
    if (asset == nullptr)
        return false;

    const off_t len = AAsset_getLength(asset);
    std::vector<uint8_t> buf((size_t)len);
    size_t total = 0;
    while (total < (size_t)len)   // compressed assets may return partial reads
    {
        const int64_t r = AAsset_read(asset, buf.data() + total, (size_t)len - total);
        if (r <= 0)
            break;
        total += (size_t)r;
    }
    AAsset_close(asset);

    if (total != (size_t)len || buf.size() < 12 ||
        !FontUsesTrueTypeOutlinesMem(buf.data(), buf.size()))
    {
        LOGI("skip asset %s: missing or not a TrueType outline font", name);
        return false;
    }

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // we keep buf alive in g_cjkFontData
    cfg.FontNo               = 0;
    ImFont* font = atlas->AddFontFromMemoryTTF(
            buf.data(), (int)buf.size(), size, &cfg,
            atlas->GetGlyphRangesChineseSimplifiedCommon());
    if (font == nullptr)
        return false;

    g_cjkFontData = std::move(buf);     // keep the bytes alive for the atlas
    LOGI("loaded CJK font from assets/%s", name);
    return true;
}

// ---------------------------------------------------------------------------
// EGL helpers (called with g_mutex held, on the render thread)
// ---------------------------------------------------------------------------
bool EnsureEglDisplayLocked()
{
    if (g_display != EGL_NO_DISPLAY && g_context != EGL_NO_CONTEXT)
        return true;

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY)
    {
        LOGE("eglGetDisplay() failed");
        return false;
    }
    if (eglInitialize(g_display, nullptr, nullptr) != EGL_TRUE)
    {
        LOGE("eglInitialize() failed: 0x%X", eglGetError());
        g_display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      0,
            EGL_STENCIL_SIZE,    0,
            EGL_NONE
    };
    const EGLint config_attribs_fallback[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE
    };

    EGLint num_configs = 0;
    bool   picked = false;
    if (eglChooseConfig(g_display, config_attribs, nullptr, 0, &num_configs) == EGL_TRUE && num_configs > 0)
        picked = eglChooseConfig(g_display, config_attribs, &g_config, 1, &num_configs) == EGL_TRUE;
    if (!picked)
    {
        LOGE("falling back to the default EGL config");
        eglChooseConfig(g_display, config_attribs_fallback, &g_config, 1, &num_configs);
    }

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, context_attribs);
    if (g_context == EGL_NO_CONTEXT)
    {
        LOGE("eglCreateContext() failed: 0x%X", eglGetError());
        return false;
    }
    return true;
}

void DestroyEglSurfaceLocked()
{
    if (g_display == EGL_NO_DISPLAY)
        return;
    if (g_surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
    }
}

// Creates (or re-creates, after a resize) the window surface and makes it current.
bool CreateEglSurfaceLocked()
{
    if (!EnsureEglDisplayLocked())
        return false;
    if (g_window == nullptr)
        return false;

    DestroyEglSurfaceLocked();

    EGLint format = 0;
    eglGetConfigAttrib(g_display, g_config, EGL_NATIVE_VISUAL_ID, &format);
    // Only the pixel format is forced here: the surface size is driven by the
    // WindowManager LayoutParams (that is how the overlay gets resized).
    ANativeWindow_setBuffersGeometry(g_window, 0, 0, format);

    g_surface = eglCreateWindowSurface(g_display, g_config, g_window, nullptr);
    if (g_surface == EGL_NO_SURFACE)
    {
        LOGE("eglCreateWindowSurface() failed: 0x%X", eglGetError());
        return false;
    }
    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) != EGL_TRUE)
    {
        LOGE("eglMakeCurrent() failed: 0x%X", eglGetError());
        DestroyEglSurfaceLocked();
        return false;
    }

    if (!g_imguiReady)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = g_iniPath.empty() ? nullptr : g_iniPath.c_str();
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(g_scale);
        style.FontScaleDpi = g_scale;

        // Chinese UI: borrow the device CJK font. Candidates are validated
        // first because stb_truetype aborts the process on a CFF based face,
        // and a rejected one just falls through to the next candidate.
        bool font_loaded = false;

        // 1) Bundled TrueType CJK font, supplied as raw bytes from Java (most
        //    reliable). Some ROMs (e.g. HyperOS) only ship CFF/OTF CJK faces
        //    that stb_truetype cannot parse, so we bundle our own TrueType.
        if (LoadCjkFontFromProvidedBytes(io.Fonts, 15.0f))
        {
            font_loaded = true;
        }
        else
        {
            static const char* asset_names[] = { "cjk_font.ttf", "NotoSansSC.ttf", "cjk.ttf" };
            for (const char* an : asset_names)
            {
                if (LoadCjkFontFromAsset(io.Fonts, an, 15.0f))
                {
                    font_loaded = true;
                    break;
                }
            }
        }

        // 2) Fall back to a system font that uses TrueType outlines.
        if (!font_loaded)
        {
            for (const std::string& font_path : CollectChineseFontCandidates())
            {
                if (!FontUsesTrueTypeOutlines(font_path.c_str()))
                {
                    LOGI("skip %s: no TrueType outlines (CFF/OTF is unsupported)", font_path.c_str());
                    continue;
                }

                ImFontConfig font_cfg;
                font_cfg.FontNo = 0; // first face of a .ttc collection
                ImFont* font = io.Fonts->AddFontFromFileTTF(
                        font_path.c_str(), 15.0f, &font_cfg,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (font != nullptr)
                {
                    LOGI("loaded CJK font: %s", font_path.c_str());
                    font_loaded = true;
                    break;
                }
                LOGE("failed to load CJK font: %s", font_path.c_str());
            }
        }

        if (!font_loaded)
        {
            // Nothing usable: keep the built-in font so the overlay still runs.
            // Chinese text then renders as boxes instead of crashing.
            LOGE("no usable CJK font (asset or system), using the embedded one");
            io.Fonts->AddFontDefault();
        }

        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imguiReady = true;
        LOGI("ImGui context created (scale=%.2f)", (double)g_scale);
    }

    g_lastFrame = 0.0;
    glViewport(0, 0, g_width, g_height);
    return true;
}

void ShutdownImGuiLocked()
{
    if (!g_imguiReady)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_imguiReady = false;
}

// ---------------------------------------------------------------------------
// ImGui frame
// ---------------------------------------------------------------------------
void DrawSolidWindow();
void DrawCpuSolidWindow();
void DrawVulkanSolidWindow();
void DrawAngleSolidWindow();
void SolidCombo(const char* label, int* index);
int  SquareContentSide();

void BuildUi(float w, float h)
{
    ImGuiIO& io = ImGui::GetIO();

    // The window always covers the whole surface: the Android side already owns
    // the title bar (drag) and the resize grip, so no ImGui title bar here.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 0.93f));

    ImGui::Begin("##imgui_overlay_root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(g_accent[0], g_accent[1], g_accent[2], 1.0f), u8"ImGui 悬浮窗");
    ImGui::SameLine();
    ImGui::TextDisabled(u8"TYPE_APPLICATION_OVERLAY");
    ImGui::Separator();

    ImGui::Text(u8"渲染尺寸：%d x %d", (int)w, (int)h);
    ImGui::Text(u8"帧率：%.1f FPS（%.2f ms/帧）", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Text(u8"缩放比例：%.2f", (double)g_scale);
    ImGui::Separator();

    ImGui::Spacing();
    ImGui::TextWrapped(u8"拖动顶部标题栏可移动窗口，拖动右下角把手可缩放窗口；"
                       u8"两者都是通过 Java 侧动态修改 WindowManager.LayoutParams 实现的。");
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Checkbox(u8"显示旋转正多面体（OpenGL ES 渲染）", &g_showSolid);
    ImGui::SameLine();
    ImGui::TextDisabled("GPU");
    ImGui::Checkbox(u8"显示旋转正多面体（CPU 软渲染）", &g_showCpuSolid);
    ImGui::SameLine();
    ImGui::TextDisabled("CPU");
    ImGui::Checkbox(u8"显示旋转正多面体（Vulkan 1.1 渲染）", &g_showVulkanSolid);
    ImGui::SameLine();
    ImGui::TextDisabled("VK");
    ImGui::Checkbox(u8"显示旋转正多面体（ANGLE：GLES 转 Vulkan）", &g_showAngleSolid);
    ImGui::SameLine();
    ImGui::TextDisabled("ANGLE");
    ImGui::Checkbox(u8"显示 ImGui 官方演示窗口", &g_showDemo);
    ImGui::SliderFloat(u8"滑条", &g_slider, 0.0f, 1.0f);
    ImGui::ColorEdit4(u8"主题色", g_accent);

    if (ImGui::Button(u8"点我一下"))
        g_counter++;
    ImGui::SameLine();
    ImGui::Text(u8"计数 = %d", g_counter);

    ImGui::ProgressBar(g_slider, ImVec2(-FLT_MIN, 0.0f), "");

    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (g_showSolid)
        DrawSolidWindow();

    if (g_showCpuSolid)
        DrawCpuSolidWindow();

    if (g_showVulkanSolid)
        DrawVulkanSolidWindow();

    if (g_showAngleSolid)
        DrawAngleSolidWindow();

    if (g_showDemo)
        ImGui::ShowDemoWindow(&g_showDemo);
}

void SolidCombo(const char* label, int* index)
{
    static const char* items[(int)SolidType::Count] = {
            SolidDisplayName(SolidType::Tetrahedron),
            SolidDisplayName(SolidType::Cube),
            SolidDisplayName(SolidType::Octahedron),
            SolidDisplayName(SolidType::Dodecahedron),
            SolidDisplayName(SolidType::Icosahedron),
    };
    ImGui::Combo(label, index, items, (int)SolidType::Count);
}

int SquareContentSide()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    int side = (int)(avail.x < avail.y ? avail.x : avail.y);
    if (side < 64)
        side = 64;
    if (side > 1024)
        side = 1024;
    return side;
}

void DrawSolidWindow()
{
    ImGui::SetNextWindowSize(ImVec2(340.0f * g_scale, 570.0f * g_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin(u8"正多面体 · OpenGL ES 3", &g_showSolid, ImGuiWindowFlags_None);

    SolidCombo(u8"选择正多面体", &g_solidIndex);

    const SolidType type = (SolidType)g_solidIndex;
    const int display = SquareContentSide();

    const ImTextureID tex = g_solidScene.Render(type, g_glSize, g_spinPhase);
    if (tex != ImTextureID_Invalid)
    {
        // GL framebuffer textures have their origin at the bottom-left -> flip.
        ImGui::Image(ImTextureRef(tex), ImVec2((float)display, (float)display),
                     ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else
    {
        ImGui::TextDisabled(u8"渲染不可用");
    }

    ImGui::Spacing();
    ImGui::SliderFloat(u8"旋转速度##gl", &g_spinSpeed, 0.0f, 3.0f);
    ImGui::SliderInt(u8"渲染分辨率##gl", &g_glSize, 64, 1024, u8"%d px");
    ImGui::Checkbox(u8"锁定 60 帧（取消则解锁为无限）##gl", &g_lockFps);
    ImGui::Text(u8"%d 个面，每个面三色相互渐变", g_solidScene.FaceCount(type));
    ImGui::Text(u8"帧率：%.1f FPS（%s）", (double)g_fps,
                g_lockFps ? u8"已锁定 60" : u8"已解锁 · 无限");
    if (g_solidScene.RendererName()[0] != '\0')
    {
        ImGui::TextWrapped(u8"渲染器：%s · %s",
                           g_solidScene.VersionName(), g_solidScene.RendererName());
    }
    ImGui::End();
}

void DrawCpuSolidWindow()
{
    ImGui::SetNextWindowSize(ImVec2(340.0f * g_scale, 600.0f * g_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin(u8"正多面体 · CPU 软渲染", &g_showCpuSolid, ImGuiWindowFlags_None);

    SolidCombo(u8"选择正多面体##cpu", &g_cpuSolidIndex);

    const SolidType type = (SolidType)g_cpuSolidIndex;
    const int display = SquareContentSide();

    const ImTextureID tex = g_cpuSolidScene.Render(type, g_cpuSize, g_spinPhase);
    if (tex != ImTextureID_Invalid)
    {
        // The CPU buffer is filled top row first, so no UV flip is needed here.
        ImGui::Image(ImTextureRef(tex), ImVec2((float)display, (float)display));
    }
    else
    {
        ImGui::TextDisabled(u8"渲染不可用");
    }

    ImGui::Spacing();
    ImGui::SliderFloat(u8"旋转速度##cpu", &g_spinSpeed, 0.0f, 3.0f);
    ImGui::SliderInt(u8"渲染分辨率##cpu", &g_cpuSize, 64, 1024, u8"%d px");
    ImGui::Checkbox(u8"锁定 60 帧（取消则解锁为无限）##cpu", &g_lockFps);
    ImGui::Text(u8"%d 个面，每个面三色相互渐变", g_cpuSolidScene.FaceCount(type));
    ImGui::Text(u8"光栅化 %dx%d 用时 %.2f ms",
                g_cpuSolidScene.RenderSize(), g_cpuSolidScene.RenderSize(),
                (double)g_cpuSolidScene.LastCpuMs());
    ImGui::Text(u8"帧率：%.1f FPS（%s）", (double)g_fps,
                g_lockFps ? u8"已锁定 60" : u8"已解锁 · 无限");
    if (g_cpuSolidScene.RendererName()[0] != '\0')
        ImGui::TextWrapped(u8"渲染器：%s", g_cpuSolidScene.RendererName());
    ImGui::End();
}

void DrawVulkanSolidWindow()
{
    ImGui::SetNextWindowSize(ImVec2(360.0f * g_scale, 660.0f * g_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin(u8"正多面体 · Vulkan 1.1", &g_showVulkanSolid, ImGuiWindowFlags_None);

    SolidCombo(u8"选择正多面体##vk", &g_vulkanSolidIndex);

    const SolidType type = (SolidType)g_vulkanSolidIndex;
    const int       display = SquareContentSide();

    const ImTextureID tex = g_vulkanSolidScene.Render(type, g_vulkanSize, g_spinPhase);
    if (tex != ImTextureID_Invalid)
    {
        // Row 0 of a Vulkan framebuffer is the top row and glTexSubImage2D()
        // uploads row 0 first, so no UV flip is needed here.
        ImGui::Image(ImTextureRef(tex), ImVec2((float)display, (float)display));
    }
    else
    {
        ImGui::TextDisabled(u8"Vulkan 不可用（此设备未初始化成功）");
    }

    ImGui::Spacing();
    ImGui::SliderFloat(u8"旋转速度##vk", &g_spinSpeed, 0.0f, 3.0f);
    ImGui::SliderInt(u8"渲染分辨率##vk", &g_vulkanSize, 64, 1024, u8"%d px");

    // Frame pacing: the render thread either sleeps to hold 60 FPS or runs flat out.
    ImGui::Checkbox(u8"锁定 60 帧（取消则解锁为无限）##vk", &g_lockFps);

    ImGui::Separator();
    ImGui::Text(u8"帧率：%.1f FPS（%s）", (double)g_fps,
                g_lockFps ? u8"已锁定 60" : u8"已解锁 · 无限");
    ImGui::Text(u8"Vulkan 绘制：%.2f ms/帧", (double)g_vulkanSolidScene.LastGpuMs());
    ImGui::Text(u8"渲染尺寸：%d x %d",
                g_vulkanSolidScene.RenderSize(), g_vulkanSolidScene.RenderSize());
    ImGui::Text(u8"%d 个面，每个面三色相互渐变", g_vulkanSolidScene.FaceCount(type));

    if (g_vulkanSolidScene.ApiVersion() != 0)
    {
        ImGui::TextWrapped(u8"渲染器：Vulkan %u.%u.%u · %s",
                           VK_VERSION_MAJOR(g_vulkanSolidScene.ApiVersion()),
                           VK_VERSION_MINOR(g_vulkanSolidScene.ApiVersion()),
                           VK_VERSION_PATCH(g_vulkanSolidScene.ApiVersion()),
                           g_vulkanSolidScene.DeviceName());
    }

    ImGui::End();
}

void DrawAngleSolidWindow()
{
    ImGui::SetNextWindowSize(ImVec2(360.0f * g_scale, 640.0f * g_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin(u8"正多面体 · ANGLE（GLES → Vulkan）", &g_showAngleSolid, ImGuiWindowFlags_None);

    SolidCombo(u8"选择正多面体##angle", &g_angleSolidIndex);

    const SolidType type = (SolidType)g_angleSolidIndex;
    const int       display = SquareContentSide();

    const ImTextureID tex = g_angleSolidScene.Render(type, g_angleSize, g_spinPhase);
    if (tex != ImTextureID_Invalid)
    {
        // glReadPixels() hands back bottom-up rows, so flip like the GL FBO path.
        ImGui::Image(ImTextureRef(tex), ImVec2((float)display, (float)display),
                     ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else
    {
        const char* reason = g_angleSolidScene.FailureReason();
        if (reason[0] != '\0')
            ImGui::TextWrapped(u8"ANGLE 不可用：%s", reason);
        else
            ImGui::TextDisabled(u8"ANGLE 不可用（此设备未提供 ANGLE 的 Vulkan 后端）");
    }

    ImGui::Spacing();
    ImGui::SliderFloat(u8"旋转速度##angle", &g_spinSpeed, 0.0f, 3.0f);
    ImGui::SliderInt(u8"渲染分辨率##angle", &g_angleSize, 64, 1024, u8"%d px");
    ImGui::Checkbox(u8"锁定 60 帧（取消则解锁为无限）##angle", &g_lockFps);

    ImGui::Separator();
    ImGui::Text(u8"帧率：%.1f FPS（%s）", (double)g_fps,
                g_lockFps ? u8"已锁定 60" : u8"已解锁 · 无限");
    ImGui::Text(u8"ANGLE 绘制 + 回读：%.2f ms/帧", (double)g_angleSolidScene.LastCpuMs());
    ImGui::Text(u8"渲染尺寸：%d x %d",
                g_angleSolidScene.RenderSize(), g_angleSolidScene.RenderSize());
    ImGui::Text(u8"%d 个面，每个面三色相互渐变", g_angleSolidScene.FaceCount(type));

    if (g_angleSolidScene.RendererName()[0] != '\0')
    {
        // ANGLE reports "ANGLE (Google, Vulkan 1.x ...)" - the proof that the
        // GLES calls really are being translated to Vulkan.
        ImGui::TextWrapped(u8"渲染器：%s", g_angleSolidScene.RendererName());
        ImGui::TextWrapped(u8"版本：%s", g_angleSolidScene.VersionName());
    }

    ImGui::End();
}

void RenderFrame(int w, int h)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    const double now = NowSeconds();
    io.DeltaTime = g_lastFrame > 0.0 ? (float)(now - g_lastFrame) : (float)(1.0f / 60.0f);
    g_lastFrame = now;
    g_spinPhase += io.DeltaTime * g_spinSpeed;

    // Exponentially smoothed frame rate, displayed by the render windows.
    if (io.DeltaTime > 0.0f)
    {
        const float instant = 1.0f / io.DeltaTime;
        g_fps = (g_fps > 0.0f) ? g_fps * 0.9f + instant * 0.1f : instant;
    }

    // Drain the touch queue filled by the UI thread.
    std::vector<TouchEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        events.swap(g_touchQueue);
    }
    for (const TouchEvent& e : events)
    {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e.x, e.y);
        if (e.action == TOUCH_DOWN)
            io.AddMouseButtonEvent(0, true);
        else if (e.action == TOUCH_UP || e.action == TOUCH_CANCEL)
        {
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // clear hover state
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    BuildUi((float)w, (float)h);

    ImGui::Render();

    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent: the overlay floats over other apps
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    eglSwapBuffers(g_display, g_surface);
}

void RenderThreadMain()
{
    LOGI("render thread started");
    const double frame_budget = 1.0 / 60.0;
    bool         lastLockFps  = g_lockFps;

    while (true)
    {
        int  w = 0, h = 0;
        bool render = false;

        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_cv.wait(lk, [] {
                return !g_running.load() || g_destroyPending || g_surfacePending ||
                       g_surface != EGL_NO_SURFACE;
            });

            if (!g_running.load())
                break;

            if (g_destroyPending)
            {
                DestroyEglSurfaceLocked();
                if (g_window != nullptr)
                {
                    ANativeWindow_release(g_window);
                    g_window = nullptr;
                }
                g_touchQueue.clear();
                g_destroyPending = false;
                g_cv.notify_all();
                continue;
            }

            if (g_surfacePending)
            {
                CreateEglSurfaceLocked();
                g_surfacePending = false;
            }

            render = (g_surface != EGL_NO_SURFACE);
            w = g_width;
            h = g_height;
        }

        if (!render)
            continue;

        const double frame_start = NowSeconds();
        RenderFrame(w, h);

        // Frame pacing. Locked: sleep to hold 60 FPS and keep the buffer swap
        // vsync-locked. Unlocked: no sleep at all and swap interval 0, so the
        // loop runs as fast as the GPU can turn frames around.
        if (g_lockFps != lastLockFps)
        {
            eglSwapInterval(g_display, g_lockFps ? 1 : 0);
            lastLockFps = g_lockFps;
            LOGI("frame pacing: %s", g_lockFps ? "locked to 60 FPS" : "unlimited");
        }

        if (g_lockFps)
        {
            const double elapsed = NowSeconds() - frame_start;
            if (elapsed < frame_budget)
                usleep((useconds_t)((frame_budget - elapsed) * 1000000.0));
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        // Keep the context current (surfaceless) so ImGui can release its GL objects.
        if (g_display != EGL_NO_DISPLAY && g_context != EGL_NO_CONTEXT)
            eglMakeCurrent(g_display, g_surface, g_surface, g_context);
        g_solidScene.Release();
        g_cpuSolidScene.Release();
        g_vulkanSolidScene.Release();
        g_angleSolidScene.Release();
        ShutdownImGuiLocked();
        DestroyEglSurfaceLocked();
        if (g_display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (g_context != EGL_NO_CONTEXT)
                eglDestroyContext(g_display, g_context);
            eglTerminate(g_display);
        }
        g_display = EGL_NO_DISPLAY;
        g_context = EGL_NO_CONTEXT;
        g_config  = nullptr;
        if (g_window != nullptr)
        {
            ANativeWindow_release(g_window);
            g_window = nullptr;
        }
        g_touchQueue.clear();
        g_surfacePending = false;
        g_destroyPending = false;
    }
    g_cv.notify_all();
    LOGI("render thread stopped");
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    (void)vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_xxy_imguid_ImguiBridge_nativeProvideCjkFont(JNIEnv* env, jclass, jbyteArray data)
{
    if (data == nullptr)
    {
        g_cjkFontProvided = false;
        g_cjkFontData.clear();
        return;
    }
    const jsize len = env->GetArrayLength(data);
    g_cjkFontData.resize((size_t)len);
    env->GetByteArrayRegion(data, 0, len,
                            reinterpret_cast<jbyte*>(g_cjkFontData.data()));
    g_cjkFontProvided = (len > 0);
    LOGI("nativeProvideCjkFont: received %d bytes", (int)len);
}

JNIEXPORT void JNICALL
Java_com_xxy_imguid_ImguiBridge_nativeInit(JNIEnv* env, jclass, jstring files_dir, jfloat density, jobject assetManager)
{
    const char* chars = env->GetStringUTFChars(files_dir, nullptr);
    std::string dir = chars != nullptr ? chars : "";
    if (chars != nullptr)
        env->ReleaseStringUTFChars(files_dir, chars);

    if (assetManager != nullptr)
        g_assetManager = AAssetManager_fromJava(env, assetManager);
    else
        LOGE("nativeInit: AssetManager is null, bundled fonts unavailable");

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_iniPath = dir + "/imgui.ini";
        g_scale = density < 1.5f ? 1.5f : (density > 3.0f ? 3.0f : density);
    }

    if (!g_running.load())
    {
        g_running = true;
        g_renderThread = std::thread(RenderThreadMain);
    }
}

JNIEXPORT void JNICALL Java_com_xxy_imguid_ImguiBridge_nativeDestroy(JNIEnv*, jclass)
{
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_running = false;
    }
    g_cv.notify_all();
    if (g_renderThread.joinable())
        g_renderThread.join();
    LOGI("native destroyed");
}

JNIEXPORT void JNICALL
Java_com_xxy_imguid_ImguiBridge_nativeSurfaceCreated(JNIEnv* env, jclass, jobject surface)
{
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr)
    {
        LOGE("ANativeWindow_fromSurface() failed");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_window != nullptr)
            ANativeWindow_release(g_window);
        g_window = window;
        g_surfacePending = true;
    }
    g_cv.notify_all();
}

JNIEXPORT void JNICALL
Java_com_xxy_imguid_ImguiBridge_nativeSurfaceChanged(JNIEnv* env, jclass, jobject surface,
                                                     jint width, jint height)
{
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_window == nullptr)
            g_window = ANativeWindow_fromSurface(env, surface);
        g_width = width;
        g_height = height;
        // The backing buffer size changed -> the EGLSurface has to be recreated.
        g_surfacePending = true;
    }
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_xxy_imguid_ImguiBridge_nativeSurfaceDestroyed(JNIEnv*, jclass)
{
    std::unique_lock<std::mutex> lk(g_mutex);
    if (g_window == nullptr && g_surface == EGL_NO_SURFACE)
        return;
    g_destroyPending = true;
    g_cv.notify_all();
    // Block until the render thread released the surface: the framework may
    // free it as soon as this callback returns.
    g_cv.wait(lk, [] { return !g_destroyPending || !g_running.load(); });
}

JNIEXPORT void JNICALL
Java_com_xxy_imguid_ImguiBridge_nativeTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_touchQueue.size() > 64)
        return;
    g_touchQueue.push_back(TouchEvent{ (int)action, x, y });
}

} // extern "C"
