#ifndef PQC_MASTER_THESIS_2026_APPLICATIONGUI_H
#define PQC_MASTER_THESIS_2026_APPLICATIONGUI_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "Layer.h"
#include "Image.h"
#include "imgui.h"
#include <vulkan/vulkan.h>

void check_vk_result(VkResult err);
struct GLFWwindow;

namespace Safira {

struct ApplicationSpecification {
    std::string           name             = "Safira Chat";
    uint32_t              width            = 1600;
    uint32_t              height           = 900;
    std::filesystem::path IconPath;
    bool                  WindowResizeable = true;
    bool                  CustomTitlebar   = false;
    bool                  CenterWindow     = false;
};

class ApplicationGUI {
public:
    explicit ApplicationGUI(const ApplicationSpecification& spec = ApplicationSpecification());
    ~ApplicationGUI();

    ApplicationGUI(const ApplicationGUI&)            = delete;
    ApplicationGUI& operator=(const ApplicationGUI&) = delete;

    static ApplicationGUI& Get();

    void Run();
    void Close();

    void SetMenubarCallback(std::function<void()> fn) { m_MenubarCallback = std::move(fn); }

    template <typename T>
    void PushLayer() {
        static_assert(std::is_base_of_v<Layer, T>, "Pushed type is not subclass of Layer!");
        m_LayerStack.emplace_back(std::make_shared<T>())->OnAttach();
    }

    void PushLayer(const std::shared_ptr<Layer>& layer) {
        m_LayerStack.emplace_back(layer);
        layer->OnAttach();
    }

    [[nodiscard]] bool                    IsMaximized()        const;
    [[nodiscard]] float                   GetTime();
    [[nodiscard]] GLFWwindow*             GetWindowHandle()    const { return m_WindowHandle; }
    [[nodiscard]] bool                    IsTitleBarHovered()  const { return m_TitleBarHovered; }
    [[nodiscard]] std::shared_ptr<Image>  GetApplicationIcon() const { return m_AppHeaderIcon; }

    static VkInstance       GetInstance();
    static VkPhysicalDevice GetPhysicalDevice();
    static VkDevice         GetDevice();

    static VkCommandBuffer  GetCommandBuffer(bool begin);
    static void             FlushCommandBuffer(VkCommandBuffer commandBuffer);

    static void    SubmitResourceFree(std::function<void()>&& func);
    static ImFont* GetFont(std::string_view name);

    /// Thread-safe: can be called from network threads, GLFW callbacks, etc.
    template <typename Func>
    void QueueEvent(Func&& func) {
        std::scoped_lock lock(m_EventQueueMutex);
        m_EventQueue.push(std::forward<Func>(func));
    }

private:
    void Init();
    void Shutdown();

    void UI_DrawTitlebar(float& outTitlebarHeight);
    void UI_DrawMenubar();

    /// Decode an embedded PNG byte array into a GPU-uploaded Image.
    static std::shared_ptr<Image> LoadEmbeddedIcon(const unsigned char* data, std::size_t size);

    void Spring(float weight, float spacing);
    void Spring();

    // ── State ────────────────────────────────────────────────────────────────
    ApplicationSpecification m_Specification;
    GLFWwindow*              m_WindowHandle = nullptr;
    bool                     m_Running      = false;

    float m_TimeStep      = 0.0f;
    float m_FrameTime     = 0.0f;
    float m_LastFrameTime = 0.0f;

    bool m_TitleBarHovered = false;

    std::vector<std::shared_ptr<Layer>> m_LayerStack;
    std::function<void()>               m_MenubarCallback;

    std::mutex                         m_EventQueueMutex;
    std::queue<std::function<void()>>  m_EventQueue;

    // Resources
    std::shared_ptr<Image> m_AppHeaderIcon;
    std::shared_ptr<Image> m_IconClose;
    std::shared_ptr<Image> m_IconMinimize;
    std::shared_ptr<Image> m_IconMaximize;
    std::shared_ptr<Image> m_IconRestore;
};

/// Implemented by the application (Client or Server).
std::unique_ptr<ApplicationGUI> CreateApplication(int argc, char** argv);

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_APPLICATIONGUI_H