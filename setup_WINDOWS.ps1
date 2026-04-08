# =============================================================================
#  setup.ps1  –  Bootstrap script for PQC_Master_Thesis_2026  (Windows)
#
#  Requirements  (installed automatically if missing):
#    • Chocolatey  (package manager)
#    • CMake 4.1+, Visual Studio 2022 (C++23), Python 3, Git, Ninja
#    • Vulkan SDK  (LunarG installer)
#    • OpenSSL 3.x (via choco)
#
#  Usage (in an Administrator PowerShell):
#    Set-ExecutionPolicy -Scope Process Bypass
#    .\setup.ps1
#
#  Optional parameters:
#    -BuildType  Debug | Release  (default: Release)
#    -Jobs       N                (default: logical CPU count)
# =============================================================================
param(
    [string]$BuildType = "Release",
    [int]   $Jobs      = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Colour helpers ─────────────────────────────────────────────────────────────
function Write-Header([string]$msg) {
    Write-Host "`n══════════════════════════════════════════════════" -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor Cyan
}
function Write-Info   ([string]$msg) { Write-Host "[INFO]  $msg" -ForegroundColor Cyan    }
function Write-Ok     ([string]$msg) { Write-Host "[ OK ]  $msg" -ForegroundColor Green   }
function Write-Warn   ([string]$msg) { Write-Host "[WARN]  $msg" -ForegroundColor Yellow  }
function Write-Err    ([string]$msg) { Write-Host "[ERR ]  $msg" -ForegroundColor Red; exit 1 }

# ── Require Administrator ──────────────────────────────────────────────────────
$currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Err "Please run this script from an Administrator PowerShell session."
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir "build"

Write-Header "PQC Master Thesis 2026 – Windows Setup"
Write-Info "Build type : $BuildType"
Write-Info "Parallel   : $Jobs jobs"
Write-Info "Source dir : $ScriptDir"
Write-Info "Build dir  : $BuildDir"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 1 – CHOCOLATEY + SYSTEM TOOLS
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 1 – System dependencies"

# Install Chocolatey if absent
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Info "Installing Chocolatey…"
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    # Refresh environment so choco is available
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("PATH","User")
} else {
    Write-Ok "Chocolatey already installed"
}

# Helper: install a choco package only if the command isn't already on PATH
function Install-ChocoIfMissing([string]$pkg, [string]$cmd) {
    if (Get-Command $cmd -ErrorAction SilentlyContinue) {
        Write-Ok "$cmd already on PATH"
    } else {
        Write-Info "Installing $pkg via Chocolatey…"
        choco install $pkg -y --no-progress
        # Refresh PATH
        $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
                    [System.Environment]::GetEnvironmentVariable("PATH","User")
    }
}

Install-ChocoIfMissing "git"     "git"
Install-ChocoIfMissing "cmake"   "cmake"
Install-ChocoIfMissing "python3" "python"
Install-ChocoIfMissing "ninja"   "ninja"
Install-ChocoIfMissing "openssl" "openssl"

# Visual Studio 2022 build tools with C++ workload (skipped if cl.exe already present)
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Warn "MSVC (cl.exe) not found."
    Write-Warn "Installing Visual Studio 2022 Build Tools with C++ Desktop workload…"
    Write-Warn "(This is a large download – ~2 GB – and may take several minutes.)"
    choco install visualstudio2022buildtools `
        --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" `
        -y --no-progress
    Write-Ok "Visual Studio 2022 Build Tools installed"
    Write-Warn "Please restart this script from a 'Developer Command Prompt for VS 2022' or"
    Write-Warn "open 'x64 Native Tools Command Prompt for VS 2022' before continuing."
} else {
    Write-Ok "MSVC compiler found"
}

# Verify CMake version
$cmakeVer = (cmake --version 2>$null | Select-String -Pattern '\d+\.\d+\.\d+').Matches[0].Value
$cmakeMajor = [int]($cmakeVer -split '\.')[0]
if ($cmakeMajor -lt 4) {
    Write-Warn "CMake $cmakeVer detected but project requires ≥ 4.1."
    Write-Info "Upgrading CMake via pip…"
    python -m pip install --upgrade cmake
    $env:PATH = (python -c "import site; print(site.getusersitepackages())" | Split-Path) + ";$env:PATH"
    Write-Ok "CMake upgraded to $(cmake --version | Select-String '\d+\.\d+\.\d+')"
} else {
    Write-Ok "CMake $cmakeVer ✓"
}

