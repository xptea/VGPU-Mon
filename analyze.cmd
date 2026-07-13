@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Installer's vswhere.exe was not found.
  exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo ERROR: Visual Studio C++ build tools were not found.
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if exist build\obj\analyze rmdir /s /q build\obj\analyze
mkdir build\obj\analyze\app
mkdir build\obj\analyze\core
mkdir build\obj\analyze\conpty

set "ANALYZE_FLAGS=/nologo /c /std:c11 /utf-8 /W4 /WX /sdl /analyze /analyze:external- /D_DEBUG"

cl %ANALYZE_FLAGS% /Fo:build\obj\analyze\app\ ^
  src\main.c src\util.c src\nvml_dyn.c src\dxgi_gpu.c src\pdh_gpu.c
if errorlevel 1 exit /b 1

cl %ANALYZE_FLAGS% /Fo:build\obj\analyze\core\ ^
  tests\test_core.c src\util.c src\pdh_gpu.c
if errorlevel 1 exit /b 1

cl %ANALYZE_FLAGS% /Fo:build\obj\analyze\conpty\ tests\test_conpty.c
if errorlevel 1 exit /b 1

echo MSVC static analysis passed.
