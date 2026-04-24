# =============================================================================
# setup_WINDOWS.ps1 - Bootstrap script for PQC_Master_Thesis_2026 (Windows)
#
# Usage (from an *elevated* PowerShell):
#     Set-ExecutionPolicy -Scope Process Bypass
#     .\setup_WINDOWS.ps1
#
# Optional:
#     -BuildType Debug
#     -Jobs 8
# =============================================================================
param(
    [string]$BuildType = "Release",
    [int]$Jobs = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Header([string]$msg) {
    Write-Host ""
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}
function Write-Info([string]$msg)    { Write-Host "[INFO] $msg" -ForegroundColor Gray }
function Write-Ok([string]$msg)      { Write-Host "[ OK ] $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg)   { Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Die([string]$msg)           { Write-Host "[ERR ] $msg" -ForegroundColor Red; exit 1 }

function Update-Environment {
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("PATH","User")
}

# Load MSVC developer environment (sets cl.exe, nmake, INCLUDE, LIB, etc.).
# Required for building Botan with --cc=msvc via nmake.
function Import-VsDevEnv {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Die "vswhere.exe not found. Install Visual Studio 2022 Build Tools, then open a NEW PowerShell and re-run this script."
    }

    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsInstall) {
        Die "No Visual Studio installation with the C++ toolset was found. Re-run after the VS Build Tools install completes."
    }

    $vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        Die "vcvars64.bat not found under $vsInstall. The C++ workload may be incomplete."
    }

    Write-Info "Loading MSVC environment from $vcvars"
    # Run vcvars64.bat and import its environment changes into this PowerShell session.
    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Die "cl.exe still not on PATH after loading vcvars64.bat. Try opening an 'x64 Native Tools Command Prompt for VS 2022' and running the script from there."
    }
    Write-Ok "MSVC environment loaded."
}

# --- Admin Check --------------------------------------------------------------
$currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die "Please run this script as Administrator."
}

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir     = Join-Path $ScriptDir "build"
$BotanDir     = Join-Path $ScriptDir "botan"
$BotanInstall = Join-Path $BotanDir "install"

Write-Header "Step 1: System tools"

# Chocolatey ------------------------------------------------------------------
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Info "Installing Chocolatey..."
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol =
        [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString(
        'https://community.chocolatey.org/install.ps1'))
    Update-Environment
}

# Dependencies ---------------------------------------------------------------
$packages = @("git", "cmake", "python", "ninja", "openssl")
foreach ($p in $packages) {
    $probe = switch ($p) {
        "python"  { "python" }
        "openssl" { "openssl" }
        default   { $p }
    }
    if (-not (Get-Command $probe -ErrorAction SilentlyContinue)) {
        Write-Info "Installing $p..."
        choco install $p -y --no-progress
        Update-Environment
    } else {
        Write-Ok "$p already installed"
    }
}

# Visual Studio Build Tools ---------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasVc = $false
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($vsInstall) { $hasVc = $true }
}
if (-not $hasVc) {
    Write-Info "Installing Visual Studio 2022 Build Tools (C++ workload)..."
    choco install visualstudio2022buildtools -y --no-progress `
        --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    Update-Environment
    Write-Warn2 "VS Build Tools installed. If MSVC is still not detected below, close this terminal, open a NEW elevated PowerShell, and re-run the script."
}

# Vulkan SDK ------------------------------------------------------------------
if (-not (Get-Command glslc -ErrorAction SilentlyContinue) -and -not $env:VULKAN_SDK) {
    Write-Info "Installing Vulkan SDK..."
    $vInstaller = Join-Path $env:TEMP "VulkanSDK-Installer.exe"
    Invoke-WebRequest `
        -Uri "https://sdk.lunarg.com/sdk/download/1.3.296.0/windows/VulkanSDK-1.3.296.0-Installer.exe" `
        -OutFile $vInstaller -UseBasicParsing
    Start-Process -FilePath $vInstaller `
        -ArgumentList "--accept-licenses", "--default-answer", "install" -Wait
    Update-Environment
} else {
    Write-Ok "Vulkan SDK already available"
}

