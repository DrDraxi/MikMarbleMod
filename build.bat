@echo off
setlocal
set "ROOT=%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist "%ROOT%RE-UE4SS\CMakeLists.txt" (
    echo === RE-UE4SS submodule is missing. Run setup.bat first. ===
    exit /b 1
)

cd /d "%ROOT%RE-UE4SS"

set DO_DEPLOY=0
set DO_CLEAN=0

:parse
if "%~1"=="" goto check
if "%~1"=="--deploy" set DO_DEPLOY=1
if "%~1"=="clean" set DO_CLEAN=1
shift
goto parse

:check
if %DO_CLEAN%==1 (
    echo === Clean Build ===
    if exist build rmdir /s /q build
)

if not exist build (
    echo === CMake Configure ===
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
    if errorlevel 1 (
        echo === CONFIGURE FAILED ===
        exit /b 1
    )
)

echo === Building MikMarbleMod ===
cmake --build build --target MikMarbleMod
if errorlevel 1 (
    echo === BUILD FAILED ===
    exit /b 1
)

:: The UE4SS proxy loader (dwmapi.dll) is a separate target and is NOT pulled in
:: by the MikMarbleMod target. Build it too so --deploy has dwmapi.dll to copy.
echo === Building proxy (dwmapi.dll) ===
cmake --build build --target proxy
if errorlevel 1 (
    echo === BUILD FAILED: proxy ===
    exit /b 1
)

if %DO_DEPLOY%==0 (
    echo === BUILD DONE ^(output in RE-UE4SS\build\Game__Shipping__Win64\bin^) ===
    exit /b 0
)

echo === Deploying to Marbles on Stream ===
set "MARBLES_DIR=C:\Program Files (x86)\Steam\steamapps\common\Marbles on Stream\MarblesOnStream\Binaries\Win64"
copy /Y "build\Game__Shipping__Win64\bin\UE4SS.dll"       "%MARBLES_DIR%\" >nul
copy /Y "build\Game__Shipping__Win64\bin\dwmapi.dll"      "%MARBLES_DIR%\" >nul
copy /Y "build\Game__Shipping__Win64\bin\MikMarbleMod.dll" "%MARBLES_DIR%\ue4ss\Mods\MikMarbleMod\dlls\main.dll" >nul
echo === DONE ===
