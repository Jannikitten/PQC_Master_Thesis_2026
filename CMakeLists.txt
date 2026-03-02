cmake_minimum_required(VERSION 4.1)
project(PQC_Master_Thesis_2026)

set(CMAKE_CXX_STANDARD 23)

# 1-5. Fetch Dependencies
include(FetchContent)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/TheCherno/glfw.git GIT_TAG master)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.x)
FetchContent_Declare(yaml-cpp GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git GIT_TAG master)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm spdlog yaml-cpp)

# 6. Find Vulkan & OpenSSL
find_package(Vulkan REQUIRED)
find_package(OpenSSL REQUIRED)

# 7. wolfSSL
add_compile_definitions(
        WOLFSSL_DTLS
        WOLFSSL_DTLS13
        WOLFSSL_DTLS_MTU
        WOLFSSL_EXTRA
        HAVE_MLKEM
        HAVE_MLDSA
)

set(WOLFSSL_DTLS         ON CACHE BOOL "" FORCE)
set(WOLFSSL_DTLS13       ON CACHE BOOL "" FORCE)
set(WOLFSSL_MLKEM        ON CACHE BOOL "" FORCE)
set(WOLFSSL_MLDSA        ON CACHE BOOL "" FORCE)
set(WOLFSSL_EXPERIMENTAL ON CACHE BOOL "" FORCE)
set(WOLFSSL_DTLS_MTU     ON CACHE BOOL "" FORCE)
set(WOLFSSL_DTLS_CH_FRAG ON CACHE BOOL "" FORCE)
set(WOLFSSL_USER_IO      ON CACHE BOOL "" FORCE)
set(WOLFSSL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(WOLFSSL_CRYPT_TESTS  OFF CACHE BOOL "" FORCE)

add_subdirectory(wolfssl)

# ─────────────────────────────────────────────────────────────────────────────
# 8. Botan 3
#
# First try find_package using Botan's own cmake config.  If the config path
# or version string doesn't match exactly (it varies between patch releases),
# fall back to creating the imported target manually from the known install
# layout.  The fallback is always correct as long as Botan_ROOT is right.
# ─────────────────────────────────────────────────────────────────────────────
set(Botan_ROOT "${CMAKE_SOURCE_DIR}/botan/install"
        CACHE PATH "Root of the Botan install prefix (the directory passed to --prefix)")

find_package(Botan 3.11.0
        PATHS "${Botan_ROOT}/lib/cmake/Botan-3.11.0"
        NO_DEFAULT_PATH
        QUIET
)

if(NOT TARGET Botan::Botan)
        message(STATUS "Botan::Botan not created by find_package — building imported target from ${Botan_ROOT}")

        # Locate the library (static preferred; fall back to shared)
        if(EXISTS "${Botan_ROOT}/lib/libbotan-3.a")
                set(_botan_lib  "${Botan_ROOT}/lib/libbotan-3.a")
                set(_botan_type STATIC)
        elseif(EXISTS "${Botan_ROOT}/lib/libbotan-3.so")
                set(_botan_lib  "${Botan_ROOT}/lib/libbotan-3.so")
                set(_botan_type SHARED)
        elseif(EXISTS "${Botan_ROOT}/lib/libbotan-3.dylib")
                set(_botan_lib  "${Botan_ROOT}/lib/libbotan-3.dylib")
                set(_botan_type SHARED
                        Core/include/P2PCredentialGenerator.h)
        else()
                message(FATAL_ERROR
                        "Cannot find Botan library under ${Botan_ROOT}/lib/\n"
                        "Build and install Botan from the submodule first:\n"
                        "  cd botan\n"
                        "  python3 configure.py --prefix=./install --with-tls --with-ml-kem --with-ml-dsa\n"
                        "  make -j\$(nproc) && make install")
        endif()

        add_library(Botan::Botan ${_botan_type} IMPORTED GLOBAL)
        set_target_properties(Botan::Botan PROPERTIES
                IMPORTED_LOCATION             "${_botan_lib}"
                INTERFACE_INCLUDE_DIRECTORIES "${Botan_ROOT}/include/botan-3"
        )
        message(STATUS "Botan::Botan -> ${_botan_lib}")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 9. CoreLib
# ─────────────────────────────────────────────────────────────────────────────
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
        Core/src/ServerPacket.cpp
        Core/src/BufferStream.cpp
        Core/src/StringUtils.cpp
        Core/src/Server.cpp
        Core/src/ApplicationConsole.cpp
        Core/src/Console.cpp
        Core/src/PrivateChatSession.cpp
        Core/include/WolfTypes.h
)

# ─────────────────────────────────────────────────────────────────────────────
# 10. Client executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(SafiraClient
        Client/ClientApp.cpp
        Core/src/ClientLayer.cpp
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_glfw.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
# 11. Server executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(SafiraServer
        Server/ServerApp.cpp
        Core/src/ServerLayer.cpp
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
        imgui/backends/imgui_impl_vulkan.cpp
        imgui/backends/imgui_impl_glfw.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
# 12. Include dirs, compile definitions, link libraries
# ─────────────────────────────────────────────────────────────────────────────
set(ALL_TARGETS CoreLib SafiraClient SafiraServer)

foreach(TGT IN LISTS ALL_TARGETS)
        target_include_directories(${TGT} PRIVATE
                ${CMAKE_SOURCE_DIR}/Core/include
                ${CMAKE_SOURCE_DIR}/imgui
                ${CMAKE_SOURCE_DIR}/imgui/misc
                ${CMAKE_SOURCE_DIR}/imgui/backends
                ${CMAKE_SOURCE_DIR}/Resources
                ${CMAKE_BINARY_DIR}/wolfssl
                "${Botan_ROOT}/include/botan-3"
        )

        target_compile_definitions(${TGT} PRIVATE
                WOLFSSL_DTLS
                WOLFSSL_DTLS13
                WOLFSSL_DTLS_MTU
                WOLFSSL_DTLS_CH_FRAG
                HAVE_MLKEM
                HAVE_MLDSA
                WOLFSSL_EXTRA
        )

        target_link_libraries(${TGT} PUBLIC
                glfw
                glm::glm
                spdlog
                yaml-cpp::yaml-cpp
                OpenSSL::SSL
                OpenSSL::Crypto
                Vulkan::Vulkan
                wolfssl::wolfssl
                Botan::Botan
        )

        if(APPLE)
                target_link_libraries(${TGT} PRIVATE
                        "-framework Foundation" "-framework Cocoa" "-framework IOKit"
                        "-framework CoreVideo" "-framework Metal" "-framework QuartzCore"
                )
        elseif(WIN32)
                target_link_libraries(${TGT} PRIVATE
                        User32.lib Gdi32.lib Shell32.lib ws2_32.lib
                )
        endif()
endforeach()

target_link_libraries(SafiraClient PRIVATE CoreLib wolfssl::wolfssl)
target_link_libraries(SafiraServer PRIVATE CoreLib wolfssl::wolfssl)