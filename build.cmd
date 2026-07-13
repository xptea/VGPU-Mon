@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "RUN_TESTS=%~2"
set "OUTPUT_NAME=%~3"
if "%OUTPUT_NAME%"=="" set "OUTPUT_NAME=vgpu-mon"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Installer's vswhere.exe was not found.
  exit /b 1
)

set "VSINSTALL="
set "VSREQUIRES=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if /i "%CONFIG%"=="Sanitize" set "VSREQUIRES=Microsoft.VisualStudio.Component.VC.ASAN"
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires %VSREQUIRES% -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  if /i "%CONFIG%"=="Sanitize" (
    echo ERROR: Visual Studio C++ AddressSanitizer tools were not found.
    echo Install "C++ AddressSanitizer" from the Visual Studio Installer.
  ) else (
    echo ERROR: Visual Studio C++ build tools were not found.
  )
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if /i "%CONFIG%"=="Sanitize" (
  set "CFLAGS=/nologo /std:c11 /utf-8 /W4 /WX /sdl /O1 /Zi /MT /fsanitize=address /DVGPU_ASAN"
  set "LFLAGS=/DEBUG /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT /MANIFEST:EMBED"
) else if /i "%CONFIG%"=="Debug" (
  set "CFLAGS=/nologo /std:c11 /utf-8 /W4 /WX /sdl /Od /Zi /MTd /D_DEBUG"
  set "LFLAGS=/DEBUG /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /CETCOMPAT /MANIFEST:EMBED"
) else (
  set "CFLAGS=/nologo /std:c11 /utf-8 /W4 /WX /sdl /O2 /GL /MT /DNDEBUG /Brepro"
  set "LFLAGS=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /CETCOMPAT /MANIFEST:EMBED /Brepro"
)

if not exist build\obj\app mkdir build\obj\app
if not exist build\obj\tests mkdir build\obj\tests

rc /nologo /fo build\obj\app\vgpu.res src\vgpu.rc
if errorlevel 1 exit /b %errorlevel%

cl %CFLAGS% /Fo:build\obj\app\ /Fd:build\obj\app\compiler.pdb /Fe:build\%OUTPUT_NAME%.exe ^
  src\main.c src\util.c src\nvml_dyn.c src\dxgi_gpu.c src\pdh_gpu.c build\obj\app\vgpu.res ^
  /link %LFLAGS% pdh.lib dxgi.lib dxguid.lib psapi.lib advapi32.lib shell32.lib ole32.lib
if errorlevel 1 exit /b %errorlevel%

if /i "%RUN_TESTS%"=="test" (
  cl %CFLAGS% /Fo:build\obj\tests\ /Fd:build\obj\tests\compiler.pdb /Fe:build\vgpu-tests.exe ^
    tests\test_core.c src\util.c src\pdh_gpu.c ^
    /link %LFLAGS% pdh.lib psapi.lib advapi32.lib
  if errorlevel 1 exit /b 1
  build\vgpu-tests.exe
  if errorlevel 1 exit /b 1
  cl %CFLAGS% /Fo:build\obj\tests\ /Fd:build\obj\tests\conpty-compiler.pdb /Fe:build\vgpu-conpty-tests.exe ^
    tests\test_conpty.c
  if errorlevel 1 exit /b 1
  build\vgpu-conpty-tests.exe build\%OUTPUT_NAME%.exe
  if errorlevel 1 exit /b 1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\test_cli.ps1 -Executable build\%OUTPUT_NAME%.exe
  if errorlevel 1 exit /b 1
)

echo Built %CD%\build\%OUTPUT_NAME%.exe
