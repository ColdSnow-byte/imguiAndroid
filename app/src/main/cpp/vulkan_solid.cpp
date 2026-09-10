#include "vulkan_solid.h"

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <ctime>

// Generated at build time by glslc (see CMakeLists.txt / embed_spv.cmake).
#include "solid_frag_spv.h"
#include "solid_vert_spv.h"

#define VK_TAG "VulkanSolid"
#define VK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VK_TAG, __VA_ARGS__)
#define VK_LOGI(...) __android_log_print(ANDROID_LOG_INFO, VK_TAG, __VA_ARGS__)

namespace
{

double NowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/**
 * Vulkan's NDC maps +Y to the *bottom* of the framebuffer, while OpenGL maps it
 * to the top. Negating the second row of the projection keeps the rendered
 * image upright, so the readback can be uploaded as a plain top-down bitmap.
 * Back-face culling is disabled in this pipeline, so the flipped winding does
 * not matter; lighting uses the model matrix and is unaffected.
 */
Mat4 VulkanProjection()
{
    Mat4 p = SolidProjection();
    p.m[1]  = -p.m[1];    // m[column * 4 + row]
    p.m[5]  = -p.m[5];
    p.m[9]  = -p.m[9];
    p.m[13] = -p.m[13];
    return p;
}

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* words, size_t byteSize)
{
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = byteSize;
    info.pCode    = words;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

} // namespace

// ---------------------------------------------------------------------------
// VulkanSolidScene
// ---------------------------------------------------------------------------
bool VulkanSolidScene::InitVulkan()
{
    // vkEnumerateInstanceVersion is a Vulkan 1.1 command, so it has to be
    // resolved dynamically: libvulkan only guarantees the 1.0 core exports.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto vkEnumerateInstanceVersionFn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (vkEnumerateInstanceVersionFn != nullptr)
        vkEnumerateInstanceVersionFn(&loaderVersion);

    VK_LOGI("loader reports Vulkan %u.%u.%u", VK_VERSION_MAJOR(loaderVersion),
            VK_VERSION_MINOR(loaderVersion), VK_VERSION_PATCH(loaderVersion));

    // Ask for 1.1 first and fall back to 1.0 if the loader rejects it.
    const uint32_t requested[] = { VK_API_VERSION_1_1, VK_API_VERSION_1_0 };
    for (uint32_t api : requested)
    {
        VkApplicationInfo appInfo{};
        appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName   = "imgui_overlay";
        appInfo.applicationVersion = 1;
        appInfo.pEngineName        = "imgui-overlay";
        appInfo.engineVersion      = 1;
        appInfo.apiVersion         = api;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        // Headless offscreen rendering: no surface / WSI extensions needed.

        const VkResult res = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (res == VK_SUCCESS)
            break;

        m_instance = VK_NULL_HANDLE;
        if (res != VK_ERROR_INCOMPATIBLE_DRIVER)
        {
            VK_LOGE("vkCreateInstance failed: %d", (int)res);
            return false;
        }
        VK_LOGI("Vulkan 1.1 rejected by the loader (%d), retrying with 1.0", (int)res);
    }

    if (m_instance == VK_NULL_HANDLE)
    {
        VK_LOGE("vkCreateInstance failed for every API version");
        return false;
    }

    // ---- Pick the first physical device that offers a graphics queue -------
    uint32_t gpuCount = 0;
    if (vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr) != VK_SUCCESS || gpuCount == 0)
    {
        VK_LOGE("no Vulkan physical device");
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(m_instance, &gpuCount, gpus.data());

    for (VkPhysicalDevice gpu : gpus)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        if (familyCount == 0)
            continue;

        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());

        for (uint32_t i = 0; i < familyCount; i++)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                continue;

            m_gpu         = gpu;
            m_queueFamily = i;

            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu, &props);
            std::snprintf(m_deviceName, sizeof(m_deviceName), "%s", props.deviceName);
            m_apiVersion = props.apiVersion;
            VK_LOGI("device: %s (Vulkan %u.%u.%u)", m_deviceName,
                    VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion));
            break;
        }
        if (m_gpu != VK_NULL_HANDLE)
            break;
    }

    if (m_gpu == VK_NULL_HANDLE)
    {
        VK_LOGE("no graphics queue family found");
        return false;
    }

    m_depthFormat = PickDepthFormat();
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
    {
        VK_LOGE("no supported depth format");
        return false;
    }

    // ---- Logical device ----------------------------------------------------
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount       = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos    = &queueInfo;
    // No device extensions: we never present to a surface.

    if (vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    // ---- Command pool / buffer / fence -------------------------------------
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_cmdPool) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_cmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_cmd) != VK_SUCCESS)
    {
        VK_LOGE("vkAllocateCommandBuffers failed");
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // first wait returns at once
    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateFence failed");
        return false;
    }

    return true;
}

