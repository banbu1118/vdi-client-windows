# qfreerdp-windows 构建脚本
# 用法: .\build-qf-client.ps1
# 前提条件:
#   - VS 2022, CMake, Ninja 已安装
#   - Qt 6.11.1 (msvc2022_64) 已安装
#   - vcpkg (spdlog, libusb) 已安装
#   - FreeRDP 已编译 (先运行 build-freerdp.ps1)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$SourceDir = $PSScriptRoot
$BuildDir = Join-Path $SourceDir "build"

# ==== 依赖路径 ====
$QtDir = "C:\Qt\6.11.1\msvc2022_64"
$FreeRDP_Install = Join-Path $ProjectRoot "freerdp-3.28.0\install"
$vcpkgRoot = "C:\Users\Administrator\Desktop\workspace\vcpkg"
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"

# ==== 验证 ====
if (-not (Test-Path $FreeRDP_Install)) {
    Write-Error "FreeRDP install not found at $FreeRDP_Install. Run build-freerdp.ps1 first."
    exit 1
}
if (-not (Test-Path $vcpkgToolchain)) {
    Write-Error "vcpkg toolchain not found at $vcpkgToolchain"
    exit 1
}
if (-not (Test-Path $QtDir)) {
    Write-Error "Qt 6 not found at $QtDir"
    exit 1
}

# 检测 Visual Studio
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

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
    exit 1
}

Write-Host "=== qfreerdp-windows Build ===" -ForegroundColor Cyan
Write-Host "Qt:       $QtDir"
Write-Host "FreeRDP:  $FreeRDP_Install"
Write-Host "vcpkg:    $vcpkgRoot"
Write-Host "Output:   $BuildDir"

