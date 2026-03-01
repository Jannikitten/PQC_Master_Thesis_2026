cmake_minimum_required(VERSION 4.1)
project(PQC_Master_Thesis_2026)

set(CMAKE_CXX_STANDARD 23)

# 1-5. Fetch Dependencies (Keeping your logic exactly as is)
include(FetchContent)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/TheCherno/glfw.git GIT_TAG master)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863)
FetchContent_Declare(gns GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git GIT_TAG master)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.x)
FetchContent_Declare(yaml-cpp GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git GIT_TAG master)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

find_package(absl REQUIRED)
find_package(Protobuf REQUIRED)

FetchContent_MakeAvailable(glfw glm gns spdlog yaml-cpp)

# Critical Mac/Homebrew fix for GNS
if(TARGET GameNetworkingSockets)
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

# 6-7. Find Vulkan, OpenSSL & WolfSSL
find_package(Vulkan REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------
# 8. Define the Shared CORE Library
# ---------------------------------------------------------
# Includes everything in Core/ except App wrappers and Layer specific logic
add_library(CoreLib STATIC
        Core/src/ApplicationGUI.cpp
        Core/src/Image.cpp
        Core/src/Random.cpp
        Core/src/UI.cpp
        Core/src/ConsoleGUI.cpp
        Core/src/Theme.cpp
        Core/src/Log.cpp
        Core/src/Input.cpp
        Core/src/Client.cpp
        Core/src/UserInfo.cpp
        Core/src/StreamReader.cpp
        Core/src/StreamWriter.cpp
        Core/src/NetworkingUtils.cpp
        Core/src/ServerPacket.cpp
        Core/src/BufferStream.cpp
        Core/src/StringUtils.cpp
        Core/src/Server.cpp
        Core/src/ApplicationConsole.cpp
        Core/src/Console.cpp
        Core/include/Console.h
)

# ---------------------------------------------------------
# 9. Define the CLIENT Executable
# ---------------------------------------------------------
add_executable(SafiraClient
        Client/ClientApp.cpp
        Core/src/ClientLayer.cpp

        # ImGui (Located in root/imgui)
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_glfw.cpp
)

# ---------------------------------------------------------
# 10. Define the SERVER Executable
# ---------------------------------------------------------
add_executable(SafiraServer
        Server/ServerApp.cpp
        Core/src/ServerLayer.cpp

        # ImGui (Located in root/imgui)
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_glfw.cpp
)

# ---------------------------------------------------------
# 11. Include Directories & Global Linking
# ---------------------------------------------------------
set(ALL_TARGETS CoreLib SafiraClient SafiraServer)

foreach(TGT IN LISTS ALL_TARGETS)
        target_include_directories(${TGT} PRIVATE
                ${CMAKE_SOURCE_DIR}/Core/include
                ${CMAKE_SOURCE_DIR}/imgui
                ${CMAKE_SOURCE_DIR}/imgui/misc
                ${CMAKE_SOURCE_DIR}/imgui/backends
                ${CMAKE_SOURCE_DIR}/glm
                ${CMAKE_SOURCE_DIR}/spdlog/include
                ${CMAKE_SOURCE_DIR}/Resources
        )

        target_link_libraries(${TGT} PUBLIC
                glfw
                glm::glm
                GameNetworkingSockets::GameNetworkingSockets
                ${Protobuf_LIBRARIES}
                absl::base
                spdlog
                yaml-cpp::yaml-cpp
                OpenSSL::SSL
                OpenSSL::Crypto
                Vulkan::Vulkan
        )

        if(APPLE)
                target_link_libraries(${TGT} PRIVATE
                        "-framework Foundation" "-framework Cocoa" "-framework IOKit"
                        "-framework CoreVideo" "-framework Metal" "-framework QuartzCore"
                )
        elseif(WIN32)
                target_link_libraries(${TGT} PRIVATE User32.lib Gdi32.lib Shell32.lib ws2_32.lib)
        endif()
endforeach()

# Final assembly
target_link_libraries(SafiraClient PRIVATE CoreLib)
target_link_libraries(SafiraServer PRIVATE CoreLib)