# ── Vulkan SDK ─────────────────────────────────────────────────────────────────
$vulkanPresent = (Get-Command glslc -ErrorAction SilentlyContinue) -or `
                 ($env:VULKAN_SDK -and (Test-Path $env:VULKAN_SDK))
if (-not $vulkanPresent) {
    Write-Info "Vulkan SDK not detected – downloading LunarG installer…"

    # Fetch the latest SDK version string from LunarG
    try {
        $latestSDK = (Invoke-WebRequest "https://vulkan.lunarg.com/sdk/latest/windows.txt" -UseBasicParsing).Content.Trim()
    } catch {
        $latestSDK = "1.3.296.0"   # fallback if web request fails
        Write-Warn "Could not retrieve latest version; using known-good $latestSDK"
    }

    $vulkanInstaller = Join-Path $env:TEMP "VulkanSDK-$latestSDK-Installer.exe"
    $vulkanUrl = "https://sdk.lunarg.com/sdk/download/$latestSDK/windows/VulkanSDK-$latestSDK-Installer.exe"

    Write-Info "Downloading Vulkan SDK $latestSDK…"
    Invoke-WebRequest -Uri $vulkanUrl -OutFile $vulkanInstaller -UseBasicParsing

    Write-Info "Running Vulkan SDK installer (silent)…"
    Start-Process -FilePath $vulkanInstaller -ArgumentList "--accept-licenses --default-answer --confirm-command install" -Wait

    # Refresh environment so VULKAN_SDK is visible in this session
    $env:VULKAN_SDK = [System.Environment]::GetEnvironmentVariable("VULKAN_SDK","Machine")
    if ($env:VULKAN_SDK) {
        $env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
        Write-Ok "Vulkan SDK installed at $env:VULKAN_SDK"
    } else {
        Write-Warn "VULKAN_SDK environment variable not set after install."
        Write-Warn "You may need to restart your shell or set it manually."
    }
} else {
    Write-Ok "Vulkan SDK already present"
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 2 – GIT SUBMODULES
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 2 – Git submodules"

Set-Location $ScriptDir
if (-not (Test-Path ".git")) {
    Write-Err "No .git directory found. Run this script from the repository root."
}

Write-Info "Updating submodules (wolfssl, botan, imgui, …)…"
git submodule update --init --recursive
Write-Ok "Submodules ready"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 3 – BUILD BOTAN 3
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 3 – Building Botan 3 (PQC algorithms)"

$BotanDir     = Join-Path $ScriptDir "botan"
$BotanInstall = Join-Path $BotanDir  "install"

if (-not (Test-Path $BotanDir)) {
    Write-Err "botan/ submodule not found. Run: git submodule update --init --recursive"
}

$botanLibA   = Join-Path $BotanInstall "lib\botan-3.lib"
$botanLibDll = Join-Path $BotanInstall "lib\botan-3.dll"

if ((Test-Path $botanLibA) -or (Test-Path $botanLibDll)) {
    Write-Ok "Botan already built – skipping (delete botan\install to force rebuild)"
} else {
    Set-Location $BotanDir
    Write-Info "Configuring Botan with ML-KEM and ML-DSA…"

    # On Windows, Botan's configure.py needs the MSVC compiler flag
    python configure.py `
        --prefix="$BotanInstall" `
        --cc=msvc `
        --with-tls `
        --enable-modules=ml_kem,ml_dsa

    Write-Info "Building Botan with $Jobs parallel jobs…"
    # Botan on Windows uses nmake
    nmake /f Makefile /J $Jobs

    Write-Info "Installing Botan to $BotanInstall…"
    nmake /f Makefile install

    Write-Ok "Botan built and installed → $BotanInstall"
    Set-Location $ScriptDir
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 4 – CMAKE CONFIGURE
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 4 – CMake configure"

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}
Set-Location $BuildDir

# Locate OpenSSL installed by Chocolatey (typically C:\Program Files\OpenSSL-Win64)
$opensslRoot = ""
$possibleOpenSSL = @(
    "C:\Program Files\OpenSSL-Win64",
    "C:\Program Files\OpenSSL",
    "C:\OpenSSL-Win64",
    "C:\OpenSSL"
)
foreach ($p in $possibleOpenSSL) {
    if (Test-Path $p) { $opensslRoot = $p; break }
}

$cmakeArgs = @(
    $ScriptDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DBotan_ROOT=$BotanInstall"
)

if ($opensslRoot) {
    Write-Info "Using OpenSSL at $opensslRoot"
    $cmakeArgs += "-DOPENSSL_ROOT_DIR=$opensslRoot"
} else {
    Write-Warn "Could not auto-detect OpenSSL root. If CMake can't find it, set -DOPENSSL_ROOT_DIR manually."
}

& cmake @cmakeArgs
Write-Ok "CMake configuration complete"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 5 – BUILD
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 5 – Building"

cmake --build . --parallel $Jobs

# ── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║         Build successful!                    ║" -ForegroundColor Green
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  Client : $BuildDir\SafiraClient.exe" -ForegroundColor White
Write-Host "  Server : $BuildDir\SafiraServer.exe" -ForegroundColor White
Write-Host ""
Write-Host "Tips:" -ForegroundColor Yellow
Write-Host "  Rebuild after changes : cmake --build $BuildDir" -ForegroundColor Gray
Write-Host "  Debug build           : .\setup.ps1 -BuildType Debug" -ForegroundColor Gray
Write-Host "  Rebuild Botan         : Remove-Item -Recurse botan\install; .\setup.ps1" -ForegroundColor Gray