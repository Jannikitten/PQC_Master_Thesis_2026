#include "Application.h"
#include "Image.h"
#include "ServerLayer.h"

bool g_ApplicationRunning = true;

Safira::Application* Safira::CreateApplication(int argc, char** argv) {
    ApplicationSpecification spec;
    spec.name = "Safira Chat Server 1.0";

    Application* app = new Application(spec);
    app->PushLayer<ServerLayer>();
    app->SetMenubarCallback([app]() {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit"))
            {
                app->Close();
            }
            ImGui::EndMenu();
        }
    });
    return app;
}

int main(const int argc, char** argv) {
    while (g_ApplicationRunning) {
        const auto app = Safira::CreateApplication(argc, argv);
        app->Run();
        delete app;
    }

    return 0;
}