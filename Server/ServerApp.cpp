#include "ApplicationConsole.h"
#include "ServerLayer.h"

bool g_ApplicationRunning = true;

Safira::ApplicationConsole* Safira::CreateApplication(int argc, char** argv) {
    ApplicationSpecification spec;
    spec.name = "Walnut Chat Server 1.0";

    ApplicationConsole* app = new ApplicationConsole(spec);
    app->PushLayer<ServerLayer>();
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