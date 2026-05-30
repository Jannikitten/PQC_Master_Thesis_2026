<div align="center">

# 🔐 Safira
### Post-Quantum Cryptography Secure Messenger
#### Master's Thesis — 2026

![C++](https://img.shields.io/badge/C%2B%2B-23-blue?style=flat-square&logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-4.1%2B-red?style=flat-square&logo=cmake)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3-orange?style=flat-square&logo=vulkan)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)

*A peer-to-peer encrypted messenger secured by **ML-KEM** and **ML-DSA** — the NIST post-quantum standards — over TLS 1.3 and DTLS 1.3.*

</div>

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
  - [macOS & Linux](#macos--linux)
  - [Windows](#windows)
- [What the Scripts Do](#what-the-scripts-do)
- [Build Options](#build-options)
- [Project Structure](#project-structure)
- [Troubleshooting](#troubleshooting)
- [Dependencies](#dependencies)
- [Security Disclaimer](#️-security-disclaimer)

---

## Overview

Safira is a real-time peer-to-peer chat application built as part of a Master's Thesis on Post-Quantum Cryptography. It contains two programming tasks for participants to replace classical key exchange schemes with their NIST-standardised post-quantum equivalents:

| Classical | Post-Quantum Replacement | Standard |
|-----------|--------------------------|----------|
| ECDH / RSA (key exchange) | **ML-KEM** (Kyber) | NIST FIPS 203 |
| TLS 1.3 | **DTLS 1.3** | RFC 9147 |

The GUI is rendered with **ImGui + Vulkan**. Cryptography is provided by **wolfSSL** and **Botan 3**.

---

## Architecture

```
┌─────────────────────┐     DTLS 1.3      ┌─────────────────────┐
│   SafiraClient      │◄─────────────────►│   SafiraServer      │
│   (ImGui + Vulkan)  │      ML-KEM       │   (ImGui + Vulkan)  │
└─────────────────────┘                   └─────────────────────┘
         │                                          │
         └──────────────────┬───────────────────────┘
                            │
                       ┌────▼─────┐
                       │  CoreLib │
                       │──────────│
                       │ wolfSSL  │  ← DTLS 1.3, ML-KEM, ML-DSA
                       │ Botan 3  │  ← PQC key gen & signatures
                       └──────────┘
```

---

## Prerequisites

The install scripts handle everything automatically, but here's what gets installed:

| Tool | Version | Notes |
|------|---------|-------|
| CMake | **4.1+** | Build system |
| C++ Compiler | C++23 | GCC 13+, Clang 17+, or MSVC 2022 |
| Python 3 | 3.8+ | Required to build Botan |
| Ninja | Latest | Fast build backend |
| Vulkan SDK | 1.3+ | [LunarG](https://vulkan.lunarg.com/) |
| OpenSSL | 3.x | System package |
| Git | Any | For submodule checkout |

> **Disk space:** Allow ~2 GB for all dependencies and build artefacts.

---

## Quick Start

### 1. Clone the repository

```bash
git clone https://github.com/<your-username>/PQC_Master_Thesis_2026.git
cd PQC_Master_Thesis_2026
```

> ⚠️ Do **not** use GitHub's "Download ZIP" — the project relies on **git submodules** (`wolfssl`, `botan`, `imgui`). You must clone with git.

---

### macOS & Linux

```bash
chmod +x setup_UNIX.sh
./setup_UNIX.sh
```

The script auto-detects your platform and uses the appropriate package manager:

| Platform | Package Manager |
|----------|----------------|
| macOS | Homebrew |
| Ubuntu / Debian | apt |
| Fedora / RHEL | dnf |
| Arch Linux | pacman |

After a successful build, binaries appear at:

```
build/SafiraClient
build/SafiraServer
```

---

### Windows

Run from an **Administrator PowerShell**:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\setup_WINDOWS.ps1
```

> **Administrator rights are required** to install Chocolatey, Visual Studio Build Tools, and the Vulkan SDK.

If Visual Studio 2022 Build Tools were just installed by the script, **close and reopen** your terminal as an *"x64 Native Tools Command Prompt for VS 2022"* before re-running, so the MSVC compiler (`cl.exe`) is on your `PATH`.

After a successful build, binaries appear at:

```
build\SafiraClient.exe
build\SafiraServer.exe
```

---

## What the Scripts Do

Both `setup_UNIX.sh` and `setup_WINDOWS.ps1` perform the same five steps:

```
Step 1 ── Install system packages
          (CMake, compiler, Python, OpenSSL, Vulkan SDK, Ninja)
           │
Step 2 ── Initialise git submodules
          (wolfssl, botan, imgui — glfw/glm/spdlog/yaml-cpp are fetched by CMake)
           │
Step 3 ── Build Botan 3 from source
          python3 configure.py --enable-modules=ml_kem,ml_dsa
          make && make install  →  botan/install/
           │
Step 4 ── CMake configure
          cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBotan_ROOT=…
           │
Step 5 ── Compile
          cmake --build build --parallel <N>
```

> **Botan is built only once.** On subsequent runs, the script detects `botan/install/` and skips the compilation entirely, making re-runs fast.

---

## Build Options

### Change build type

```bash
# macOS / Linux
BUILD_TYPE=Debug ./setup_UNIX.sh

# Windows PowerShell
.\setup_WINDOWS.ps1 -BuildType Debug
```

### Override parallelism

```bash
# macOS / Linux  (default: all logical cores)
JOBS=4 ./setup_UNIX.sh

# Windows PowerShell
.\setup_WINDOWS.ps1 -Jobs 4
```

### Rebuild only (after the first setup)

Once the project is configured you don't need to re-run the full setup script. Use CMake directly:

```bash
cmake --build build          # macOS / Linux
cmake --build build          # Windows (from VS prompt)
```

### Force-rebuild Botan

```bash
# macOS / Linux
rm -rf botan/install && ./setup_UNIX.sh

# Windows
Remove-Item -Recurse -Force botan\install
.\setup_WINDOWS.ps1
```

### Clean everything and start fresh

```bash
# macOS / Linux
rm -rf build botan/install
./setup_UNIX.sh

# Windows
Remove-Item -Recurse -Force build, botan\install
.\setup_WINDOWS.ps1
```

---

## Project Structure

```
PQC_Master_Thesis_2026/
├── Core/
│   ├── common/                  # Logging, input, utilities
│   ├── domain/                  # Types, ports, business rules
│   ├── application/             # Client & server application layers
│   ├── infrastructure/
│   │   ├── crypto/              # WolfSSL & Botan crypto wrappers
│   │   ├── network/             # DTLS client/server, P2P session
│   │   ├── persistence/         # YAML message store
│   │   ├── serialization/       # Packet serialisation
│   │   └── platform/            # macOS native helpers
│   └── presentation/
│       ├── app/                 # GUI & console app shells
│       ├── views/               # ImGui view components
│       ├── services/            # Avatar manager, etc.
│       └── widgets/             # Reusable UI widgets
├── Client/                      # SafiraClient entry point
├── Server/                      # SafiraServer entry point
├── imgui/                       # ImGui (submodule)
├── wolfssl/                     # wolfSSL (submodule)
├── botan/                       # Botan 3 (submodule)
│   └── install/                 # Built by setup script
├── Resources/
├── CMakeLists.txt
├── setup_UNIX.sh                # macOS / Linux install script
└── setup_WINDOWS.ps1            # Windows install script
```

---

## Troubleshooting

### `Cannot find Botan library`
The Botan submodule wasn't built. Run:
```bash
rm -rf botan/install && ./setup_UNIX.sh   # or .\setup_WINDOWS.ps1 on Windows
```

### `find_package(Vulkan) failed`
The Vulkan SDK isn't on your `PATH`. Source its environment script:
```bash
# macOS / Linux
source $VULKAN_SDK/setup-env.sh

# Windows — open "x64 Native Tools Command Prompt for VS 2022"
# (the Vulkan SDK installer adds VULKAN_SDK to the system environment)
```

### `CMake version too old`
Your system CMake is below 4.1. The script upgrades it via `pip`, but you may need to open a new terminal session for `PATH` to update. Alternatively, install directly from [cmake.org/download](https://cmake.org/download/).

### `cl.exe not found` (Windows)
You need to build from a Visual Studio developer prompt. Search for *"x64 Native Tools Command Prompt for VS 2022"* in the Start menu and run `setup.ps1` from there.

### `git submodule` errors
Ensure you cloned with `git clone` (not downloaded as a ZIP), then run:
```bash
git submodule update --init --recursive
```

### OpenSSL not found (macOS)
Homebrew isolates OpenSSL in a keg. The script passes `-DOPENSSL_ROOT_DIR` automatically, but if you invoke CMake manually:
```bash
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

---

## Dependencies

| Library | Version | Role | How obtained |
|---------|---------|------|-------------|
| [wolfSSL](https://github.com/wolfSSL/wolfssl) | submodule | DTLS 1.3, ML-KEM/ML-DSA transport | git submodule |
| [Botan](https://github.com/randombit/botan) | 3.11.0 | ML-KEM, ML-DSA primitives | git submodule (built by script) |
| [ImGui](https://github.com/ocornut/imgui) | submodule | Immediate-mode GUI | git submodule |
| [GLFW](https://github.com/TheCherno/glfw) | master | Window & input | CMake FetchContent |
| [GLM](https://github.com/g-truc/glm) | 0af55cc | Math library | CMake FetchContent |
| [spdlog](https://github.com/gabime/spdlog) | v1.x | Logging | CMake FetchContent |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | master | Config & message persistence | CMake FetchContent |
| Vulkan SDK | 1.3+ | GPU rendering backend | System / LunarG |
| OpenSSL | 3.x | Auxiliary cryptography | System package |

---

---

## ⚠️ Security Disclaimer

> **This software is a research prototype and is NOT safe for production use.**

Safira was developed solely as an academic proof-of-concept for a Master's Thesis. It has **not** undergone any independent security audit, formal verification, or cryptographic review. Specifically:

- **No audit.** The codebase has not been reviewed by a qualified third-party security firm.
- **No formal verification.** Protocol correctness, key lifecycle management, and resistance to side-channel attacks have not been formally proven or tested.
- **Implementation bugs.** The cryptographic primitives are provided by wolfSSL and Botan, but their integration and the surrounding protocol logic may contain serious flaws.
- **No threat modelling.** The application has not been evaluated against real-world adversary models.
- **Prototype-grade code.** Error handling, input validation, and memory safety have not been hardened beyond what was needed for research purposes.

**Do not use this application to transmit sensitive, private, or confidential information under any circumstances.** The post-quantum algorithms used (ML-KEM, ML-DSA) are standards-track, but correct algorithm selection alone does not make an implementation secure.

This project exists as part of a qualitative study, not to provide guarantees.

---

<div align="center">

*Master's Thesis in Computer Science — Post-Quantum Cryptography*
*2026*

</div>
