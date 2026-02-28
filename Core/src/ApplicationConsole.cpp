#include "ApplicationConsole.h"
#include "Log.h"
#include <chrono>

extern bool g_ApplicationRunning;

static Safira::ApplicationConsole* s_Instance = nullptr;

namespace Safira {

    ApplicationConsole::ApplicationConsole(const ApplicationSpecification& specification)
        : m_Specification(specification) {
        s_Instance = this;
        Init();
    }

    ApplicationConsole::~ApplicationConsole() {
        Shutdown();
        s_Instance = nullptr;
    }

    ApplicationConsole& ApplicationConsole::Get() {
        return *s_Instance;
    }

    void ApplicationConsole::Init() {
        Log::Init();
    }

    void ApplicationConsole::Shutdown() {
        for (auto& layer : m_LayerStack)
            layer->OnDetach();

        m_LayerStack.clear();
        g_ApplicationRunning = false;
        Log::Shutdown();
    }

    void ApplicationConsole::Run() {
        m_Running = true;

        // Main loop
        while (m_Running) {
            for (auto& layer : m_LayerStack)
                layer->OnUpdate(m_TimeStep);

            if (m_Specification.sleepDuration > 0.0f)
                std::this_thread::sleep_for(std::chrono::milliseconds(m_Specification.sleepDuration));

            float time = GetTime();
            m_FrameTime = time - m_LastFrameTime;
            m_TimeStep = glm::min<float>(m_FrameTime, 0.0333f);
            m_LastFrameTime = time;
        }
    }

    void ApplicationConsole::Close() {
        m_Running = false;
    }

    float ApplicationConsole::GetTime() {
        return m_AppTimer.Elapsed();
    }

}
