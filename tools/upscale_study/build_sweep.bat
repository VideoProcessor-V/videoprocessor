@echo off
rem Builds tools\upscale_study\bin\PlScaleSweep.exe against the stable libplacebo (3rdparty\libplacebo,
rem the same sampling code as VP's scalers) and copies its runtime DLLs next to it.
rem Run from a Visual Studio x64 developer prompt (or after vcvars64.bat). Respect the build-slot
rem rules (one small cl invocation; no solution build).
setlocal
set ROOT=%~dp0..\..
set OUT=%~dp0bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++14 /EHsc /O2 /W3 /I "%ROOT%\3rdparty\libplacebo\include" "%~dp0PlScaleSweep.cpp" /Fo"%OUT%\\" /Fe"%OUT%\PlScaleSweep.exe" /link "%ROOT%\3rdparty\libplacebo\lib\libplacebo-360.lib" dxgi.lib || exit /b 1
xcopy /D /Y "%ROOT%\3rdparty\libplacebo\bin\*.dll" "%OUT%\" >nul
echo built %OUT%\PlScaleSweep.exe
