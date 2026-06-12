@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

rem Build Lilac2LilyMod with CMake + Visual Studio 2022 + MSVC/MSBuild only.
rem Then stage the UE4SS package inside this repository:
rem   third_party\ue4ss-sdk\bin\UE4SS.dll -> UE4SS_FOR_Lilac2LilyMod\UE4SS\UE4SS.dll
rem   third_party\ue4ss-sdk\bin\dwmapi.dll -> UE4SS_FOR_Lilac2LilyMod\dwmapi.dll
rem   build\<Config>\main.dll -> UE4SS_FOR_Lilac2LilyMod\UE4SS\Mods\Lilac2LilyMod\dlls\main.dll
rem   config\* -> UE4SS_FOR_Lilac2LilyMod\UE4SS\Mods\Lilac2LilyMod\dlls\config\
rem   assets\* -> UE4SS_FOR_Lilac2LilyMod\UE4SS\Mods\Lilac2LilyMod\dlls\assets\
rem
rem Usage:
rem   build.bat        build + stage package
rem   build.bat clean  clean rebuild + stage package
rem
rem Override:
rem   set L2L_BUILD_CONFIG=Release

set "RT_ROOT_BS=%~dp0"
set "RT_ROOT=%RT_ROOT_BS:~0,-1%"
set "BUILD_DIR=%RT_ROOT%\build"
if not defined L2L_BUILD_CONFIG set "L2L_BUILD_CONFIG=RelWithDebInfo"
set "CMAKE_GENERATOR=Visual Studio 17 2022"
set "CMAKE_GENERATOR_PLATFORM=x64"

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
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0

:after_vs

if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" (
    echo [clean] removing %BUILD_DIR%
    rmdir /S /Q "%BUILD_DIR%"
)

if exist "%BUILD_DIR%\build.ninja" (
    echo [configure] existing build dir is Ninja; removing %BUILD_DIR%
    rmdir /S /Q "%BUILD_DIR%"
)

if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [configure] existing build dir is not Visual Studio 2022; removing %BUILD_DIR%
        rmdir /S /Q "%BUILD_DIR%"
    )
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [configure] cmake -G "%CMAKE_GENERATOR%" -A %CMAKE_GENERATOR_PLATFORM% -B "%BUILD_DIR%"
    cmake -G "%CMAKE_GENERATOR%" -A %CMAKE_GENERATOR_PLATFORM% -B "%BUILD_DIR%" -S "%RT_ROOT%"
    if errorlevel 1 exit /b 1
)

echo [build] cmake --build (MSBuild via Visual Studio 2022, %L2L_BUILD_CONFIG%)
cmake --build "%BUILD_DIR%" --config "%L2L_BUILD_CONFIG%" --target Lilac2LilyMod -j 4
if errorlevel 1 exit /b 1

set "BUILT_DLL=%BUILD_DIR%\%L2L_BUILD_CONFIG%\main.dll"
if not exist "%BUILT_DLL%" (
    echo [ERROR] built main.dll not found: %BUILT_DLL%
    exit /b 1
)

call :stage_package
if errorlevel 1 exit /b !ERRORLEVEL!

endlocal
exit /b 0

:stage_package
set "UE4SS_SDK_BIN=%RT_ROOT%\third_party\ue4ss-sdk\bin"
set "PACKAGE_ROOT=%RT_ROOT%\UE4SS_FOR_Lilac2LilyMod"
set "PACKAGE_UE4SS_DIR=%PACKAGE_ROOT%\UE4SS"
set "MOD_DLL_DIR=%PACKAGE_UE4SS_DIR%\Mods\Lilac2LilyMod\dlls"
set "MOD_CONFIG_DIR=%MOD_DLL_DIR%\config"
set "MOD_ASSETS_DIR=%MOD_DLL_DIR%\assets"

if not exist "%UE4SS_SDK_BIN%\UE4SS.dll" (
    echo [ERROR] UE4SS.dll not found: %UE4SS_SDK_BIN%\UE4SS.dll
    exit /b 2
)
if not exist "%UE4SS_SDK_BIN%\dwmapi.dll" (
    echo [ERROR] dwmapi.dll not found: %UE4SS_SDK_BIN%\dwmapi.dll
    exit /b 3
)

if not exist "%PACKAGE_UE4SS_DIR%" mkdir "%PACKAGE_UE4SS_DIR%"
if not exist "%MOD_DLL_DIR%" mkdir "%MOD_DLL_DIR%"
if not exist "%MOD_CONFIG_DIR%" mkdir "%MOD_CONFIG_DIR%"
if not exist "%MOD_ASSETS_DIR%" mkdir "%MOD_ASSETS_DIR%"

copy /Y "%UE4SS_SDK_BIN%\UE4SS.dll" "%PACKAGE_UE4SS_DIR%\UE4SS.dll" >nul
if errorlevel 1 (
    echo [ERROR] failed to copy UE4SS.dll
    exit /b 4
)

copy /Y "%UE4SS_SDK_BIN%\dwmapi.dll" "%PACKAGE_ROOT%\dwmapi.dll" >nul
if errorlevel 1 (
    echo [ERROR] failed to copy dwmapi.dll
    exit /b 5
)

copy /Y "%BUILT_DLL%" "%MOD_DLL_DIR%\main.dll" >nul
if errorlevel 1 (
    echo [ERROR] failed to copy main.dll
    exit /b 6
)

xcopy /E /I /Y "%RT_ROOT%\config" "%MOD_CONFIG_DIR%" >nul
if errorlevel 1 (
    echo [ERROR] failed to copy config directory
    exit /b 7
)

xcopy /E /I /Y "%RT_ROOT%\assets" "%MOD_ASSETS_DIR%" >nul
if errorlevel 1 (
    echo [ERROR] failed to copy assets directory
    exit /b 8
)

echo [stage] UE4SS.dll -^> %PACKAGE_UE4SS_DIR%\UE4SS.dll
echo [stage] dwmapi.dll -^> %PACKAGE_ROOT%\dwmapi.dll
echo [stage] main.dll  -^> %MOD_DLL_DIR%\main.dll
echo [stage] config    -^> %MOD_CONFIG_DIR%
echo [stage] assets    -^> %MOD_ASSETS_DIR%
exit /b 0
