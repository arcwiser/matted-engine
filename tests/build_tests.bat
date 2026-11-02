@echo off
rem Matted Engine - standalone test-suite build + run (AgentB, tests/**)
rem Does NOT depend on AgentA's CMake/scripts; links only the engine TUs the
rem tests exercise (narrowphase/physics excluded on purpose).
setlocal
cd /d "%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [tests] vswhere not found - install VS 2022 Build Tools
  exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [tests] no VS installation with VC tools found
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [tests] vcvars64 failed
  exit /b 1
)

if not exist build\tests\obj mkdir build\tests\obj

cl /nologo /std:c++20 /O2 /fp:fast /EHsc /MP /W3 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I. ^
  tests\main.cpp tests\test_math.cpp tests\test_memory.cpp tests\test_jobs.cpp ^
  tests\test_ecs.cpp tests\test_render.cpp tests\test_core.cpp ^
  engine\core\log.cpp engine\core\memory\allocators.cpp engine\core\jobs\job_system.cpp ^
  engine\core\profiler.cpp engine\render\mesh.cpp engine\render\render_queue.cpp ^
  /Fe:build\tests\tests.exe /Fo:build\tests\obj\
if errorlevel 1 (
  echo [tests] BUILD FAILED
  exit /b 1
)

build\tests\tests.exe
set RC=%errorlevel%
if %RC% neq 0 (
  echo [tests] SUITE FAILED ^(exit %RC%^)
) else (
  echo [tests] SUITE GREEN
)
exit /b %RC%
