#pragma once

#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "solid_geometry.h"

/**
 * Vulkan 1.1 path: renders a tumbling Platonic solid with a real Vulkan
 * graphics pipeline into an offscreen image, then copies the pixels back and
 * hands them to OpenGL ES so ImGui::Image() can display them.
 *
 * This is genuine Vulkan all the way through:
 *   - VkInstance created with apiVersion = VK_API_VERSION_1_1
 *   - physical device / queue family selection
 *   - SPIR-V shaders (compiled offline by glslc, embedded as C arrays)
 *   - VkRenderPass + VkFramebuffer with a colour and a depth attachment
 *   - VkPipeline using push constants, so no descriptor sets are needed
 *   - vkCmdDraw() followed by vkCmdCopyImageToBuffer() readback
 *
 * No swapchain / surface is involved: the overlay is drawn by the OpenGL ES
 * ImGui backend, so Vulkan only renders offscreen and GL does the final
 * glTexSubImage2D() upload.
 */
class VulkanSolidScene
{
public:
    VulkanSolidScene() = default;
    ~VulkanSolidScene() { Release(); }

    VulkanSolidScene(const VulkanSolidScene&) = delete;
    VulkanSolidScene& operator=(const VulkanSolidScene&) = delete;

    /** Must be called while the GL context is still alive. */
    void Release();

    /** Renders one frame at (size x size) and returns the texture to display. */
    ImTextureID Render(SolidType type, int size, float timeSeconds);

    int FaceCount(SolidType type) const;

    /** False when Vulkan could not be initialised on this device. */
    bool Available() const { return m_ready; }

    /** GPU name reported by Vulkan (empty before the first successful frame). */
    const char* DeviceName() const { return m_deviceName; }

    /** Instance API version as a packed Vulkan version number (0 if unknown). */
    uint32_t ApiVersion() const { return m_apiVersion; }

    /** Time spent recording + submitting + waiting for the last frame, in ms. */
    float LastGpuMs() const { return m_lastGpuMs; }

    /** Resolution actually used for the last frame (capped, see kMaxSize). */
    int RenderSize() const { return m_size; }

private:
    struct Mesh
    {
        VkBuffer       buffer      = VK_NULL_HANDLE;
        VkDeviceMemory memory      = VK_NULL_HANDLE;
        uint32_t       vertexCount = 0;
        int            faceCount   = 0;
    };

    bool     InitVulkan();
    bool     CreatePipeline();
    bool     EnsureMesh(SolidType type);
    bool     EnsureTarget(int size);
    void     DestroyTarget();

    VkFormat PickDepthFormat() const;
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    bool CreateImage(uint32_t size, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect, VkImage& image,
                     VkDeviceMemory& memory, VkImageView& view);

    // Upper bound for the resolution slider. The image -> buffer readback is
    // O(pixels), so this trades sharpness against frame time.
    static const int kMaxSize = 1024;

    VkInstance       m_instance    = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu         = VK_NULL_HANDLE;
    VkDevice         m_device      = VK_NULL_HANDLE;
    VkQueue          m_queue       = VK_NULL_HANDLE;
    uint32_t         m_queueFamily = 0;

    VkCommandPool    m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer  m_cmd     = VK_NULL_HANDLE;
    VkFence          m_fence   = VK_NULL_HANDLE;

    VkRenderPass     m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_layout     = VK_NULL_HANDLE;
    VkPipeline       m_pipeline   = VK_NULL_HANDLE;
    VkFormat         m_depthFormat = VK_FORMAT_UNDEFINED;

    VkImage        m_colorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_colorMem   = VK_NULL_HANDLE;
    VkImageView    m_colorView  = VK_NULL_HANDLE;
    VkImage        m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMem   = VK_NULL_HANDLE;
    VkImageView    m_depthView  = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffer = VK_NULL_HANDLE;

    VkBuffer       m_readback    = VK_NULL_HANDLE;
    VkDeviceMemory m_readbackMem = VK_NULL_HANDLE;
    void*          m_readbackPtr = nullptr;

    Mesh   m_meshes[(int)SolidType::Count];

    GLuint m_tex = 0;
    int    m_size = 0;

    bool     m_ready      = false;
    bool     m_initTried  = false;
    char     m_deviceName[256] = {};
    uint32_t m_apiVersion = 0;
    float    m_lastGpuMs  = 0.0f;
};
