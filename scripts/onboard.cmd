@echo off
REM Onboards a new AI agent: prints the paste-ready prompt.
REM Usage: scripts\onboard.cmd  (then copy the output into the new agent)
type "%~dp0..\ONBOARD_PROMPT.txt"
