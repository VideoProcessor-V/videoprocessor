@echo off
rem Copyright(C) 2026 VP-V contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds tools\camera_eval\bin\camera_eval.exe (offline CPU driver for the global camera model fitter and the
rem measurement plan / reference matcher). Not part of the solution. Run from any prompt; respect the build-slot
rem rules (it is a ~20 s single-file build, no slot needed). The sources include no pch.
setlocal
if "%VSCMD_VER%"=="" call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0..\..
set GM=%ROOT%\src\VP-V\modules\motion\estimation\global_model
set OUT=%~dp0bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\src" /I "%GM%" "%~dp0camera_eval.cpp" "%GM%\GlobalModelFit.cpp" "%GM%\GlobalModelGeometry.cpp" "%GM%\GlobalModelObjects.cpp" "%GM%\GlobalModelTemporal.cpp" "%GM%\DynamicTexture.cpp" "%GM%\StaticHypothesis.cpp" "%GM%\GlobalModelSearchHints.cpp" "%GM%\GlobalModelMeasurement.cpp" "%GM%\ReferenceMatcher.cpp" /Fo"%OUT%\\" /Fe"%OUT%\camera_eval.exe" || exit /b 1
echo built %OUT%\camera_eval.exe
