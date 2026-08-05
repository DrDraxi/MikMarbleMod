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
:: dwmapi.dll is the proxy loader and MUST sit next to the game exe.
copy /Y "build\Game__Shipping__Win64\bin\dwmapi.dll"      "%MARBLES_DIR%\" >nul
if errorlevel 1 goto :deployfail
:: UE4SS.dll MUST go in the ue4ss\ subfolder - that is where the proxy loads it
:: from (matching the layout in dist\ue4ss\UE4SS.dll). Copying it to the Win64
:: root instead leaves a stale ue4ss\UE4SS.dll in place, and the game silently
:: keeps running the OLD build while your rebuilds appear to do nothing.
copy /Y "build\Game__Shipping__Win64\bin\UE4SS.dll"       "%MARBLES_DIR%\ue4ss\" >nul
if errorlevel 1 goto :deployfail
copy /Y "build\Game__Shipping__Win64\bin\MikMarbleMod.dll" "%MARBLES_DIR%\ue4ss\Mods\MikMarbleMod\dlls\main.dll" >nul
if errorlevel 1 goto :deployfail
echo === DONE ===
exit /b 0

:deployfail
:: Without this check the copies fail silently while the build still reports
:: success, so you go on testing (or shipping) a stale DLL. The usual cause is
:: the game still running and holding the files open.
echo.
echo === DEPLOY FAILED: could not copy into the game folder. ===
echo === Is Marbles on Stream still running? Close it and re-run. ===
echo === (The build itself succeeded - output is in                ===
echo ===  RE-UE4SS\build\Game__Shipping__Win64\bin\)               ===
echo.
exit /b 1
