@echo off
setlocal

echo ========================================
echo VDI Client - Build and Package
echo ========================================
echo.

REM Set Qt path (MSVC 2022 64-bit)
set QT_PATH=C:\Qt\6.11.1\msvc2022_64

echo [Check] Qt path: %QT_PATH%
if not exist "%QT_PATH%" (
    echo [ERROR] Qt path not found: %QT_PATH%
    echo Please modify QT_PATH variable in this script
    pause
    exit /b 1
)

set WINDEPLOYQT=%QT_PATH%\bin\windeployqt.exe
echo [Check] windeployqt path: %WINDEPLOYQT%
if not exist "%WINDEPLOYQT%" (
    echo [ERROR] windeployqt not found: %WINDEPLOYQT%
    pause
    exit /b 1
)

REM Detect VS 2022 and set up MSVC environment
set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo [ERROR] Visual Studio 2022 not found
    pause
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -property installationPath`) do set VSINSTALL=%%i
set VCVARS="%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [ERROR] vcvars64.bat not found at %VCVARS%
    pause
    exit /b 1
)
echo [Check] VS 2022 found at: %VSINSTALL%

REM Detect Inno Setup
set ISCC=
if exist "D:\Inno Setup 6\ISCC.exe" set ISCC=D:\Inno Setup 6\ISCC.exe
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set ISCC=C:\Program Files\Inno Setup 6\ISCC.exe

if "%ISCC%"=="" (
    echo [ERROR] Inno Setup not found
    echo Please download from https://jrsoftware.org/isdl.php
    pause
    exit /b 1
)

echo [Check] Inno Setup path: %ISCC%
echo.

echo [1/6] Cleaning old build files...
if exist "build" rmdir /s /q "build"
mkdir build
echo Done.

echo.
echo [2/6] Copying qf-client and dependencies...
if not exist "bin" mkdir bin
xcopy /y /i "..\qfreerdp-windows\build\qf-client.exe" "bin\"
if not exist "bin\qf-client.exe" (
    echo [ERROR] Failed to copy qf-client.exe
    pause
    exit /b 1
)
xcopy /y /i "..\qfreerdp-windows\build\*.dll" "bin\"
xcopy /y /i "..\qfreerdp-windows\build\openssl.cnf" "bin\"
xcopy /y /i /e "..\qfreerdp-windows\build\qml" "bin\qml\"
xcopy /y /i /e "..\qfreerdp-windows\build\platforms" "bin\platforms\"
xcopy /y /i /e "..\qfreerdp-windows\build\styles" "bin\styles\"
xcopy /y /i /e "..\qfreerdp-windows\build\generic" "bin\generic\"
xcopy /y /i /e "..\qfreerdp-windows\build\iconengines" "bin\iconengines\"
xcopy /y /i /e "..\qfreerdp-windows\build\imageformats" "bin\imageformats\"
xcopy /y /i /e "..\qfreerdp-windows\build\networkinformation" "bin\networkinformation\"

REM Copy UsbDk driver installer for USB redirection
if exist "..\UsbDk_1.0.22_x64.msi" (
    if not exist "drivers" mkdir drivers
    copy /y "..\UsbDk_1.0.22_x64.msi" "drivers\UsbDk_1.0.22_x64.msi"
    if exist "drivers\UsbDk_1.0.22_x64.msi" (
        echo [OK] UsbDk driver MSI copied.
    )
) else (
    echo [WARNING] UsbDk_1.0.22_x64.msi not found at parent directory.
    echo          USB redirection will not be available in the installer.
)
echo Done.

echo.
echo [3/6] Configuring CMake project...
cd build

REM Set up MSVC environment and run cmake
set PATH=C:\Program Files\CMake\bin;%PATH%
call %VCVARS%
if errorlevel 1 (
    echo [ERROR] Failed to initialize VS 2022 environment
    cd ..
    pause
    exit /b 1
)

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=%QT_PATH% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    cd ..
    pause
    exit /b 1
)
echo Done.

echo.
echo [4/6] Building project...
cmake --build . --config Release
if errorlevel 1 (
    echo [ERROR] Build failed
    cd ..
    pause
    exit /b 1
)
echo Done.

echo.
echo [5/7] Deploying Qt dependencies...
echo Using windeployqt to deploy all Qt libraries...
"%WINDEPLOYQT%" --release --no-translations Release\VDIClient.exe
if errorlevel 1 (
    echo [ERROR] Qt deployment failed
    pause
    exit /b 1
)
echo Done.

echo.
echo [6/7] Deploying VC++ runtime DLLs...
set VCRT_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT
xcopy /y "%VCRT_DIR%\msvcp140.dll" "Release\"
xcopy /y "%VCRT_DIR%\msvcp140_1.dll" "Release\"
xcopy /y "%VCRT_DIR%\msvcp140_2.dll" "Release\"
xcopy /y "%VCRT_DIR%\msvcp140_atomic_wait.dll" "Release\"
xcopy /y "%VCRT_DIR%\msvcp140_codecvt_ids.dll" "Release\"
xcopy /y "%VCRT_DIR%\vcruntime140.dll" "Release\"
xcopy /y "%VCRT_DIR%\vcruntime140_1.dll" "Release\"
xcopy /y "%VCRT_DIR%\vcruntime140_threads.dll" "Release\"
xcopy /y "%VCRT_DIR%\concrt140.dll" "Release\"
xcopy /y "%VCRT_DIR%\vccorlib140.dll" "Release\"
echo Done.

cd ..

echo.
echo [7/7] Creating installer package...
echo ========================================

if not exist "installer.iss" (
    echo [ERROR] installer installer.iss not found
    pause
    exit /b 1
)

"%ISCC%" installer.iss
if errorlevel 1 (
    echo [ERROR] Installer creation failed
    pause
    exit /b 1
)

echo.
echo ========================================
echo Build and package completed!
echo ========================================
echo.
echo Installer location: installer_output\VDIClient-Setup.exe
echo.
echo You can now run the installer for testing.
echo.
