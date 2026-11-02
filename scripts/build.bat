@echo off
setlocal
REM Matted Engine build: vswhere -> vcvars64 -> CMake (Ninja, Release)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [matted] vswhere not found - install VS 2022 Build Tools with C++ workload
    exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo [matted] no MSVC installation found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [matted] vcvars64 failed
    exit /b 1
)
if not exist build mkdir build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo [matted] build OK: build\demo.exe  build\bench.exe
