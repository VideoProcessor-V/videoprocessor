@echo off
rem Copyright(C) 2026 VP-V contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds tools\td-cli\bin\camfit_cli.exe (offline CPU driver for the global camera model fitter).
rem Run after vcvars64.bat; respect the build-slot rules. The fitter sources include no pch.
setlocal
set ROOT=%~dp0..\..
set GM=%ROOT%\src\VP-V\modules\motion\estimation\global_model
set OUT=%~dp0bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\src" /I "%GM%" /I "%~dp0nvof_pch" "%~dp0camfit_cli.cpp" "%GM%\GlobalModelFit.cpp" "%GM%\GlobalModelGeometry.cpp" "%GM%\GlobalModelObjects.cpp" "%GM%\GlobalModelTemporal.cpp" "%GM%\GlobalModelSearchHints.cpp" /Fo"%OUT%\\" /Fe"%OUT%\camfit_cli.exe" || exit /b 1
echo built %OUT%\camfit_cli.exe
