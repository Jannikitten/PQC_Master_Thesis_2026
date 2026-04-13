# =============================================================================
#  setup.ps1  –  Bootstrap script for PQC_Master_Thesis_2026  (Windows)
# =============================================================================
param(
    [string]$BuildType = "Release",
    [int]   $Jobs      = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Helpers ──────────────────────────────────────────────────────────────────
function Write-Header([string]$msg) {
    Write-Host "`n══════════════════════════════════════════════════" -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor Cyan
}
function Write-Info   ([string]$msg) { Write-Host "[INFO]  $msg" -ForegroundColor Cyan    }
function Write-Ok     ([string]$msg) { Write-Host "[ OK ]  $msg" -ForegroundColor Green   }
function Write-Warn   ([string]$msg) { Write-Host "[WARN]  $msg" -ForegroundColor Yellow  }
function Write-Err    ([string]$msg) { Write-Host "[ERR ]  $msg" -ForegroundColor Red; exit 1 }

function Update-Environment {
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("PATH","User")
}

# ── Require Administrator ──────────────────────────────────────────────────────
$currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Err "Please run this script from an Administrator PowerShell session."
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir "build"

Write-Header "PQC Master Thesis 2026 – Windows Setup"
Write-Info "Build type : $BuildType"
Write-Info "Parallel   : $Jobs jobs (where supported)"

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 1 – SYSTEM DEPENDENCIES
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 1 – System dependencies"

if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Info "Installing Chocolatey…"
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    Update-Environment
}

function Install-ChocoIfMissing([string]$pkg, [string]$cmd) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Info "Installing $pkg via Chocolatey…"
        choco install $pkg -y --no-progress
        Update-Environment
    }
}

Install-ChocoIfMissing "git" "git"
Install-ChocoIfMissing "cmake" "cmake"
Install-ChocoIfMissing "python3" "python"
Install-ChocoIfMissing "ninja" "ninja"
Install-ChocoIfMissing "openssl" "openssl"

# Visual Studio Build Tools (Using Splatting to avoid backtick errors)
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Warn "MSVC not found. Installing VS 2022 Build Tools..."
    $vsArgs = @(
        "visualstudio2022buildtools",
        "--package-parameters",
        "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended",
        "-y",
        "--no-progress"
    )
    choco install @vsArgs
    Write-Warn "Please restart this terminal after installation to enable 'cl.exe'."
}

# ── Vulkan SDK ─────────────────────────────────────────────────────────────────
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    Write-Info "Vulkan SDK not detected – installing..."
    try {
        $latestSDK = (Invoke-WebRequest "https://vulkan.lunarg.com/sdk/latest/windows.txt" -UseBasicParsing).Content.Trim()
    } catch {
        $latestSDK = "1.3.296.0"
    }
    $vulkanInstaller = Join-Path $env:TEMP "VulkanSDK-Installer.exe"
    Invoke-WebRequest -Uri "https://sdk.lunarg.com/sdk/download/$latestSDK/windows/VulkanSDK-$latestSDK-Installer.exe" -OutFile $vulkanInstaller -UseBasicParsing
    
    $vulkanArgs = @("--accept-licenses", "--default-answer", "install")
    Start-Process -FilePath $vulkanInstaller -ArgumentList $vulkanArgs -Wait
    Update-Environment
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 2 – GIT SUBMODULES
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 2 – Git submodules"
Set-Location $ScriptDir
git submodule update --init --recursive

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 3 – BUILD BOTAN 3
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 3 – Building Botan 3"
$BotanDir = Join-Path $ScriptDir "botan"
$BotanInstall = Join-Path $BotanDir "install"
$botanLib = Join-Path $BotanInstall "lib\botan-3.lib"

if (Test-Path $botanLib) {
    Write-Ok "Botan already built."
} else {
    Set-Location $BotanDir
    $botanConfig = @(
        "configure.py",
        "--prefix=$BotanInstall",
        "--cc=msvc",
        "--enable-modules=ml_kem,ml_dsa"
    )
    python @botanConfig
    
    Write-Info "Building Botan (nmake is single-threaded)..."
    nmake /f Makefile
    nmake /f Makefile install
    Set-Location $ScriptDir
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 4 – CMAKE CONFIGURE & BUILD
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 4 – Build"
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
Set-Location $BuildDir

# Detect OpenSSL path
$opensslRoot = "C:\Program Files\OpenSSL-Win64"

$cmakeParams = @(
    "..",
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DBotan_ROOT=$BotanInstall",
    "-DOPENSSL_ROOT_DIR=$opensslRoot"
)

& cmake @cmakeParams
cmake --build . --parallel $Jobs

Write-Ok "Build Successful!"