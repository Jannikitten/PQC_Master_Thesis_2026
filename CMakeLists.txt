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
set(WOLFSSL_KEYGEN       ON CACHE BOOL "" FORCE)
set(WOLFSSL_CERTGEN      ON CACHE BOOL "" FORCE)

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
                set(_botan_type SHARED)
        else()
                message(FATAL_ERROR
                        "Cannot find Botan library under ${Botan_ROOT}/lib/\n"
                        "Build and install Botan from the submodule first:\n"
                        "  cd botan\n"
                        "  python3 configure.py --prefix=./install --enable-modules=ml_kem,ml_dsa\n"
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
        # ── common layer ─────────────────────────────────────────────────
        Core/common/Image.cpp
        Core/common/Random.cpp
        Core/common/Log.cpp
        Core/common/Input.cpp
        Core/common/StringUtils.cpp

        # ── infrastructure/crypto ─────────────────────────────────────────
        Core/infrastructure/crypto/WolfSSLCrypto.cpp
        Core/infrastructure/crypto/BotanP2PCrypto.cpp

        # ── infrastructure/network ───────────────────────────────────────
        Core/infrastructure/network/NetworkExecutor.cpp
        Core/infrastructure/network/DtlsClient.cpp
        Core/infrastructure/network/DtlsServer.cpp
        Core/infrastructure/network/P2PSession.cpp

        # ── infrastructure/persistence ───────────────────────────────────
        Core/infrastructure/persistence/YamlMessageStore.cpp

        # ── presentation/app ──────────────────────────────────────────────
        Core/presentation/app/GuiApp.cpp
        Core/presentation/app/ConsoleApp.cpp

        # ── presentation/views ────────────────────────────────────────────
        Core/presentation/views/ChatView.cpp
        Core/presentation/views/ServerConsoleView.cpp
        Core/presentation/views/ConsoleGUIView.cpp
        Core/presentation/views/ConnectionView.cpp
        Core/presentation/views/UserListView.cpp
        Core/presentation/views/ConversationListView.cpp

        # ── presentation/services ─────────────────────────────────────────
        Core/presentation/services/AvatarManager.cpp

        # ── presentation/widgets ──────────────────────────────────────────
        Core/presentation/widgets/UIWidgets.cpp

        # ── infrastructure/serialization ──────────────────────────────────
        Core/infrastructure/serialization/PacketSerialize.cpp
)

# Platform-specific sources for the native window / file-dialog helpers.
if(APPLE)
        target_sources(CoreLib PRIVATE
                Core/infrastructure/platform/MacOSWindow.mm
                Core/infrastructure/platform/FileDialog.mm)
elseif(WIN32)
        target_sources(CoreLib PRIVATE
                Core/infrastructure/platform/FileDialog_Windows.cpp)
else()
        target_sources(CoreLib PRIVATE
                Core/infrastructure/platform/FileDialog_Linux.cpp)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 10. Client executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(SafiraClient
        Client/ClientApp.cpp
        Core/application/client/ClientLayer.cpp
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
        Core/application/server/ServerLayer.cpp
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
                ${CMAKE_SOURCE_DIR}/Core/common
                ${CMAKE_SOURCE_DIR}/Core/domain/types
                ${CMAKE_SOURCE_DIR}/Core/domain/ports
                ${CMAKE_SOURCE_DIR}/Core/domain/rules
                ${CMAKE_SOURCE_DIR}/Core/infrastructure/serialization
                ${CMAKE_SOURCE_DIR}/Core/infrastructure/network
                ${CMAKE_SOURCE_DIR}/Core/infrastructure/crypto
                ${CMAKE_SOURCE_DIR}/Core/infrastructure/persistence
                ${CMAKE_SOURCE_DIR}/Core/infrastructure/platform
                ${CMAKE_SOURCE_DIR}/Core/application/store
                ${CMAKE_SOURCE_DIR}/Core/application/server
                ${CMAKE_SOURCE_DIR}/Core/application/client
                ${CMAKE_SOURCE_DIR}/Core/presentation/app
                ${CMAKE_SOURCE_DIR}/Core/presentation/views
                ${CMAKE_SOURCE_DIR}/Core/presentation/services
                ${CMAKE_SOURCE_DIR}/Core/presentation/widgets
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
                WOLFSSL_KEY_GEN
                WOLFSSL_CERT_GEN
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
                        "-framework UniformTypeIdentifiers"
                )
        elseif(WIN32)
                target_link_libraries(${TGT} PRIVATE
                        User32.lib Gdi32.lib Shell32.lib ws2_32.lib Ole32.lib
                )
        endif()
endforeach()

target_link_libraries(SafiraClient PRIVATE CoreLib wolfssl::wolfssl)
target_link_libraries(SafiraServer PRIVATE CoreLib wolfssl::wolfssl)
