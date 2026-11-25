@echo off
rem _stdcall: MSVC keyword used by onnxruntime_c_api.h, missing in MinGW (no-op on x64)
g++ -std=c++17 -O2 -s -static -fno-exceptions -fno-rtti -mwindows -municode -Wall -Wno-misleading-indentation ^
  -D_stdcall=__stdcall -Iinclude ^
  mousefinger.cpp -o mousefinger.exe ^
  -lmfreadwrite -lmf -lmfplat -lmfuuid -lole32 -luuid -lpsapi -lshell32 -lgdi32 -luser32
if errorlevel 1 exit /b 1
echo OK: mousefinger.exe