# Locate OpenSSL install prefix (chocolatey default vs alternative layouts) ---
$OpenSslRoot = $null
foreach ($cand in @(
    "C:\Program Files\OpenSSL-Win64",
    "C:\Program Files\OpenSSL",
    "C:\ProgramData\chocolatey\lib\openssl\tools"
)) {
    if (Test-Path (Join-Path $cand "include\openssl\ssl.h")) { $OpenSslRoot = $cand; break }
}
if (-not $OpenSslRoot) {
    Write-Warn2 "Could not auto-detect OpenSSL install prefix; CMake will try its defaults."
} else {
    Write-Ok "OpenSSL found at $OpenSslRoot"
}

# --- Step 2: Git submodules --------------------------------------------------
Write-Header "Step 2: Git submodules"
Set-Location $ScriptDir

if (-not (Test-Path ".git")) {
    Die "This directory is not a git repository root. Clone the repo first."
}

# --no-recommend-shallow disables the shallow=true hint from .gitmodules, which
# can otherwise fail when the recorded commit is not at the tip of the branch.
& git submodule update --init --recursive --progress --no-recommend-shallow
if ($LASTEXITCODE -ne 0) {
    Write-Warn2 "Submodule update failed. Retrying after deinit..."
    & git submodule deinit -f --all
    & git submodule update --init --recursive --progress --no-recommend-shallow
    if ($LASTEXITCODE -ne 0) {
        Die "Failed to clone submodules. Check your internet connection and run: git submodule update --init --recursive --no-recommend-shallow"
    }
}

$required = @(
    "imgui\imgui.h",
    "botan\configure.py",
    "wolfssl\CMakeLists.txt",
    "glm\CMakeLists.txt",
    "spdlog\CMakeLists.txt",
    "yaml-cpp\CMakeLists.txt"
)
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $ScriptDir $f))) {
        Die "Submodule file missing: $f (init did not populate its parent). Try: git submodule update --init --recursive --force"
    }
}
Write-Ok "Submodules ready"

# --- Step 3: Build Botan -----------------------------------------------------
Write-Header "Step 3: Build Botan"

# Botan's nmake-based build needs cl.exe + nmake on PATH; load vcvars64 now.
Import-VsDevEnv

$botanLib = Join-Path $BotanInstall "lib\botan-3.lib"
if (Test-Path $botanLib) {
    Write-Ok "Botan already built - skipping (delete '$BotanInstall' to force rebuild)"
} else {
    Set-Location $BotanDir

    # NOTE: Botan 3 has no `--with-tls` flag; TLS is enabled by default.
    & python configure.py `
        --prefix="$BotanInstall" `
        --cc=msvc `
        --enable-modules=ml_kem,ml_dsa
    if ($LASTEXITCODE -ne 0) { Die "Botan configure.py failed" }

    & nmake /f Makefile
    if ($LASTEXITCODE -ne 0) { Die "Botan nmake build failed" }

    & nmake /f Makefile install
    if ($LASTEXITCODE -ne 0) { Die "Botan install failed" }

    Set-Location $ScriptDir
    Write-Ok "Botan built -> $BotanInstall"
}

# --- Step 4: CMake configure & build -----------------------------------------
Write-Header "Step 4: CMake configure & build"

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
Set-Location $BuildDir

$cmakeArgs = @(
    $ScriptDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DBotan_ROOT=$BotanInstall"
)
if ($OpenSslRoot) { $cmakeArgs += "-DOPENSSL_ROOT_DIR=$OpenSslRoot" }

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Die "CMake configure failed" }

& cmake --build . --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Die "Build failed" }

Write-Host ""
Write-Host "==============================================" -ForegroundColor Green
Write-Host "           Build successful!" -ForegroundColor Green
Write-Host "==============================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Client : $BuildDir\SafiraClient.exe"
Write-Host "  Server : $BuildDir\SafiraServer.exe"
