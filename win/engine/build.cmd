@echo off
rem Build mvc_engine.dll with the portable zig toolchain (win/toolchain/zig).
rem Run from win/engine/.
setlocal
set ZIG=%~dp0..\toolchain\zig\zig.exe
set RT=%~dp0..\..\MeanVC2\runtime_cpp
if not exist "%ZIG%" (
  echo ERROR: %ZIG% not found - unzip win\toolchain\zig.zip first
  exit /b 1
)
"%ZIG%" c++ mvc_engine.cpp -shared ^
  -I "%RT%" -I "%RT%\ort_include" -L "%RT%" -lonnxruntime_imp ^
  -lwinmm -lole32 -luuid ^
  -O2 -o mvc_engine.dll || exit /b 1
copy /y "%RT%\onnxruntime.dll" . >nul
echo built %~dp0mvc_engine.dll
