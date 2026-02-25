cmake_minimum_required(VERSION 4.1)
project(PQC_Master_Thesis_2026)

set(CMAKE_CXX_STANDARD 23)

# 1. Fetch GLFW
include(FetchContent)
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        master
        GIT_PROGRESS   TRUE
)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw)

# 2. Find Vulkan
find_package(Vulkan REQUIRED)

# 3. Add the Executable
add_executable(PQC_Master_Thesis_2026
        main.cpp
        src/Application.cpp

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
        )

# 4. Include Directories
target_include_directories(PQC_Master_Thesis_2026 PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/imgui
        ${CMAKE_SOURCE_DIR}/imgui/misc
        ${CMAKE_SOURCE_DIR}/imgui/backends
)

# 5. Link Libraries (Common)
target_link_libraries(PQC_Master_Thesis_2026 PRIVATE
        glfw
        Vulkan::Vulkan
)

# 6. Platform-Specific Configurations
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
        )
endif()