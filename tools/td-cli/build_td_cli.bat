@echo off
rem Copyright(C) 2026 VP-V contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds tools\td-cli\bin\td_cli.exe (TemporalDetail CPU model driver; header-only model, no libraries).
rem Run from a Visual Studio x64 developer prompt (or after vcvars64.bat). One small cl invocation, no
rem solution build; respect the build-slot rules.
setlocal
set ROOT=%~dp0..\..
set OUT=%~dp0bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /fp:precise /W4 /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\src" "%~dp0td_cli.cpp" /Fo"%OUT%\\" /Fe"%OUT%\td_cli.exe" || exit /b 1
echo built %OUT%\td_cli.exe
