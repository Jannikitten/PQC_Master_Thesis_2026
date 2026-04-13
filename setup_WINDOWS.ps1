# =============================================================================
# setup.ps1 – PQC Master Thesis 2026 (Flattened Version)
# =============================================================================
param([string]$BuildType = "Release", [int]$Jobs = [Environment]::ProcessorCount)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Header([string]$msg) {
    Write-Host "`n--- $msg ---" -ForegroundColor Cyan
}

function Update-Environment {
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH","User")
}

# --- Admin Check ---
$currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[ERR] Please run as Administrator!" -ForegroundColor Red; exit 1
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build"

Write-Header "Step 1: System Tools"

# Install Chocolatey
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Chocolatey..."
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    Update-Environment
}

# Install Dependencies
$packages = @("git", "cmake", "python3", "ninja", "openssl")
foreach ($p in $packages) {
    if (-not (Get-Command $p -ErrorAction SilentlyContinue)) {
        Write-Host "Installing $p..."
        choco install $p -y --no-progress
        Update-Environment
    }
}

# Visual Studio
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Host "Installing VS Build Tools..." -ForegroundColor Yellow
    choco install visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -y --no-progress
    Write-Host "CRITICAL: Restart your terminal after this script finishes!" -ForegroundColor Yellow
}

# Vulkan
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Vulkan SDK..."
    $vInstaller = Join-Path $env:TEMP "VulkanSDK-Installer.exe"
    Invoke-WebRequest -Uri "https://sdk.lunarg.com/sdk/download/1.3.296.0/windows/VulkanSDK-1.3.296.0-Installer.exe" -OutFile $vInstaller -UseBasicParsing
    Start-Process -FilePath $vInstaller -ArgumentList "--accept-licenses", "--default-answer", "install" -Wait
    Update-Environment
}

Write-Header "Step 2: Git Submodules"
Set-Location $ScriptDir
git submodule update --init --recursive

Write-Header "Step 3: Build Botan"
$BotanDir = Join-Path $ScriptDir "botan"
$BotanInstall = Join-Path $BotanDir "install"
if (-not (Test-Path (Join-Path $BotanInstall "lib\botan-3.lib"))) {
    Set-Location $BotanDir
    python configure.py --prefix="$BotanInstall" --cc=msvc --enable-modules=ml_kem,ml_dsa
    nmake /f Makefile
    nmake /f Makefile install
    Set-Location $ScriptDir
}

Write-Header "Step 4: CMake & Build"
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir }
Set-Location $BuildDir

# Flattened CMake command (No backticks, no arrays)
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=$BuildType -DBotan_ROOT="$BotanInstall" -DOPENSSL_ROOT_DIR="C:\Program Files\OpenSSL-Win64"

cmake --build . --parallel $Jobs

Write-Host "DONE! Build Successful." -ForegroundColor Green