# 创建构建目录
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Push-Location $BuildDir
try {
    # CMake 配置
    $cmakeArgs = @(
        "-G", "Ninja"
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_PREFIX_PATH=$QtDir;$FreeRDP_Install"
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
        "$SourceDir"
    )

    Write-Host "`nConfiguring..." -ForegroundColor Yellow
    $configureCmd = "`"$vcvars`" && cmake $($cmakeArgs -join ' ') 2>&1"
    cmd /c $configureCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configuration failed (exit code: $LASTEXITCODE)"
        exit 1
    }

    # 编译
    Write-Host "`nBuilding..." -ForegroundColor Yellow
    $buildCmd = "`"$vcvars`" && cmake --build . --config Release --parallel 2>&1"
    cmd /c $buildCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit code: $LASTEXITCODE)"
        exit 1
    }

    Write-Host "`n=== Build completed successfully! ===" -ForegroundColor Cyan

    # ==== 部署运行时 ====
    Write-Host "`nDeploying runtime dependencies..." -ForegroundColor Yellow

    # FreeRDP DLLs
    Write-Host "  - FreeRDP DLLs" -ForegroundColor Gray
    Copy-Item (Join-Path $FreeRDP_Install "bin\freerdp3.dll") $BuildDir -Force -ErrorAction Stop
    Copy-Item (Join-Path $FreeRDP_Install "bin\freerdp-client3.dll") $BuildDir -Force -ErrorAction Stop
    Copy-Item (Join-Path $FreeRDP_Install "bin\winpr3.dll") $BuildDir -Force -ErrorAction Stop

    # OpenSSL 及 vcpkg DLLs
    $vcpkgBin = Join-Path $vcpkgRoot "installed\x64-windows\bin"
    Write-Host "  - OpenSSL DLLs" -ForegroundColor Gray
    Copy-Item (Join-Path $vcpkgBin "libcrypto-3-x64.dll") $BuildDir -Force -ErrorAction Stop
    Copy-Item (Join-Path $vcpkgBin "libssl-3-x64.dll") $BuildDir -Force -ErrorAction Stop

    # 已由 CMake 复制的: spdlog.dll, fmt.dll, libusb-1.0.dll
    # FFmpeg DLLs (由 sound/rdpsnd 通道使用)
    Write-Host "  - FFmpeg DLLs" -ForegroundColor Gray
    foreach ($dll in @("avcodec-62.dll","avdevice-62.dll","avfilter-11.dll","avformat-62.dll","avutil-60.dll",
                       "swscale-9.dll","swresample-6.dll")) {
        $src = Join-Path $vcpkgBin $dll
        if (Test-Path $src) { Copy-Item $src $BuildDir -Force }
    }

    # zlib
    Write-Host "  - zlib DLL" -ForegroundColor Gray
    $z_src = Join-Path $vcpkgBin "z.dll"
    if (Test-Path $z_src) { Copy-Item $z_src $BuildDir -Force }

    # OpenH264
    Write-Host "  - OpenH264 DLL" -ForegroundColor Gray
    $h264_src = Join-Path $vcpkgBin "openh264-7.dll"
    if (Test-Path $h264_src) { Copy-Item $h264_src $BuildDir -Force }

    # libx264
    Write-Host "  - libx264 DLL" -ForegroundColor Gray
    $x264_src = Join-Path $vcpkgBin "libx264-164.dll"
    if (Test-Path $x264_src) { Copy-Item $x264_src $BuildDir -Force }

    # Qt DLLs
    $qtBin = Join-Path $QtDir "bin"
    Write-Host "  - Qt DLLs" -ForegroundColor Gray
    $qtDlls = @(
        "Qt6Core.dll","Qt6Gui.dll","Qt6Network.dll","Qt6OpenGL.dll","Qt6Qml.dll",
        "Qt6QmlMeta.dll","Qt6QmlModels.dll","Qt6QmlWorkerScript.dll","Qt6Quick.dll",
        "Qt6Quick3DUtils.dll","Qt6QuickControls2.dll","Qt6QuickTemplates2.dll",
        "Qt6QuickControls2Impl.dll","Qt6QuickControls2Basic.dll",
        "Qt6QuickControls2BasicStyleImpl.dll","Qt6QuickEffects.dll",
        "Qt6QuickLayouts.dll","Qt6QuickShapes.dll","Qt6Svg.dll",
        "Qt6QuickControls2Fusion.dll","Qt6QuickControls2FusionStyleImpl.dll",
        "Qt6QuickControls2FluentWinUI3StyleImpl.dll"
    )
    foreach ($dll in $qtDlls) {
        $src = Join-Path $qtBin $dll
        if (Test-Path $src) { Copy-Item $src $BuildDir -Force }
    }

    # Qt 平台插件
    Write-Host "  - Qt Platform Plugins" -ForegroundColor Gray
    $pluginDir = Join-Path $BuildDir "platforms"
    New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
    Copy-Item (Join-Path $QtDir "plugins\platforms\qwindows.dll") $pluginDir -Force

    # Qt 图片格式插件
    $imgDir = Join-Path $BuildDir "imageformats"
    New-Item -ItemType Directory -Force -Path $imgDir | Out-Null
    foreach ($dll in @("qgif.dll","qico.dll","qjpeg.dll","qsvg.dll")) {
        $src = Join-Path $QtDir "plugins\imageformats\$dll"
        if (Test-Path $src) { Copy-Item $src $imgDir -Force }
    }

    # Qt 图标引擎
    $icoDir = Join-Path $BuildDir "iconengines"
    New-Item -ItemType Directory -Force -Path $icoDir | Out-Null
    Copy-Item (Join-Path $QtDir "plugins\iconengines\qsvgicon.dll") $icoDir -Force

    # QML 模块
    Write-Host "  - QML Modules" -ForegroundColor Gray
    $qmlDir = Join-Path $BuildDir "qml"
    $qtQmlDir = Join-Path $QtDir "qml"
    foreach ($module in @("QtQml","QtQuick","QtQuick3DUtils")) {
        $src = Join-Path $qtQmlDir $module
        $dst = Join-Path $qmlDir $module
        if (Test-Path $src) { Copy-Item -Recurse $src $dst -Force }
    }

    # MSVC 运行时 DLLs
    Write-Host "  - MSVC Runtime DLLs" -ForegroundColor Gray
    $msvcRedist = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT\*.dll" |
        Select-Object -First 1 -ExpandProperty DirectoryName
    if ($msvcRedist) {
        Copy-Item "$msvcRedist\*.dll" $BuildDir -Force
    }

    # OpenSSL Legacy Provider (required for MD4/NTLM with Windows 7)
    Write-Host "  - OpenSSL Legacy Provider" -ForegroundColor Gray
    Copy-Item (Join-Path $vcpkgBin "legacy.dll") $BuildDir -Force -ErrorAction Stop
    $srcCnf = Join-Path $SourceDir "src\openssl.cnf"
    if (Test-Path $srcCnf) {
        Copy-Item $srcCnf (Join-Path $BuildDir "openssl.cnf") -Force
    }

    Write-Host "`n=== Deploy completed! ===" -ForegroundColor Cyan
    Write-Host "Output: $BuildDir\qf-client.exe" -ForegroundColor Green
    Write-Host "`nRun example:" -ForegroundColor Gray
    Write-Host "  cd $BuildDir" -ForegroundColor Gray
    Write-Host "  .\qf-client.exe /v:192.168.1.90 /u:administrator /p:123456 /cert:ignore /f" -ForegroundColor Gray
}
finally {
    Pop-Location
}
