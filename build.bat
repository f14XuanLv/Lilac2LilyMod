@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

rem Configure and build Lilac2LilyMod with the pinned RE-UE4SS submodule.
rem CMake's post-build step packages config, assets, and dlls\main.dll.
rem This script does not modify or stage files inside RE-UE4SS.
rem
rem Usage:
rem   build.bat        configure (if needed) and build
rem   build.bat clean  clean, configure, and build
rem
rem Override:
rem   set L2L_BUILD_CONFIG=Release

set "RT_ROOT_BS=%~dp0"
set "RT_ROOT=%RT_ROOT_BS:~0,-1%"
set "BUILD_DIR=%RT_ROOT%\build"
if not defined L2L_BUILD_CONFIG set "L2L_BUILD_CONFIG=Game__Shipping__Win64"
set "CMAKE_GENERATOR=Ninja"
set "MSVC_TOOLSET_VERSION=14.44"

set "DO_CLEAN=0"
for %%A in (%*) do (
    if /I "%%~A"=="clean" set "DO_CLEAN=1"
)

call :setup_vs
if errorlevel 1 exit /b 1
goto :after_vs

:setup_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo [ERROR] vswhere.exe not found: %VSWHERE% && exit /b 1
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%I"
if not defined VS echo [ERROR] VS installation not found via vswhere && exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=%MSVC_TOOLSET_VERSION% >nul
exit /b 0

:after_vs
if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" (
    echo [clean] removing %BUILD_DIR%
    rmdir /S /Q "%BUILD_DIR%"
)

if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [configure] existing build dir is not Ninja; removing %BUILD_DIR%
        rmdir /S /Q "%BUILD_DIR%"
    )
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    if not defined L2L_BUILD_CONFIG set "L2L_BUILD_CONFIG=Game__Shipping__Win64"
    echo [configure] cmake -G "%CMAKE_GENERATOR%" -DCMAKE_BUILD_TYPE=%L2L_BUILD_CONFIG% -B "%BUILD_DIR%"
    cmake -G "%CMAKE_GENERATOR%" -DCMAKE_BUILD_TYPE=%L2L_BUILD_CONFIG% -B "%BUILD_DIR%" -S "%RT_ROOT%"
    if errorlevel 1 exit /b 1
)

echo [build] cmake --build (Ninja with MSVC %MSVC_TOOLSET_VERSION%, %L2L_BUILD_CONFIG%)
cmake --build "%BUILD_DIR%" --target Lilac2LilyMod -j 4
if errorlevel 1 exit /b 1

echo [done] main.dll, config, and assets are in the Lilac2LilyMod target output directory.
endlocal
exit /b 0
