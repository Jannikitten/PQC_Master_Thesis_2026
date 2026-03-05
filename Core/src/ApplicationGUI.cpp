#include "ApplicationGUI.h"
#include "UI.h"
#include "Log.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "Theme.h"
#include "MacOSWindow.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <atomic>
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>
#include <chrono>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <spdlog/spdlog.h>

// Embedded font
#include "Roboto-Regular.embed"
#include "Roboto-Bold.embed"
#include "Roboto-Italic.embed"

extern bool g_ApplicationRunning;

// Volk headers
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

//#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Data
static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Per-frame-in-flight
static std::vector<std::vector<VkCommandBuffer>> s_AllocatedCommandBuffers;
static std::vector<std::vector<std::function<void()>>> s_ResourceFreeQueue;

// Unlike g_MainWindowData.FrameIndex, this is not the the swapchain image index
// and is always guaranteed to increase (eg. 0, 1, 2, 0, 1, 2)
static uint32_t s_CurrentFrameIndex = 0;

static Safira::ApplicationGUI* s_Instance = nullptr;
static std::unordered_map<std::string, ImFont*> s_Fonts;
static std::atomic<int> s_VulkanFatalError { VK_SUCCESS };

void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) {
        if (s_VulkanFatalError.exchange(static_cast<int>(err), std::memory_order_acq_rel) == VK_SUCCESS) {
            spdlog::critical("[vulkan] fatal device error: {}", static_cast<int>(err));
            if (s_Instance)
                s_Instance->Close();
            g_ApplicationRunning = false;
        }
    }
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
{
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix; // Unused arguments
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif // APP_USE_VULKAN_DEBUG_REPORT

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // Enabling validation layers
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

        // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    {
        // Chat UI uses many dynamic textures (avatars, crop previews, icons).
        // Keep this comfortably above the backend minimum to avoid
        // VK_ERROR_OUT_OF_POOL_MEMORY during normal usage.
        constexpr uint32_t kDescriptorSamplerPoolSize = 512;
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSamplerPoolSize },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->Surface = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_COUNTOF(present_modes));

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
}

static void CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow(ImGui_ImplVulkanH_Window* wd)
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, wd, g_Allocator);
    vkDestroySurfaceKHR(g_Instance, wd->Surface, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);

        // This frame slot has completed on GPU; release deferred resources.
        s_CurrentFrameIndex = wd->FrameIndex;
        for (auto& func : s_ResourceFreeQueue[s_CurrentFrameIndex])
            func();
        s_ResourceFreeQueue[s_CurrentFrameIndex].clear();
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}


// ═════════════════════════════════════════════════════════════════════════════
// Safira namespace — refactored
//
// Changes from original:
//   • DockSpace removed — layers render as fixed children of the main window
//   • UI_DrawTitlebar gains a chat-toggle button (right side, Mac-friendly)
//   • DrawFadedLogo() renders the branded watermark when chat is hidden
//   • Clear color darkened to match the Safira dark theme
// ═════════════════════════════════════════════════════════════════════════════

namespace Safira {
#include "Walnut-Icon.embed"
#include "WindowImages.embed"

