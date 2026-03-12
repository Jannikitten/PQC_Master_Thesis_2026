#ifndef PQC_MASTER_THESIS_2026_APPLICATIONCONSOLE_H
#define PQC_MASTER_THESIS_2026_APPLICATIONCONSOLE_H

#include "Layer.h"
#include "Timer.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace Safira {
	struct ApplicationSpecification {
		std::string name = "Safira App";
		uint32_t width = 1600;
		uint32_t height = 900;

		uint64_t sleepDuration = 0;
	};

	class ApplicationConsole {
	public:
		ApplicationConsole(const ApplicationSpecification& applicationSpecification = ApplicationSpecification());
		~ApplicationConsole();

		static ApplicationConsole& Get();

		void Run();

		// No menubar for headless apps
		void SetMenubarCallback(const std::function<void()>& menubarCallback) {}

		template<typename T>
		void PushLayer() {
			static_assert(std::is_base_of<Layer, T>::value, "Pushed type is not subclass of Layer!");
			m_LayerStack.emplace_back(std::make_shared<T>())->OnAttach();
		}

		void PushLayer(const std::shared_ptr<Layer>& layer) { m_LayerStack.emplace_back(layer); layer->OnAttach(); }

		void Close();

		float GetTime();
	private:
		void Init();
		void Shutdown();
	private:
		ApplicationSpecification m_Specification;
		bool m_Running = false;

		float m_TimeStep = 0.0f;
		float m_FrameTime = 0.0f;
		float m_LastFrameTime = 0.0f;

		std::vector<std::shared_ptr<Layer>> m_LayerStack;
		Timer m_AppTimer;
	};

	// Implemented by CLIENT
	ApplicationConsole* CreateApplication(int argc, char** argv);
}

#endif //PQC_MASTER_THESIS_2026_APPLICATIONCONSOLE_H