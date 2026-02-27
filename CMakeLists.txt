cmake_minimum_required(VERSION 4.1)
project(PQC_Master_Thesis_2026)

set(CMAKE_CXX_STANDARD 23)

# 1. Fetch GLFW
include(FetchContent)
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/TheCherno/glfw.git
        GIT_TAG        master
        GIT_PROGRESS   TRUE
)

# 2. Fetch GLM
FetchContent_Declare(
        glm
        GIT_REPOSITORY	https://github.com/g-truc/glm.git
        GIT_TAG 	0af55ccecd98d4e5a8d1fad7de25ba429d60e863 #refs/tags/1.0.1
)

# Set GLFW options before making it available
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

find_package(absl REQUIRED)
find_package(Protobuf REQUIRED)

# 3. Fetch GameNetworkingSockets
FetchContent_Declare(
        gns
        GIT_REPOSITORY	https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG 	master
)

# 4. Fetch spdlog
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY	https://github.com/gabime/spdlog.git
        GIT_TAG 	v1.x
)

# 5. Fetch yaml-cpp
FetchContent_Declare(
        yaml-cpp
        GIT_REPOSITORY	https://github.com/jbeder/yaml-cpp.git
        GIT_TAG 	master
)

FetchContent_MakeAvailable(glfw glm gns spdlog yaml-cpp)

if(TARGET GameNetworkingSockets)
        # This tells the linker to look in Homebrew's lib folder and
        # link all potential abseil logging components by filename
        target_link_libraries(GameNetworkingSockets PRIVATE
                "-L/opt/homebrew/lib"
                absl_log_internal_check_op
                absl_log_internal_message
                absl_log_internal_nullguard
                absl_log_globals
                absl_base
                absl_throw_delegate
                absl_raw_logging_internal
        )
endif()

# 6. Find Vulkan
find_package(Vulkan REQUIRED)

# 7. Find OpenSSL
find_package(OpenSSL REQUIRED)

# 8. Add the Executable
add_executable(PQC_Master_Thesis_2026
        main.cpp

        # ImGui Core
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp

        # ImGui Extras
        imgui/misc/cpp/imgui_stdlib.cpp

        # ImGui Backends
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_glfw.cpp

        # Source files
        src/Application.cpp
        src/Image.cpp
        src/Random.cpp
        src/UI.cpp
        src/Console.cpp
        src/Theme.cpp
        src/Log.cpp
        src/Input.cpp
        src/ClientLayer.cpp
        src/Client.cpp
        src/UserInfo.cpp
        src/StreamReader.cpp
        src/StreamWriter.cpp
        src/NetworkingUtils.cpp
        src/ServerPacket.cpp
        src/BufferStream.cpp
        src/StringUtils.cpp
)

# 9. Include Directories
target_include_directories(PQC_Master_Thesis_2026 PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/resources
        ${CMAKE_SOURCE_DIR}/imgui
        ${CMAKE_SOURCE_DIR}/imgui/misc
        ${CMAKE_SOURCE_DIR}/imgui/backends
        ${CMAKE_SOURCE_DIR}/glm
        ${CMAKE_SOURCE_DIR}/glm/glm
        ${CMAKE_SOURCE_DIR}/GameNetworkingSockets
        ${CMAKE_SOURCE_DIR}/spdlog
        ${CMAKE_SOURCE_DIR}/spdlog/include
)

# 10. Link Libraries (Common)
target_link_libraries(PQC_Master_Thesis_2026 PRIVATE
        glfw
        glm::glm
        GameNetworkingSockets::GameNetworkingSockets
        ${Protobuf_LIBRARIES}
        absl::log_internal_check_op  # This is the most likely candidate for your error
        absl::base
        spdlog
        yaml-cpp::yaml-cpp
        OpenSSL::SSL
        OpenSSL::Crypto
        Vulkan::Vulkan
)

# 11. Platform-Specific Configurations
if(APPLE)
        target_link_libraries(PQC_Master_Thesis_2026 PRIVATE
                "-framework Foundation"
                "-framework Cocoa"
                "-framework IOKit"
                "-framework CoreVideo"
                "-framework Metal"
                "-framework QuartzCore"
        )
elseif(WIN32)
        target_link_libraries(PQC_Master_Thesis_2026 PRIVATE
                User32.lib
                Gdi32.lib
                Shell32.lib
                ws2_32.lib
        )
endif()