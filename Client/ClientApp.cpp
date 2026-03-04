#include "ApplicationGUI.h"
#include "Image.h"
#include "ClientLayer.h"

#include <memory>

bool g_ApplicationRunning = true;

std::unique_ptr<Safira::ApplicationGUI> Safira::CreateApplication(int argc, char** argv) {
    ApplicationSpecification spec;
    spec.name           = "Safira Chat Client";
    spec.IconPath       = "Safira-Icon.png";
    spec.width          = 800;
    spec.height         = 700;
    spec.CustomTitlebar = true;
    spec.CenterWindow   = true;

    auto app = std::make_unique<ApplicationGUI>(spec);
    auto clientLayer = std::make_shared<ClientLayer>();
    app->PushLayer(clientLayer);
    app->SetMenubarCallback([raw = app.get(), clientLayer]() {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Disconnect"))
                clientLayer->OnDisconnectButton();
            if (ImGui::MenuItem("Exit"))
                raw->Close();
            ImGui::EndMenu();
        }
    });
    return app;
}

int main(int argc, char** argv) {
    while (g_ApplicationRunning) {
        auto app = Safira::CreateApplication(argc, argv);
        app->Run();
    }
    return 0;
}