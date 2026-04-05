@echo off
rem DXC-first shader build script with FXC fallback.

setlocal EnableExtensions EnableDelayedExpansion
set error=0

if %PROCESSOR_ARCHITECTURE%.==ARM64. (
    set SHADERARCH=arm64
) else (
    if %PROCESSOR_ARCHITECTURE%.==AMD64. (
        set SHADERARCH=x64
    ) else (
        set SHADERARCH=x86
    )
)

call :FindDXC
call :FindFXC

if not defined CompileShadersOutput set CompileShadersOutput=Compiled
set StrTrim=%CompileShadersOutput%##
set StrTrim=%StrTrim: ##=%
set CompileShadersOutput=%StrTrim:##=%
if not exist "%CompileShadersOutput%" mkdir "%CompileShadersOutput%"

if defined PCDXC (
    echo Using DXC: %PCDXC%
) else (
    if defined PCFXC (
        echo DXC not found. Falling back to FXC: %PCFXC%
    ) else (
        echo Neither DXC nor FXC could be located.
        exit /b 1
    )
)

call :CompileShader alpha_luminance_sprite_ps ps SpritePixelShader
call :CompileShader alpha_luminance_sprite_ps vs SpriteVertexShader

if %error% == 0 (
    echo Shaders compiled ok
) else (
    echo There were shader compilation errors!
    exit /b 1
)

endlocal
exit /b 0

:FindDXC
set PCDXC=
set CANDIDATE="%WindowsSdkVerBinPath%%SHADERARCH%\dxc.exe"
if exist %CANDIDATE% set PCDXC=%CANDIDATE%
if defined PCDXC exit /b

set CANDIDATE="%WindowsSdkBinPath%%WindowsSDKVersion%\%SHADERARCH%\dxc.exe"
if exist %CANDIDATE% set PCDXC=%CANDIDATE%
if defined PCDXC exit /b

set CANDIDATE="%WindowsSdkDir%bin\%WindowsSDKVersion%\%SHADERARCH%\dxc.exe"
if exist %CANDIDATE% set PCDXC=%CANDIDATE%
if defined PCDXC exit /b

if exist "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\%SHADERARCH%\dxc.exe" (
    set PCDXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\%SHADERARCH%\dxc.exe"
    exit /b
)

for /f "delims=" %%I in ('where dxc 2^>nul') do (
    set PCDXC="%%~fI"
    goto :eof
)
exit /b

:FindFXC
set PCFXC=
set CANDIDATE="%WindowsSdkVerBinPath%%SHADERARCH%\fxc.exe"
if exist %CANDIDATE% set PCFXC=%CANDIDATE%
if defined PCFXC exit /b

set CANDIDATE="%WindowsSdkBinPath%%WindowsSDKVersion%\%SHADERARCH%\fxc.exe"
if exist %CANDIDATE% set PCFXC=%CANDIDATE%
if defined PCFXC exit /b

set CANDIDATE="%WindowsSdkDir%bin\%WindowsSDKVersion%\%SHADERARCH%\fxc.exe"
if exist %CANDIDATE% set PCFXC=%CANDIDATE%
if defined PCFXC exit /b

for /f "delims=" %%I in ('where fxc 2^>nul') do (
    set PCFXC="%%~fI"
    goto :eof
)
exit /b

:CompileShader
set SOURCEFILE=%1.fx
if not exist "%SOURCEFILE%" set SOURCEFILE=%1.hlsl
if not exist "%SOURCEFILE%" (
    echo Missing shader source: %1.fx or %1.hlsl
    set error=1
    exit /b
)

set attempt_error=0

if defined PCDXC (
    set DXCOPTS=-nologo -WX -Ges -Zi -Zpc -Qstrip_reflect -Qstrip_debug -HV 2021
    set CMD=%PCDXC% "!SOURCEFILE!" !DXCOPTS! -T %2_6_0 -E %3 -Fh"%CompileShadersOutput%\%1_%3.inc" -Fd"%CompileShadersOutput%\%1_%3.pdb" -Vn%1_%3
    echo.
    echo !CMD!
    !CMD! || set attempt_error=1
    if !attempt_error! == 0 exit /b
    echo DXC compile failed for %SOURCEFILE%, trying FXC fallback...
)

if not defined PCFXC (
    set error=1
    exit /b
)

set FXCOPTS=/nologo /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug
set CMD=%PCFXC% "!SOURCEFILE!" !FXCOPTS! /T%2_5_0 /E%3 "/Fh%CompileShadersOutput%\%1_%3.inc" "/Fd%CompileShadersOutput%\%1_%3.pdb" /Vn%1_%3
echo.
echo !CMD!
!CMD! || set error=1
exit /b