VkFormat VulkanSolidScene::PickDepthFormat() const
{
    static const VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM,
    };
    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_gpu, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return format;
    }
    return VK_FORMAT_UNDEFINED;
}

bool VulkanSolidScene::CreatePipeline()
{
    // ---- Render pass -------------------------------------------------------
    // The colour attachment ends in TRANSFER_SRC_OPTIMAL so vkCmdCopyImageToBuffer()
    // can run immediately after the pass without an extra barrier.
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    attachments[1].format         = m_depthFormat;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Make the colour write visible to the transfer that follows the pass.
    VkSubpassDependency dependency{};
    dependency.srcSubpass    = 0;
    dependency.dstSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dependency;

    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateRenderPass failed");
        return false;
    }

    // ---- Pipeline layout: two mat4 in push constants, no descriptor sets ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(float) * 32;   // mvp + model

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS)
    {
        VK_LOGE("vkCreatePipelineLayout failed");
        return false;
    }

    // ---- Shaders -----------------------------------------------------------
    VkShaderModule vertModule = CreateShaderModule(m_device, kSolidVertSpv, sizeof(kSolidVertSpv));
    VkShaderModule fragModule = CreateShaderModule(m_device, kSolidFragSpv, sizeof(kSolidFragSpv));
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
    {
        VK_LOGE("vkCreateShaderModule failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    // ---- Vertex input: pos3 + normal3 + color3, non-indexed ----------------
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = 9 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3] = {};
    for (int i = 0; i < 3; i++)
    {
        attributes[i].location = (uint32_t)i;
        attributes[i].binding  = 0;
        attributes[i].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[i].offset   = (uint32_t)(i * 3 * sizeof(float));
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions    = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor are dynamic, so resizing never rebuilds the pipeline.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable        = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = VK_CULL_MODE_NONE;   // depth test only, like the GL path
    raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable         = VK_FALSE;
    raster.lineWidth               = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = VK_TRUE;
    depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable    = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOpEnable     = VK_FALSE;
    blend.attachmentCount   = 1;
    blend.pAttachments      = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &blend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_layout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    const bool ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                              nullptr, &m_pipeline) == VK_SUCCESS;

    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);

    if (!ok)
    {
        VK_LOGE("vkCreateGraphicsPipelines failed");
        return false;
    }
    return true;
}

uint32_t VulkanSolidScene::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool VulkanSolidScene::CreateImage(uint32_t size, VkFormat format, VkImageUsageFlags usage,
                                   VkImageAspectFlags aspect, VkImage& image,
                                   VkDeviceMemory& memory, VkImageView& view)
{
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = format;
    info.extent        = { size, size, 1 };
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &info, nullptr, &image) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, image, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX)
    {
        VK_LOGE("no device local memory for the render target");
        return false;
    }
    if (vkAllocateMemory(m_device, &alloc, nullptr, &memory) != VK_SUCCESS)
    {
        VK_LOGE("vkAllocateMemory (image) failed");
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = aspect;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateImageView failed");
        return false;
    }
    return true;
}

