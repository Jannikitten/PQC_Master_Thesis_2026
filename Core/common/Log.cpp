#include "Log.h"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"

#include <filesystem>

#define WL_HAS_CONSOLE !WL_DIST

namespace Safira {

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

    void Log::Init() {
        // TODO: Remove this class.
    }

    void Log::Shutdown() {
        s_ClientLogger.reset();
        s_CoreLogger.reset();
        spdlog::drop_all();
    }

}
