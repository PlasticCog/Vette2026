@echo off
rem Builds VETTE! 2026 with MSVC and Ninja from Visual Studio 2022.
rem Usage: build.bat [debug/release]   (default: debug)
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=debug"

rem vcvars64 also puts the CMake and Ninja bundled with VS on PATH. Skip it if already set up.
where cl >nul 2>nul && goto build
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 with the C++ x64 tools was not found. 1>&2
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

:build
cd /d "%~dp0"
if not exist build\windows-msvc\CMakeCache.txt (
    cmake --preset windows-msvc || exit /b 1
)
cmake --build --preset windows-msvc-%CONFIG%
