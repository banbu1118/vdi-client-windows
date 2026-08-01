# FreeRDP 3.28.0 Windows Build Script
# Usage: .\build-freerdp.ps1

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$FreeRDP_Source = Join-Path $ProjectRoot "freerdp-3.28.0"
$FreeRDP_Build = Join-Path $FreeRDP_Source "build"
$FreeRDP_Install = Join-Path $FreeRDP_Source "install"

# vcpkg configuration
$vcpkgRoot = "C:\Users\Administrator\Desktop\workspace\vcpkg"
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"

if (-not (Test-Path $vcpkgToolchain)) {
    Write-Error "vcpkg toolchain not found at $vcpkgToolchain"
    exit 1
}

Write-Host "=== FreeRDP 3.28.0 Windows Build ===" -ForegroundColor Cyan
Write-Host "Source: $FreeRDP_Source"
Write-Host "vcpkg:  $vcpkgRoot"

if (-not (Test-Path $FreeRDP_Source)) {
    Write-Error "FreeRDP source not found at $FreeRDP_Source"
    exit 1
}

New-Item -ItemType Directory -Force -Path $FreeRDP_Build | Out-Null
New-Item -ItemType Directory -Force -Path $FreeRDP_Install | Out-Null

# Detect Visual Studio
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = ""
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -property installationPath
}
if (-not $vsPath) {
    $paths = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    )
    foreach ($p in $paths) {
        if (Test-Path $p) { $vsPath = $p; break }
    }
}
if (-not $vsPath) {
    Write-Error "Visual Studio not found."
    exit 1
}

Write-Host "Visual Studio: $vsPath" -ForegroundColor Green

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
    exit 1
}

Push-Location $FreeRDP_Build

try {
    $cmakeArgs = @(
        "-G", "Ninja"
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=$FreeRDP_Install"
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
        "-DWITH_PULSE=OFF"
        "-DWITH_WASAPI=ON"
        "-DWITH_FFMPEG=ON"
        "-DWITH_SWSCALE=ON"
        "-DWITH_OPENH264=ON"
        "-DWITH_OPENSSL=ON"
        "-DWITH_VAAPI=OFF"
        "-DWITH_CUPS=OFF"
        "-DWITH_PCSC=OFF"
        "-DWITH_ALSA=OFF"
        "-DWITH_GSM=OFF"
        "-DWITH_CHANNELS=ON"
        "-DWITH_CLIENT_COMMON=ON"
        "-DWITH_DSP_FFMPEG=ON"
        "-DWITH_SSE2=ON"
        "-DCHANNEL_URBDRC=ON"
        "-DCHANNEL_RDPECAM_CLIENT=ON"
        "-DCHANNEL_GEOMETRY=ON"
        "-DWITH_MANPAGES=OFF"
        "-DWITH_SERVER=OFF"
        "-DWITH_PROXY=OFF"
        "-DWITH_CLIENT_SDL=OFF"
        "$FreeRDP_Source"
    )

    # Add Ninja to PATH (vcpkg downloads)
    $env:PATH = "C:\Users\Administrator\Desktop\workspace\vcpkg\downloads\tools\ninja-1.13.2-windows;$env:PATH"

    Write-Host "Configuring FreeRDP..." -ForegroundColor Yellow
    $configureCmd = "`"$vcvars`" && cmake $($cmakeArgs -join ' ')"
    cmd /c $configureCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configuration failed (exit code: $LASTEXITCODE)"
        exit 1
    }

    Write-Host "Building FreeRDP..." -ForegroundColor Yellow
    $buildCmd = "`"$vcvars`" && cmake --build . --config Release --parallel"
    cmd /c $buildCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit code: $LASTEXITCODE)"
        exit 1
    }

    Write-Host "Installing FreeRDP..." -ForegroundColor Yellow
    $installCmd = "`"$vcvars`" && cmake --install . --config Release"
    cmd /c $installCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Install failed (exit code: $LASTEXITCODE)"
        exit 1
    }

    Write-Host "=== FreeRDP build completed successfully! ===" -ForegroundColor Cyan
    Write-Host "Install directory: $FreeRDP_Install" -ForegroundColor Green
}
finally {
    Pop-Location
}
