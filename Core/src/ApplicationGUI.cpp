#include "ApplicationGUI.h"
#include "UI.h"
#include "Log.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "Theme.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>
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

void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
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
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
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

        UI::SetSafiraTheme();

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
    // UI_DrawTitlebar — now includes a chat-panel toggle on the right
    //
    // On macOS the native traffic-light buttons sit on the LEFT of the
    // titlebar, so we place our toggle button on the RIGHT.
    // ─────────────────────────────────────────────────────────────────────────
    void ApplicationGUI::UI_DrawTitlebar(float& outTitlebarHeight) {
        const float titlebarHeight = 58.0f;
        const bool isMaximized = IsMaximized();
        float titlebarVerticalOffset = isMaximized ? -6.0f : 0.0f;
        const ImVec2 windowPadding = ImGui::GetCurrentWindow()->WindowPadding;

        ImGui::SetCursorPos(ImVec2(windowPadding.x, windowPadding.y + titlebarVerticalOffset));
        const ImVec2 titlebarMin = ImGui::GetCursorScreenPos();
        const ImVec2 titlebarMax = {
            ImGui::GetCursorScreenPos().x + ImGui::GetWindowWidth() - windowPadding.y * 2.0f,
            ImGui::GetCursorScreenPos().y + titlebarHeight
        };
        auto* bgDrawList = ImGui::GetBackgroundDrawList();
        bgDrawList->AddRectFilled(titlebarMin, titlebarMax, UI::Colors::Theme::titlebar);

        const float w = ImGui::GetContentRegionAvail().x;
        constexpr float buttonsAreaWidth = 94;

        // Title bar drag area
        ImGui::SetCursorPos(ImVec2(windowPadding.x, windowPadding.y + titlebarVerticalOffset));
        ImGui::InvisibleButton("##titleBarDragZone", ImVec2(w - buttonsAreaWidth, titlebarHeight));
        m_TitleBarHovered = ImGui::IsItemHovered();

        if (isMaximized) {
            float windowMousePosY = ImGui::GetMousePos().y - ImGui::GetCursorScreenPos().y;
            if (windowMousePosY >= 0.0f && windowMousePosY <= 5.0f)
                m_TitleBarHovered = true;
        }

        // Draw Menubar
        if (m_MenubarCallback) {
            ImGui::SetNextItemAllowOverlap();
            const float logoHorizontalOffset = 16.0f * 2.0f + 48.0f + windowPadding.x;
            ImGui::SetCursorPos(ImVec2(logoHorizontalOffset, 6.0f + titlebarVerticalOffset));
            UI_DrawMenubar();
            if (ImGui::IsItemHovered())
                m_TitleBarHovered = false;
        }

        // Centered Window title
        {
            ImVec2 currentCursorPos = ImGui::GetCursorPos();
            ImVec2 textSize = ImGui::CalcTextSize(m_Specification.name.c_str());
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetWindowWidth() * 0.5f - textSize.x * 0.5f,
                2.0f + windowPadding.y + 6.0f));
            ImGui::Text("%s", m_Specification.name.c_str());
            ImGui::SetCursorPos(currentCursorPos);
        }

        // ── Chat toggle button (right side) ─────────────────────────────────
        {
            constexpr float btnSize   = 30.0f;
            constexpr float rightPad  = 16.0f;
            const float btnX = ImGui::GetWindowWidth() - windowPadding.x
                               - rightPad - btnSize;
            const float btnY = windowPadding.y + titlebarVerticalOffset
                               + (titlebarHeight - btnSize) * 0.5f;

            ImGui::SetCursorPos({ btnX, btnY });

            // Button styling — gold when active, dim when hidden
            const ImVec4 colN = m_ChatPanelVisible
                ? ImVec4{ 0.85f, 0.73f, 0.42f, 0.25f }
                : ImVec4{ 0.30f, 0.30f, 0.30f, 0.25f };
            const ImVec4 colH = ImVec4{ 0.85f, 0.73f, 0.42f, 0.45f };
            const ImVec4 colA = ImVec4{ 0.85f, 0.73f, 0.42f, 0.60f };

            ImGui::PushStyleColor(ImGuiCol_Button,        colN);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  colH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   colA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnSize * 0.3f);

            if (ImGui::Button("##ChatToggle", { btnSize, btnSize }))
                m_ChatPanelVisible = !m_ChatPanelVisible;

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            // Draw a chat-bubble icon on the button
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 bMin = ImGui::GetItemRectMin();
            const ImVec2 bCen = {
                bMin.x + btnSize * 0.5f,
                bMin.y + btnSize * 0.5f
            };

            const ImU32 iconCol = m_ChatPanelVisible
                ? IM_COL32(218, 185, 107, 230)
                : IM_COL32(170, 170, 170, 160);

            // Bubble body (rounded rect)
            const float bw = 14.0f, bh = 10.0f;
            const ImVec2 bubMin = { bCen.x - bw * 0.5f, bCen.y - bh * 0.5f - 1.0f };
            const ImVec2 bubMax = { bCen.x + bw * 0.5f, bCen.y + bh * 0.5f - 1.0f };
            dl->AddRectFilled(bubMin, bubMax, iconCol, 3.0f);

            // Tail (small triangle at bottom-left)
            dl->AddTriangleFilled(
                { bubMin.x + 3.0f, bubMax.y },
                { bubMin.x + 0.0f, bubMax.y + 4.0f },
                { bubMin.x + 7.0f, bubMax.y },
                iconCol);

            if (ImGui::IsItemHovered()) {
                m_TitleBarHovered = false;
                ImGui::SetTooltip(m_ChatPanelVisible ? "Hide chat panel" : "Show chat panel");
            }
        }

        // Spring spacers for layout (keep existing Mac-compatible positioning)
        Spring(-1.0f, 15.0f);
        Spring(-1.0f, 18.0f);

        outTitlebarHeight = titlebarHeight;
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
        dl->AddCircleFilled(center, radius, IM_COL32(218, 185, 107, 16), 48);
        dl->AddCircle(center, radius,       IM_COL32(218, 185, 107, 28), 48, 2.0f);

        // Inner ring
        dl->AddCircle(center, radius - 8.0f, IM_COL32(218, 185, 107, 14), 48, 1.5f);

        // Large "S" — use the Bold font at a scaled-up size
        ImFont* bold = GetFont("Bold");
        ImFont* font = bold ? bold : ImGui::GetFont();
        constexpr float fontSize = 72.0f;
        const char* letter = "S";
        ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, letter);
        dl->AddText(font, fontSize,
            { center.x - sz.x * 0.5f, center.y - sz.y * 0.5f },
            IM_COL32(218, 185, 107, 32), letter);

        // Tagline below the circle
        const char* tagline = "Post-Quantum Secure Messaging";
        ImGui::PushFont(font);
        ImVec2 tagSz = ImGui::CalcTextSize(tagline);
        ImGui::PopFont();
        dl->AddText(font, font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "X").y,
            { center.x - tagSz.x * 0.5f, center.y + radius + 22.0f },
            IM_COL32(160, 160, 160, 35), tagline);
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

        // Dark clear colour matching the Safira theme background
        ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
        ImGuiIO& io = ImGui::GetIO();

        while (!glfwWindowShouldClose(m_WindowHandle) && m_Running)
        {
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
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);

                ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4{ 0, 0, 0, 0 });
                ImGui::Begin("SafiraMainWindow", nullptr, window_flags);
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize
                ImGui::PopStyleVar(2);  // WindowRounding, WindowBorderSize(outer)

                // Window border
                {
                    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(50, 50, 50, 255));
                    if (!isMaximized)
                        UI::RenderWindowOuterBorders(ImGui::GetCurrentWindow());
                    ImGui::PopStyleColor();
                }

                // Custom titlebar
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