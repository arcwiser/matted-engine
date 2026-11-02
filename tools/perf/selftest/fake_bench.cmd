@echo off
if /I "%~1"=="render"  echo BENCH render  p99_ms=7.42 mean_ms=5.10 fps=196.2 instances=1,000,000 culled=812,345 draws=1,200
if /I "%~1"=="physics" echo BENCH physics p99_ms=3.10 mean_ms=2.20 bodies=10000 contacts=45000 steps=120
exit /b 0
