@echo off
REM Matted Engine commit+push helper.
REM Usage: scripts\push.bat "[AgentX][TASK-N] what changed"
if "%~1"=="" (
    echo usage: scripts\push.bat "[AgentX][TASK-N] what changed"
    exit /b 1
)
git add -A
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
