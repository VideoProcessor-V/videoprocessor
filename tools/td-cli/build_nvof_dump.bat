@echo off
rem Copyright(C) 2026 VP-V contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds tools\td-cli\bin\nvof_dump.exe (offline NVOFA vector dumper reusing VP's NvOpticalFlow wrapper).
rem Run after vcvars64.bat; respect the build-slot rules.
setlocal
set ROOT=%~dp0..\..
set OUT=%~dp0bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I "%~dp0nvof_pch" /I "%ROOT%" /I "%ROOT%\3rdparty" /I "%ROOT%\src" "%~dp0nvof_dump.cpp" "%ROOT%\src\VP-V\modules\motion\estimation\NvOpticalFlow.cpp" /Fo"%OUT%\\" /Fe"%OUT%\nvof_dump.exe" /link d3d11.lib dxgi.lib || exit /b 1
echo built %OUT%\nvof_dump.exe
