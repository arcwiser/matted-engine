@echo off
REM Matted Engine commit+push helper.
REM Usage: scripts\push.bat "[AgentX][TASK-N] what changed" your\paths...
REM Stages ONLY the paths you pass - never another agent's files.
if "%~1"=="" (
    echo usage: scripts\push.bat "[AgentX][TASK-N] what changed" your\paths...
    echo   example: scripts\push.bat "[AgentB][TASK-1] D3D12 mesh upload" engine\render\d3d12\*
    exit /b 1
)
setlocal enabledelayedexpansion
set "PATHS="
:collect
shift
if "%~1"=="" goto :done
set "PATHS=!PATHS! "%~1""
goto :collect
:done
if not defined PATHS (
    echo [git] ERROR: no paths given - stage ONLY your own files, e.g.
    echo [git]   scripts\push.bat "[AgentB][TASK-1] msg" engine\render\d3d12\*
    exit /b 1
)
git add %PATHS%
git commit -m "%~1"
if errorlevel 1 (
    echo [git] nothing to commit or commit failed
    exit /b 1
)
git push
if errorlevel 1 (
    echo [git] push rejected - run: git pull --rebase, resolve, then push again
    exit /b 1
)
echo [git] pushed to origin main
