#include "ApplicationGUI.h"
#include "Image.h"
#include "ClientLayer.h"

bool g_ApplicationRunning = true;

Safira::ApplicationGUI* Safira::CreateApplication(int argc, char** argv) {
        ApplicationSpecification spec;
        spec.name = "Safira Chat Client";
        spec.IconPath = "Safira-Icon.png";
        spec.CustomTitlebar = true;
        spec.CenterWindow = true;

        ApplicationGUI* app = new ApplicationGUI(spec);
        std::shared_ptr<ClientLayer> clientLayer = std::make_shared<ClientLayer>();
        app->PushLayer(clientLayer);
        app->SetMenubarCallback([app, clientLayer]()
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Disconnect"))
                    clientLayer->OnDisconnectButton();

                if (ImGui::MenuItem("Exit"))
                    app->Close();
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