void VulkanSolidScene::DestroyTarget()
{
    if (m_tex != 0)
    {
        glDeleteTextures(1, &m_tex);
        m_tex = 0;
    }

    if (m_framebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_colorView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_colorView, nullptr);
        m_colorView = VK_NULL_HANDLE;
    }
    if (m_colorImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_colorImage, nullptr);
        m_colorImage = VK_NULL_HANDLE;
    }
    if (m_colorMem != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_colorMem, nullptr);
        m_colorMem = VK_NULL_HANDLE;
    }
    if (m_depthView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMem != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_depthMem, nullptr);
        m_depthMem = VK_NULL_HANDLE;
    }
    if (m_readbackPtr != nullptr)
    {
        vkUnmapMemory(m_device, m_readbackMem);
        m_readbackPtr = nullptr;
    }
    if (m_readback != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_readback, nullptr);
        m_readback = VK_NULL_HANDLE;
    }
    if (m_readbackMem != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_readbackMem, nullptr);
        m_readbackMem = VK_NULL_HANDLE;
    }
    m_size = 0;
}

bool VulkanSolidScene::EnsureTarget(int size)
{
    if (m_framebuffer != VK_NULL_HANDLE && m_size == size && m_tex != 0)
        return true;

    DestroyTarget();

    if (!CreateImage((uint32_t)size, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, m_colorImage, m_colorMem, m_colorView))
        return false;

    if (!CreateImage((uint32_t)size, m_depthFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, m_depthImage, m_depthMem, m_depthView))
        return false;

    const VkImageView views[2] = { m_colorView, m_depthView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_renderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments    = views;
    fbInfo.width           = (uint32_t)size;
    fbInfo.height          = (uint32_t)size;
    fbInfo.layers          = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateFramebuffer failed");
        return false;
    }

    // ---- Readback buffer, kept permanently mapped --------------------------
    const VkDeviceSize bytes = (VkDeviceSize)size * (VkDeviceSize)size * 4u;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = bytes;
    bufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_readback) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateBuffer (readback) failed");
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, m_readback, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX)
    {
        VK_LOGE("no host visible memory for the readback buffer");
        return false;
    }
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_readbackMem) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_readback, m_readbackMem, 0) != VK_SUCCESS ||
        vkMapMemory(m_device, m_readbackMem, 0, req.size, 0, &m_readbackPtr) != VK_SUCCESS)
    {
        VK_LOGE("readback buffer setup failed");
        return false;
    }

    // ---- GL texture that receives the copied pixels ------------------------
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_size = size;
    return m_tex != 0;
}

bool VulkanSolidScene::EnsureMesh(SolidType type)
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return false;

    Mesh& mesh = m_meshes[idx];
    if (mesh.buffer != VK_NULL_HANDLE)
        return true;

    std::vector<float> vertices;
    int                faceCount = 0;
    if (!BuildSolidMesh(type, vertices, faceCount) || vertices.empty())
        return false;

    const VkDeviceSize bytes = (VkDeviceSize)vertices.size() * sizeof(float);

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = bytes;
    bufInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &mesh.buffer) != VK_SUCCESS)
    {
        VK_LOGE("vkCreateBuffer (vertex) failed");
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, mesh.buffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX)
    {
        VK_LOGE("no host visible memory for vertex data");
        return false;
    }
    if (vkAllocateMemory(m_device, &alloc, nullptr, &mesh.memory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, mesh.buffer, mesh.memory, 0) != VK_SUCCESS)
    {
        VK_LOGE("vertex buffer setup failed");
        return false;
    }

    void* data = nullptr;
    if (vkMapMemory(m_device, mesh.memory, 0, req.size, 0, &data) != VK_SUCCESS)
        return false;
    std::memcpy(data, vertices.data(), (size_t)bytes);
    vkUnmapMemory(m_device, mesh.memory);

    mesh.vertexCount = (uint32_t)(vertices.size() / 9);   // 9 floats per vertex
    mesh.faceCount   = faceCount;
    return true;
}

