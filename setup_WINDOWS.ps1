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

# Function to refresh PATH without restarting PowerShell
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

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 1 – SYSTEM TOOLS
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
        Write-Info "Installing $pkg…"
        choco install $pkg -y --no-progress
        Update-Environment
    }
}

Install-ChocoIfMissing "git" "git"
Install-ChocoIfMissing "cmake" "cmake"
Install-ChocoIfMissing "python3" "python"
Install-ChocoIfMissing "ninja" "ninja"
Install-ChocoIfMissing "openssl" "openssl"

# Visual Studio Build Tools
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Warn "MSVC not found. Installing Build Tools..."
    choco install visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -y
    Write-Warn "CRITICAL: You must restart this terminal to use 'cl.exe' or 'nmake'."
}

# ── Vulkan SDK ─────────────────────────────────────────────────────────────────
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    Write-Info "Installing Vulkan SDK..."
    $latestSDK = (Invoke-WebRequest "https://vulkan.lunarg.com/sdk/latest/windows.txt" -UseBasicParsing).Content.Trim()
    $vulkanInstaller = Join-Path $env:TEMP "VulkanSDK-Installer.exe"
    Invoke-WebRequest -Uri "https://sdk.lunarg.com/sdk/download/$latestSDK/windows/VulkanSDK-$latestSDK-Installer.exe" -OutFile $vulkanInstaller -UseBasicParsing
    Start-Process -FilePath $vulkanInstaller -ArgumentList "--accept-licenses", "--default-answer", "install" -Wait
    Update-Environment
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 3 – BUILD BOTAN 3
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 3 – Building Botan 3"

$BotanDir = Join-Path $ScriptDir "botan"
$BotanInstall = Join-Path $BotanDir "install"

if (-not (Test-Path $botanLibA)) {
    Set-Location $BotanDir
    # Correct configure call for Windows MSVC
    python configure.py --prefix="$BotanInstall" --cc=msvc --enable-modules=ml_kem,ml_dsa

    Write-Info "Building Botan (nmake does not support parallel /J)..."
    # FIX: nmake does not support /J. We use it sequentially or use 'jom' if installed.
    nmake /f Makefile
    nmake /f Makefile install
    Set-Location $ScriptDir
}

# ═══════════════════════════════════════════════════════════════════════════════
# STEP 4 & 5 – CMAKE & BUILD
# ═══════════════════════════════════════════════════════════════════════════════
Write-Header "Step 4 & 5 – Build"

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir }
Set-Location $BuildDir

# Detect OpenSSL via Choco's default path
$opensslRoot = "C:\Program Files\OpenSSL-Win64"

cmake .. -G "Ninja" `
    -DCMAKE_BUILD_TYPE=$BuildType `
    -DBotan_ROOT="$BotanInstall" `
    -DOPENSSL_ROOT_DIR="$opensslRoot"

cmake --build . --parallel $Jobs

Write-Ok "Build Successful!"