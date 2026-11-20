@echo off
setlocal
rem Incremental Release build of UEVRBackend.dll (output: build\bin\uevr).
rem
rem Usage:
rem   build-dll.bat                 build only
rem   build-dll.bat "D:\path\uevr"  build, then copy the dll and pdb there
rem
rem The build folder must already be configured (see COMPILING.md).

cd /d "%~dp0"

rem --- locate cmake: PATH first, then the copy that ships with Visual Studio ---
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :have_cmake

set "CMAKE="
call :find_vs_cmake "%ProgramFiles%\Microsoft Visual Studio"
if defined CMAKE goto :have_cmake
call :find_vs_cmake "%ProgramFiles(x86)%\Microsoft Visual Studio"
if defined CMAKE goto :have_cmake

echo [build] cmake was not found on PATH or in Visual Studio.
exit /b 1

:find_vs_cmake
if not exist "%~1\" exit /b 0
for /d %%V in ("%~1\*") do for /d %%E in ("%%~V\*") do call :try_cmake "%%~E"
exit /b 0

:try_cmake
if defined CMAKE exit /b 0
if exist "%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE=%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
exit /b 0

:have_cmake
if not exist "build\CMakeCache.txt" (
  echo [build] build folder is not configured. Run this once:
  echo   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
  exit /b 1
)

echo [build] cmake --build build --config Release --target uevr
"%CMAKE%" --build build --config Release --target uevr --parallel
if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)

set "DLL=build\bin\uevr\UEVRBackend.dll"
if not exist "%DLL%" (
  echo [build] %DLL% was not produced.
  exit /b 1
)

for %%F in ("%DLL%") do echo [build] OK  %%~fF  %%~zF bytes  %%~tF

if "%~1"=="" exit /b 0

if not exist "%~1\" (
  echo [build] deploy folder "%~1" does not exist.
  exit /b 1
)

copy /y "%DLL%" "%~1\" >nul
if errorlevel 1 (
  echo [build] could not copy the dll; is the game still running?
  exit /b 1
)
if exist "build\bin\uevr\UEVRBackend.pdb" copy /y "build\bin\uevr\UEVRBackend.pdb" "%~1\" >nul
echo [build] copied to %~1
exit /b 0