    // ─────────────────────────────────────────────────────────────────────────
    // LoadEmbeddedIcon
    // ─────────────────────────────────────────────────────────────────────────
    std::shared_ptr<Image> ApplicationGUI::LoadEmbeddedIcon(const unsigned char* data,
                                                            std::size_t size) {
        uint32_t w, h;
        void* pixels = Image::Decode(data, size, w, h);
        auto image = std::make_shared<Image>(w, h, ImageFormat::RGBA, pixels);
        free(pixels);
        return image;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Ctor / Dtor / Get
    // ─────────────────────────────────────────────────────────────────────────
    ApplicationGUI::ApplicationGUI(const ApplicationSpecification& specification)
        : m_Specification(specification) {
        s_Instance = this;
        Init();
    }

    ApplicationGUI::~ApplicationGUI() {
        Shutdown();
        s_Instance = nullptr;
    }

    ApplicationGUI& ApplicationGUI::Get() {
        return *s_Instance;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Init
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::Init() {
        glfwSetErrorCallback(glfw_error_callback);
        if (!glfwInit()) {
            spdlog::error("Could not initialize GLFW!");
            return;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        if (m_Specification.CustomTitlebar)
            glfwWindowHint(GLFW_TITLEBAR, false);

        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);

        float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
        m_WindowHandle = glfwCreateWindow(
            (int)(m_Specification.width * main_scale),
            (int)(m_Specification.height * main_scale),
            m_Specification.name.c_str(),
            nullptr, nullptr);

        int monitorX, monitorY;
        glfwGetMonitorPos(primaryMonitor, &monitorX, &monitorY);
        if (m_Specification.CenterWindow) {
            glfwSetWindowPos(m_WindowHandle,
                monitorX + (videoMode->width - m_Specification.width) / 2,
                monitorY + (videoMode->height - m_Specification.height) / 2);
            glfwSetWindowAttrib(m_WindowHandle, GLFW_RESIZABLE,
                m_Specification.WindowResizeable ? GLFW_TRUE : GLFW_FALSE);
        }

        if (!glfwVulkanSupported()) {
            spdlog::error("GLFW: Vulkan not supported!");
            return;
        }

        glfwSetWindowUserPointer(m_WindowHandle, this);
        glfwSetTitlebarHitTestCallback(m_WindowHandle,
            [](GLFWwindow* window, int, int, int* hit) {
                auto* app = static_cast<ApplicationGUI*>(glfwGetWindowUserPointer(window));
                *hit = app->IsTitleBarHovered();
            });

        ImVector<const char*> extensions;
        uint32_t extensions_count = 0;
        const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
        for (uint32_t i = 0; i < extensions_count; i++)
            extensions.push_back(glfw_extensions[i]);
        SetupVulkan(extensions);

        VkSurfaceKHR surface;
        VkResult err = glfwCreateWindowSurface(g_Instance, m_WindowHandle, g_Allocator, &surface);
        check_vk_result(err);

        int w, h;
        glfwGetFramebufferSize(m_WindowHandle, &w, &h);
        ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
        SetupVulkanWindow(wd, surface, w, h);

        s_AllocatedCommandBuffers.resize(wd->ImageCount);
        s_ResourceFreeQueue.resize(wd->ImageCount);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding    = ImVec2(10.0f, 10.0f);
        style.FramePadding     = ImVec2(8.0f, 6.0f);
        style.ItemSpacing      = ImVec2(6.0f, 6.0f);
        style.ChildRounding    = 6.0f;
        style.PopupRounding    = 6.0f;
        style.FrameRounding    = 6.0f;
        style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

        style.ScaleAllSizes(main_scale);
        style.FontScaleDpi = main_scale;

        ImGui_ImplGlfw_InitForVulkan(m_WindowHandle, true);
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance       = g_Instance;
        init_info.PhysicalDevice = g_PhysicalDevice;
        init_info.Device         = g_Device;
        init_info.QueueFamily    = g_QueueFamily;
        init_info.Queue          = g_Queue;
        init_info.PipelineCache  = g_PipelineCache;
        init_info.DescriptorPool = g_DescriptorPool;
        init_info.MinImageCount  = g_MinImageCount;
        init_info.ImageCount     = wd->ImageCount;
        init_info.Allocator      = g_Allocator;
        init_info.PipelineInfoMain.RenderPass  = wd->RenderPass;
        init_info.PipelineInfoMain.Subpass     = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = check_vk_result;
        ImGui_ImplVulkan_Init(&init_info);

        ImFontConfig fontConfig;
        fontConfig.FontDataOwnedByAtlas = false;
        ImFont* robotoFont = io.Fonts->AddFontFromMemoryTTF(
            (void*)g_RobotoRegular, sizeof(g_RobotoRegular), 20.0f, &fontConfig);
        s_Fonts["Default"] = robotoFont;
        s_Fonts["Bold"]    = io.Fonts->AddFontFromMemoryTTF(
            (void*)g_RobotoBold, sizeof(g_RobotoBold), 20.0f, &fontConfig);
        s_Fonts["Italic"]  = io.Fonts->AddFontFromMemoryTTF(
            (void*)g_RobotoItalic, sizeof(g_RobotoItalic), 20.0f, &fontConfig);
        io.FontDefault = robotoFont;

        m_AppHeaderIcon = LoadEmbeddedIcon(g_WalnutIcon,         sizeof(g_WalnutIcon));
        m_IconMinimize  = LoadEmbeddedIcon(g_WindowMinimizeIcon, sizeof(g_WindowMinimizeIcon));
        m_IconMaximize  = LoadEmbeddedIcon(g_WindowMaximizeIcon, sizeof(g_WindowMaximizeIcon));
        m_IconRestore   = LoadEmbeddedIcon(g_WindowRestoreIcon,  sizeof(g_WindowRestoreIcon));
        m_IconClose     = LoadEmbeddedIcon(g_WindowCloseIcon,    sizeof(g_WindowCloseIcon));

        glfwShowWindow(m_WindowHandle);

        // macOS: hide native title text, make titlebar transparent,
        // match background colour to the current theme,
        // and centre the traffic-light buttons in our 48px titlebar.
        {
            ImVec4 bg = Theme::Get().ClearColor();
            MacOS_StyleWindow(m_WindowHandle, bg.x, bg.y, bg.z, 48.0f);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Shutdown
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::Shutdown() {
        for (const auto& layer : m_LayerStack)
            layer->OnDetach();
        m_LayerStack.clear();

        m_AppHeaderIcon.reset();
        m_IconClose.reset();
        m_IconMinimize.reset();
        m_IconMaximize.reset();
        m_IconRestore.reset();

        const VkResult err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);

        for (auto& queue : s_ResourceFreeQueue) {
            for (auto& func : queue)
                func();
        }
        s_ResourceFreeQueue.clear();

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        CleanupVulkanWindow(&g_MainWindowData);
        CleanupVulkan();

        glfwDestroyWindow(m_WindowHandle);
        glfwTerminate();

        g_ApplicationRunning = false;
    }


    // ─────────────────────────────────────────────────────────────────────────
    // UI_DrawTitlebar
    //
    // Layout:
    //   LEFT:   avatar · username · clickable status dot (12px from edge)
    //   RIGHT:  [Logout]  [chat-toggle]
    //   No center content. No menubar.
    // ─────────────────────────────────────────────────────────────────────────
    //
    //  Phase 1: UI_DrawTitlebar — background, avatar, text, dot (visual only)
    //           Runs inside SafiraMainWindow BEFORE layers.
    //
    //  Phase 2: UI_DrawTitlebarButtons — transparent overlay window with the
    //           actual interactive buttons (status dot, chat toggle, logout).
    //           Runs AFTER all layers so it has highest z-order.
    //
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::UI_DrawTitlebar(float& outTitlebarHeight) {
        const float titlebarHeight = 48.0f;
        const bool isMaximized = IsMaximized();
        const float vOff = isMaximized ? -6.0f : 0.0f;
        const ImVec2 wPad = ImGui::GetCurrentWindow()->WindowPadding;

        // ── Titlebar background ──────────────────────────────────────────────
        ImGui::SetCursorPos({ wPad.x, wPad.y + vOff });
        const ImVec2 tbMin = ImGui::GetCursorScreenPos();
        const ImVec2 tbMax = {
            tbMin.x + ImGui::GetWindowWidth() - wPad.y * 2.0f,
            tbMin.y + titlebarHeight
        };

        // Cache for phase 2
        // The overlay and centering should span from the actual window top
        // edge (not offset by WindowPadding) to the separator line.
        const float windowTopY = tbMin.y - wPad.y;
        m_CachedTbMin     = { tbMin.x, windowTopY };
        m_CachedTbMax     = tbMax;
        m_CachedTbCenterY = (windowTopY + tbMax.y) * 0.5f;
        m_CachedShowDot  = false;

        auto* bgDl = ImGui::GetBackgroundDrawList();
        bgDl->AddRectFilled({ tbMin.x, tbMin.y - wPad.y }, tbMax, Theme::Get().BgTitlebar);
        // Separator line between titlebar and content below
        bgDl->AddLine({ tbMin.x, tbMax.y }, { tbMax.x, tbMax.y },
                       Theme::Get().Separator, 1.0f);

        // ── Drag detection (mouse-rect only, no InvisibleButton) ────────────
        //    Exclude the macOS traffic-light button area from drag hits.
        {
            const float trafficW = MacOS_GetTrafficLightWidth(m_WindowHandle);
            ImVec2 dragMin = { tbMin.x + trafficW, tbMin.y };
            m_TitleBarHovered = ImGui::IsMouseHoveringRect(dragMin, tbMax, false);
        }
        if (isMaximized) {
            float mouseY = ImGui::GetMousePos().y - tbMin.y;
            if (mouseY >= 0.0f && mouseY <= 5.0f)
                m_TitleBarHovered = true;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* bold = GetFont("Bold");
        ImFont* body = GetFont("Default");
        if (!body) body = ImGui::GetFont();

        const float tbCenterY = m_CachedTbCenterY;

        // ── AFK auto-detection ───────────────────────────────────────────────
        {
            const ImGuiIO& io = ImGui::GetIO();
            bool anyActivity = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f
                               || io.MouseWheel != 0.0f
                               || !io.InputQueueCharacters.empty();
            for (int m = 0; m < IM_ARRAYSIZE(io.MouseDown); ++m)
                if (io.MouseDown[m]) { anyActivity = true; break; }
            if (!anyActivity) {
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
                    if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k)))
                        { anyActivity = true; break; }
            }

            if (anyActivity) {
                m_LastActivityTime = std::chrono::steady_clock::now();
                if (m_UserManualAway)
                    m_UserManualAway = false;
            }

            const auto elapsed = std::chrono::steady_clock::now() - m_LastActivityTime;
            const float secs = std::chrono::duration<float>(elapsed).count();
            m_TitlebarUserOnline = m_TitlebarConnected
                                   && !m_UserManualAway
                                   && (secs < m_AfkTimeoutSeconds);
        }

        // ── LEFT: User profile — visuals only (avatar, name, dot circle) ────
        {
            // On macOS the native close / minimise / zoom buttons sit in the
            // top-left corner.  Shift our content to the right of them.
            const float trafficW = MacOS_GetTrafficLightWidth(m_WindowHandle);
            const float leftX = tbMin.x + (trafficW > 0.0f ? trafficW : 12.0f);
            constexpr float avatarR = 13.0f;
            const float ax = leftX + avatarR;

            if (m_TitlebarAvatarTex) {
                dl->AddImageRounded(
                    m_TitlebarAvatarTex,
                    { ax - avatarR, tbCenterY - avatarR },
                    { ax + avatarR, tbCenterY + avatarR },
                    { 0, 0 }, { 1, 1 },
                    Theme::Get().AvatarImageTint, avatarR);
            } else {
                dl->AddCircleFilled({ ax, tbCenterY }, avatarR,
                    Theme::Get().Accent, 24);

                char letter = m_TitlebarUserName.empty()
                    ? 'S' : (char)toupper(m_TitlebarUserName[0]);
                char buf[2] = { letter, '\0' };

                if (bold) ImGui::PushFont(bold);
                ImVec2 lsz = ImGui::CalcTextSize(buf);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { ax - lsz.x * 0.5f, tbCenterY - lsz.y * 0.5f },
                    Theme::Get().AccentText, buf);
                if (bold) ImGui::PopFont();
            }

            if (!m_TitlebarUserName.empty()) {
                const float textX = leftX + avatarR * 2.0f + 8.0f;

                if (body) ImGui::PushFont(body);
                ImVec2 nameSz = ImGui::CalcTextSize(m_TitlebarUserName.c_str());
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    { textX, tbCenterY - nameSz.y * 0.5f },
                    Theme::Get().TextTitlebar,
                    m_TitlebarUserName.c_str());
                if (body) ImGui::PopFont();

                // Status dot — visual circle (interaction is in phase 2)
                const float dotX  = textX + nameSz.x + 10.0f;
                const float dotR  = 4.5f;
                const float cx = dotX + dotR;

                const bool isAway   = m_TitlebarConnected && !m_TitlebarUserOnline;
                const bool isOffline = !m_TitlebarConnected;
                ImU32 dotCol;
                if (isOffline)       dotCol = Theme::Get().StatusDotInactive;
                else if (isAway)     dotCol = Theme::Get().StatusAway;
                else                 dotCol = Theme::Get().StatusOnline;

                dl->AddCircleFilled({ cx, tbCenterY }, dotR, dotCol, 12);

                // Cache dot position for phase 2
                m_CachedDotCx  = cx;
                m_CachedDotCy  = tbCenterY;
                m_CachedShowDot = true;
            }
        }