ImTextureID VulkanSolidScene::Render(SolidType type, int size, float timeSeconds)
{
    size = ((size + 7) / 8) * 8;
    if (size < 64)
        size = 64;
    if (size > kMaxSize)
        size = kMaxSize;   // the image -> buffer readback is O(pixels)

    if (!m_ready)
    {
        if (m_initTried)
            return ImTextureID_Invalid;   // do not retry (and spam logcat) every frame
        m_initTried = true;
        if (!InitVulkan() || !CreatePipeline())
        {
            VK_LOGE("Vulkan initialisation failed");
            Release();
            return ImTextureID_Invalid;
        }
        m_ready = true;
    }

    if (!EnsureMesh(type))
        return ImTextureID_Invalid;
    if (!EnsureTarget(size))
        return ImTextureID_Invalid;

    const Mesh& mesh  = m_meshes[(int)type];
    const Mat4  model = SolidTumbleMatrix(timeSeconds);
    const Mat4  mvp   = Mat4Multiply(VulkanProjection(),
                                     Mat4Multiply(SolidViewMatrix(), model));

    const double t0 = NowMs();

    // The fence is already signaled here - either from creation or from the
    // wait at the end of the previous frame - so the command buffer is free to
    // reuse. Resetting it only right before the submit keeps it signaled if
    // recording bails out early, which avoids a deadlock on the next frame.
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_cmd, &beginInfo) != VK_SUCCESS)
        return ImTextureID_Invalid;

    VkClearValue clearValues[2] = {};
    clearValues[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };   // transparent
    clearValues[1].depthStencil = { 1.0f, 0u };

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass      = m_renderPass;
    rpBegin.framebuffer     = m_framebuffer;
    rpBegin.renderArea.offset = { 0, 0 };
    rpBegin.renderArea.extent = { (uint32_t)size, (uint32_t)size };
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues    = clearValues;

    vkCmdBeginRenderPass(m_cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)size;
    viewport.height   = (float)size;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = { (uint32_t)size, (uint32_t)size };
    vkCmdSetScissor(m_cmd, 0, 1, &scissor);

    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(m_cmd, 0, 1, &mesh.buffer, &vertexOffset);

    vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp.m);
    vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, model.m);

    vkCmdDraw(m_cmd, mesh.vertexCount, 1, 0, 0);
    vkCmdEndRenderPass(m_cmd);

    // The attachment's final layout is TRANSFER_SRC_OPTIMAL, so it is already
    // in the layout the copy expects.
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = (uint32_t)size;
    region.bufferImageHeight = (uint32_t)size;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = { (uint32_t)size, (uint32_t)size, 1 };
    vkCmdCopyImageToBuffer(m_cmd, m_colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_readback, 1, &region);

    if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS)
        return ImTextureID_Invalid;

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &m_cmd;

    vkResetFences(m_device, 1, &m_fence);
    if (vkQueueSubmit(m_queue, 1, &submit, m_fence) != VK_SUCCESS)
    {
        VK_LOGE("vkQueueSubmit failed");
        return ImTextureID_Invalid;
    }
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);

    m_lastGpuMs = (float)(NowMs() - t0);

    // Hand the pixels to OpenGL ES. A Vulkan framebuffer's row 0 is the top row
    // and glTexSubImage2D() also uploads row 0 first, so no flip is needed.
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, m_readbackPtr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    return (ImTextureID)m_tex;
}

int VulkanSolidScene::FaceCount(SolidType type) const
{
    const int idx = (int)type;
    if (idx < 0 || idx >= (int)SolidType::Count)
        return 0;
    return m_meshes[idx].faceCount;
}

void VulkanSolidScene::Release()
{
    if (m_tex != 0)
    {
        glDeleteTextures(1, &m_tex);
        m_tex = 0;
    }

    DestroyTarget();

    for (Mesh& mesh : m_meshes)
    {
        if (mesh.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, mesh.buffer, nullptr);
            mesh.buffer = VK_NULL_HANDLE;
        }
        if (mesh.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, mesh.memory, nullptr);
            mesh.memory = VK_NULL_HANDLE;
        }
        mesh.vertexCount = 0;
        mesh.faceCount   = 0;
    }

    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_fence != VK_NULL_HANDLE)
    {
        vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }
    if (m_cmdPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
        m_cmdPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_cmd         = VK_NULL_HANDLE;
    m_gpu         = VK_NULL_HANDLE;
    m_queue       = VK_NULL_HANDLE;
    m_queueFamily = 0;
    m_depthFormat = VK_FORMAT_UNDEFINED;
    m_ready       = false;
    m_lastGpuMs   = 0.0f;
}