        outTitlebarHeight = titlebarHeight;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Styled tooltip — matches the right-click context menu appearance
    // ─────────────────────────────────────────────────────────────────────
    static void StyledTooltip(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg,    Theme::Get().BgPopupAlt);
        ImGui::PushStyleColor(ImGuiCol_Border,     Theme::Get().ModalBorder);
        ImGui::PushStyleColor(ImGuiCol_Text,       Theme::Get().TextPrimary);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   { 10.0f, 6.0f });
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // UI_DrawTitlebarButtons — Phase 2: transparent overlay with buttons.
    //
    // Called AFTER all layers render so this window is on top of everything.
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::UI_DrawTitlebarButtons() {
        const ImVec2 tbMin = m_CachedTbMin;
        const ImVec2 tbMax = m_CachedTbMax;
        const float  tbH   = tbMax.y - tbMin.y;
        const float  tbW   = tbMax.x - tbMin.x;

        if (tbH <= 0.0f || tbW <= 0.0f) return;

        // Create a frameless, transparent overlay exactly over the titlebar
        ImGui::SetNextWindowPos(tbMin);
        ImGui::SetNextWindowSize({ tbW, tbH });

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    { 0, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

        const ImGuiWindowFlags flags =
              ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoSavedSettings;

        if (!ImGui::Begin("##TitlebarOverlay", nullptr, flags)) {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            return;
        }

        // All coordinates below are LOCAL to the overlay window.
        // overlay (0,0) == tbMin in screen space.

        constexpr float toggleSz = 30.0f;
        constexpr float rightPad = 14.0f;
        constexpr float gap      = 10.0f;

        // ── Chat toggle (rightmost) ──────────────────────────────────────────
        const float toggleX = tbW - rightPad - toggleSz;
        const float toggleY = (tbH - toggleSz) * 0.5f;

        ImGui::SetCursorPos({ toggleX, toggleY });

        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().GhostBtnBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().GhostBtnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().GhostBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Border,         Theme::Get().Separator);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        if (ImGui::Button("##ChatToggle", { toggleSz, toggleSz }))
            m_ChatPanelVisible = !m_ChatPanelVisible;

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        // Draw bubble icon on top
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 tMin = ImGui::GetItemRectMin();
            const ImVec2 tCen = {
                tMin.x + toggleSz * 0.5f,
                tMin.y + toggleSz * 0.5f
            };
            const ImU32 iconCol = m_ChatPanelVisible
                ? Theme::Get().ToggleIconActive
                : Theme::Get().ToggleIconInactive;

            const float bw = 13.0f, bh = 9.0f;
            const ImVec2 bubMin = { tCen.x - bw*0.5f, tCen.y - bh*0.5f - 1.f };
            const ImVec2 bubMax = { tCen.x + bw*0.5f, tCen.y + bh*0.5f - 1.f };
            dl->AddRectFilled(bubMin, bubMax, iconCol, 3.0f);
            dl->AddTriangleFilled(
                { bubMin.x + 2.5f, bubMax.y },
                { bubMin.x - 0.5f, bubMax.y + 3.5f },
                { bubMin.x + 6.5f, bubMax.y },
                iconCol);
        }
        if (ImGui::IsItemHovered()) {
            m_TitleBarHovered = false;
            StyledTooltip(m_ChatPanelVisible
                ? "Hide chat panel" : "Show chat panel");
        }

        // ── Logout button (left of toggle) ───────────────────────────────────
        //    Ghost icon button matching the chat toggle style.
        //    Draws a "door with arrow" logout icon via DrawList.
        if (!m_TitlebarUserName.empty()) {
            constexpr float logoutSz = 30.0f;
            const float logoutX = toggleX - gap - logoutSz;
            const float logoutY = (tbH - logoutSz) * 0.5f;

            ImGui::SetCursorPos({ logoutX, logoutY });

            // Ghost button — same style as chat toggle
            ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().GhostBtnBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().LogoutBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().LogoutBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Border,         Theme::Get().Separator);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

            if (ImGui::Button("##Logout", { logoutSz, logoutSz })) {
                if (m_OnLogout) m_OnLogout();
            }

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            // Draw logout icon (door + arrow)
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 bMin = ImGui::GetItemRectMin();
                const float cx = bMin.x + logoutSz * 0.5f;
                const float cy = bMin.y + logoutSz * 0.5f;

                const bool hov = ImGui::IsItemHovered();
                const ImU32 col = hov
                    ? Theme::Get().LogoutIconHover
                    : Theme::Get().LogoutIcon;
                const float t = 1.6f;  // stroke thickness

                // Door frame (open rectangle, left side missing)
                //   ┌───┐
                //   │   │
                //   └───┘
                const float dw = 8.0f, dh = 11.0f;
                const float dx = cx + 1.0f;  // shift door right slightly
                const float dTop = cy - dh * 0.5f;
                const float dBot = cy + dh * 0.5f;
                const float dLeft  = dx - dw * 0.5f;
                const float dRight = dx + dw * 0.5f;

                // Top edge
                dl->AddLine({ dLeft, dTop }, { dRight, dTop }, col, t);
                // Right edge
                dl->AddLine({ dRight, dTop }, { dRight, dBot }, col, t);
                // Bottom edge
                dl->AddLine({ dRight, dBot }, { dLeft, dBot }, col, t);
                // Left edge — partial (top and bottom stubs, gap in middle for arrow)
                dl->AddLine({ dLeft, dTop }, { dLeft, cy - 3.0f }, col, t);
                dl->AddLine({ dLeft, cy + 3.0f }, { dLeft, dBot }, col, t);

                // Arrow pointing left (exits through the door gap)
                const float arrowTip = dLeft - 5.0f;
                const float arrowBase = dLeft + 2.0f;
                // Shaft
                dl->AddLine({ arrowBase, cy }, { arrowTip, cy }, col, t);
                // Arrowhead
                dl->AddLine({ arrowTip, cy }, { arrowTip + 3.5f, cy - 3.0f }, col, t);
                dl->AddLine({ arrowTip, cy }, { arrowTip + 3.5f, cy + 3.0f }, col, t);
            }

            if (ImGui::IsItemHovered()) {
                m_TitleBarHovered = false;
                StyledTooltip("Logout");
            }

            // ── Theme toggle button (left of logout) ────────────────────────
            constexpr float themeSz = 30.0f;
            const float themeX = logoutX - gap - themeSz;
            const float themeY = (tbH - themeSz) * 0.5f;

            ImGui::SetCursorPos({ themeX, themeY });

            ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Get().GhostBtnBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  Theme::Get().GhostBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   Theme::Get().GhostBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Border,         Theme::Get().Separator);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

            if (ImGui::Button("##ThemeToggle", { themeSz, themeSz })) {
                Theme::Toggle();
            }

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            // Draw sun or moon icon
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 bMin = ImGui::GetItemRectMin();
                const float icx = bMin.x + themeSz * 0.5f;
                const float icy = bMin.y + themeSz * 0.5f;

                const bool hov = ImGui::IsItemHovered();
                const ImU32 iconCol = hov
                    ? Theme::Get().ToggleIconActive
                    : Theme::Get().ToggleIconInactive;

                if (Theme::IsDark()) {
                    // Sun icon — circle + rays (switch TO light)
                    constexpr float sr = 4.0f;
                    dl->AddCircleFilled({ icx, icy }, sr, iconCol, 16);
                    for (int i = 0; i < 8; ++i) {
                        float angle = i * (3.14159f * 2.0f / 8.0f);
                        float inner = sr + 2.5f;
                        float outer = sr + 5.0f;
                        dl->AddLine(
                            { icx + cosf(angle) * inner, icy + sinf(angle) * inner },
                            { icx + cosf(angle) * outer, icy + sinf(angle) * outer },
                            iconCol, 1.4f);
                    }
                } else {
                    // Moon icon — crescent (switch TO dark)
                    constexpr float mr = 6.0f;
                    dl->AddCircleFilled({ icx, icy }, mr, iconCol, 24);
                    // Cut out a circle to create crescent
                    const ImU32 btnBg = hov
                        ? Theme::Get().GhostBtnHover
                        : Theme::Get().GhostBtnBg;
                    dl->AddCircleFilled(
                        { icx + 3.5f, icy - 2.5f }, mr - 1.0f,
                        btnBg, 24);
                }
            }

            if (ImGui::IsItemHovered()) {
                m_TitleBarHovered = false;
                StyledTooltip(Theme::IsDark()
                    ? "Switch to light mode" : "Switch to dark mode");
            }
        }

        // ── Status dot click area ────────────────────────────────────────────
        if (m_CachedShowDot) {
            // Convert screen-space dot center to overlay-local coords
            const float dotLocalX = m_CachedDotCx - tbMin.x;
            const float dotLocalY = m_CachedDotCy - tbMin.y;
            const float hitSz = 18.0f;

            ImGui::SetCursorPos({
                dotLocalX - hitSz * 0.5f,
                dotLocalY - hitSz * 0.5f });

            if (ImGui::InvisibleButton("##StatusDot", { hitSz, hitSz })) {
                if (m_TitlebarConnected) {
                    m_UserManualAway = !m_UserManualAway;
                    if (!m_UserManualAway)
                        m_LastActivityTime = std::chrono::steady_clock::now();
                }
            }

            if (ImGui::IsItemHovered()) {
                m_TitleBarHovered = false;
                const bool isAway   = m_TitlebarConnected && !m_TitlebarUserOnline;
                const bool isOffline = !m_TitlebarConnected;
                if (isOffline)
                    StyledTooltip("Status: Disconnected");
                else if (isAway)
                    StyledTooltip("Status: Away  (click to go online)");
                else
                    StyledTooltip("Status: Online  (click to go away)");
            }
        }

        // ── Window dragging ───────────────────────────────────────────────────
        //    On macOS, fullSizeContentView breaks GLFW's native hit-test drag.
        //    We detect a click on the titlebar background (m_TitleBarHovered is
        //    false when over any button) and hand it off to the system window
        //    manager via performWindowDragWithEvent — smooth and flicker-free.
        //    On other platforms, fall back to manual glfwSetWindowPos.
        // NEW — drag from titlebar OR any empty background
#ifdef __APPLE__
        {
            bool canDrag = !ImGui::IsAnyItemActive()
                        && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

            if (canDrag && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                MacOS_BeginNativeDrag(m_WindowHandle);
            }
        }
#else
{
    static bool sDragging = false;

    bool canDrag = !ImGui::IsAnyItemActive()
                && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    if (canDrag && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        sDragging = true;

    if (sDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (delta.x != 0.0f || delta.y != 0.0f) {
                int wx, wy;
                glfwGetWindowPos(m_WindowHandle, &wx, &wy);
                glfwSetWindowPos(m_WindowHandle,
                                 wx + (int)delta.x, wy + (int)delta.y);
            }
        } else {
            sDragging = false;
        }
    }
}
#endif

        ImGui::End();
        ImGui::PopStyleColor();  // WindowBg
        ImGui::PopStyleVar(3);   // Padding, BorderSize, Rounding
    }

    // ─────────────────────────────────────────────────────────────────────────
    // UI_DrawMenubar (unchanged)
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::UI_DrawMenubar() {
        if (!m_MenubarCallback) return;

        if (m_Specification.CustomTitlebar) {
            const ImRect menuBarRect = {
                ImGui::GetCursorPos(),
                { ImGui::GetContentRegionAvail().x + ImGui::GetCursorScreenPos().x,
                  ImGui::GetFrameHeightWithSpacing() }
            };
            ImGui::BeginGroup();
            if (UI::BeginMenubar(menuBarRect))
                m_MenubarCallback();
            UI::EndMenubar();
            ImGui::EndGroup();
        } else {
            if (ImGui::BeginMenuBar()) {
                m_MenubarCallback();
                ImGui::EndMenuBar();
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // DrawFadedLogo — large watermark in the content area
    //
    // Renders a faded Safira "S" inside a circle plus the tagline.
    // Drawn with ImDrawList so it sits behind popup modals.
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::DrawFadedLogo() {
        const ImVec2 avail     = ImGui::GetContentRegionAvail();
        const ImVec2 windowPos = ImGui::GetCursorScreenPos();
        const ImVec2 center    = {
            windowPos.x + avail.x * 0.5f,
            windowPos.y + avail.y * 0.45f
        };

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Outer circle
        constexpr float radius = 72.0f;
        dl->AddCircleFilled(center, radius, Theme::Get().AccentFaded, 48);
        dl->AddCircle(center, radius,       Theme::Get().AccentRing, 48, 2.0f);

        // Inner ring
        dl->AddCircle(center, radius - 8.0f, Theme::Get().AccentRingInner, 48, 1.5f);

        // Large "S" — use the Bold font at a scaled-up size
        ImFont* bold = GetFont("Bold");
        ImFont* font = bold ? bold : ImGui::GetFont();
        constexpr float fontSize = 72.0f;
        const char* letter = "S";
        ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, letter);
        dl->AddText(font, fontSize,
            { center.x - sz.x * 0.5f, center.y - sz.y * 0.5f },
            Theme::Get().AccentFaded, letter);

        // Tagline below the circle
        const char* tagline = "Post-Quantum Secure Messaging";
        ImGui::PushFont(font);
        ImVec2 tagSz = ImGui::CalcTextSize(tagline);
        ImGui::PopFont();
        dl->AddText(font, font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "X").y,
            { center.x - tagSz.x * 0.5f, center.y + radius + 22.0f },
            Theme::Get().TextTagline, tagline);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Run — main loop
    //
    // KEY CHANGE: the DockSpace is removed.  Layers render as fixed children
    // inside the main window.  When the chat panel is hidden the faded logo
    // is drawn instead.
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::Run() {
        m_Running = true;

        ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;

        // Clear colour is re-evaluated each frame to reflect theme changes
        ImGuiIO& io = ImGui::GetIO();

        while (!glfwWindowShouldClose(m_WindowHandle) && m_Running)
        {
            ImVec4 clear_color = Theme::Get().ClearColor();
            MacOS_SetWindowColor(m_WindowHandle, clear_color.x, clear_color.y, clear_color.z);
            MacOS_RepositionTrafficLights(m_WindowHandle, 48.0f);
            glfwPollEvents();

            {
                std::scoped_lock lock(m_EventQueueMutex);
                while (!m_EventQueue.empty()) {
                    m_EventQueue.front()();
                    m_EventQueue.pop();
                }
            }

            for (auto& layer : m_LayerStack)
                layer->OnUpdate(m_TimeStep);

            // Resize swap chain?
            int fb_width, fb_height;
            glfwGetFramebufferSize(m_WindowHandle, &fb_width, &fb_height);
            if (fb_width > 0 && fb_height > 0
                && (g_SwapChainRebuild
                    || g_MainWindowData.Width  != fb_width
                    || g_MainWindowData.Height != fb_height))
            {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(
                    g_Instance, g_PhysicalDevice, g_Device, wd,
                    g_QueueFamily, g_Allocator,
                    fb_width, fb_height, g_MinImageCount, 0);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
            if (glfwGetWindowAttrib(m_WindowHandle, GLFW_ICONIFIED) != 0) {
                ImGui_ImplGlfw_Sleep(10);
                continue;
            }

            // ── Start frame ─────────────────────────────────────────────────
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            {
                ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->Pos);
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::SetNextWindowViewport(viewport->ID);

                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

                ImGuiWindowFlags window_flags =
                      ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_NoTitleBar
                    | ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoResize
                    | ImGuiWindowFlags_NoMove
                    | ImGuiWindowFlags_NoBringToFrontOnFocus
                    | ImGuiWindowFlags_NoNavFocus;

                if (!m_Specification.CustomTitlebar && m_MenubarCallback)
                    window_flags |= ImGuiWindowFlags_MenuBar;

                const bool isMaximized = IsMaximized();

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                    isMaximized ? ImVec2(6.0f, 6.0f) : ImVec2(1.0f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

                ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4{ 0, 0, 0, 0 });
                ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Get().ClearColor());
                ImGui::Begin("SafiraMainWindow", nullptr, window_flags);
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize
                ImGui::PopStyleVar(2);  // WindowRounding, WindowBorderSize(outer)

                // Custom titlebar - phase 1: background + visuals only
                if (m_Specification.CustomTitlebar) {
                    float titleBarHeight;
                    UI_DrawTitlebar(titleBarHeight);
                    ImGui::SetCursorPosY(titleBarHeight);
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                }

                if (!m_Specification.CustomTitlebar)
                    UI_DrawMenubar();

                // ── Content area — NO dockspace ─────────────────────────────
                // When the chat panel is hidden, draw the faded watermark.
                // Layers always render (connection modal / invite popups still
                // need to work), but ClientLayer checks IsChatPanelVisible()
                // to skip its main chat window.
                if (!m_ChatPanelVisible)
                    DrawFadedLogo();

                for (auto& layer : m_LayerStack)
                    layer->OnUIRender();

                ImGui::End(); // SafiraMainWindow

                // Custom titlebar - phase 2: interactive buttons in overlay
                // This MUST be after all layer windows so it has the highest
                // z-order and buttons actually receive input.
                if (m_Specification.CustomTitlebar)
                    UI_DrawTitlebarButtons();
            }

            // ── Render & present ────────────────────────────────────────────
            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            const bool is_minimized =
                (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
            if (!is_minimized) {
                wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
                wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
                wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
                wd->ClearValue.color.float32[3] = clear_color.w;
                FrameRender(wd, draw_data);
                FramePresent(wd);
            }

            float time = GetTime();
            m_FrameTime = time - m_LastFrameTime;
            m_TimeStep  = glm::min<float>(m_FrameTime, 0.0333f);
            m_LastFrameTime = time;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Remaining methods — unchanged
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::Close() { m_Running = false; }

    bool ApplicationGUI::IsMaximized() const {
        return (bool)glfwGetWindowAttrib(m_WindowHandle, GLFW_MAXIMIZED);
    }

    float ApplicationGUI::GetTime() {
        return (float)glfwGetTime();
    }

    VkInstance ApplicationGUI::GetInstance()             { return g_Instance;       }
    VkPhysicalDevice ApplicationGUI::GetPhysicalDevice() { return g_PhysicalDevice; }
    VkDevice ApplicationGUI::GetDevice()                 { return g_Device;         }

    VkCommandBuffer ApplicationGUI::GetCommandBuffer(bool /*begin*/) {
        ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
        VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;

        VkCommandBufferAllocateInfo cmdBufAllocateInfo = {};
        cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufAllocateInfo.commandPool = command_pool;
        cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufAllocateInfo.commandBufferCount = 1;

        VkCommandBuffer& command_buffer =
            s_AllocatedCommandBuffers[wd->FrameIndex].emplace_back();
        auto err = vkAllocateCommandBuffers(g_Device, &cmdBufAllocateInfo, &command_buffer);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);

        return command_buffer;
    }

    void ApplicationGUI::FlushCommandBuffer(VkCommandBuffer commandBuffer) {
        constexpr uint64_t DEFAULT_FENCE_TIMEOUT = 100000000000;

        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &commandBuffer;
        auto err = vkEndCommandBuffer(commandBuffer);
        check_vk_result(err);

        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = 0;
        VkFence fence;
        err = vkCreateFence(g_Device, &fenceCreateInfo, nullptr, &fence);
        check_vk_result(err);

        err = vkQueueSubmit(g_Queue, 1, &end_info, fence);
        check_vk_result(err);

        err = vkWaitForFences(g_Device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT);
        check_vk_result(err);

        vkDestroyFence(g_Device, fence, nullptr);
    }

    void ApplicationGUI::SubmitResourceFree(std::function<void()>&& func) {
        s_ResourceFreeQueue[s_CurrentFrameIndex].emplace_back(func);
    }

    ImFont* ApplicationGUI::GetFont(std::string_view name) {
        auto it = s_Fonts.find(std::string(name));
        return (it != s_Fonts.end()) ? it->second : nullptr;
    }

    void ApplicationGUI::Spring(float weight, float spacing) {
        if (weight < 0.0f) {
            const float pos_x = ImGui::GetCursorPosX()
                              + ImGui::GetContentRegionAvail().x - spacing;
            ImGui::SameLine(pos_x);
        } else {
            ImGui::Dummy(ImVec2(spacing, 0.0f));
            ImGui::SameLine();
        }
    }

    void ApplicationGUI::Spring() {
        if (const float x = ImGui::GetContentRegionAvail().x; x > 0.0f) {
            ImGui::Dummy(ImVec2(x, 0.0f));
            ImGui::SameLine();
        }
    }
} // namespace Safira
