@echo off
setlocal
set "ROOT=%~dp0"

echo === Initializing RE-UE4SS submodule ===
echo (First run is large and slow - it pulls the UE4SS framework and its deps.)
echo (-c url.insteadOf rewrites UE4SS's SSH submodule URLs to HTTPS so this works
echo  even without a configured GitHub SSH key.)
git -C "%ROOT%." -c url.https://github.com/.insteadOf=git@github.com: submodule update --init --recursive
if errorlevel 1 (
    echo === Submodule init FAILED. Is git installed and is this a git clone? ===
    exit /b 1
)

set "CPPMODS=%ROOT%RE-UE4SS\cppmods\CMakeLists.txt"
if not exist "%CPPMODS%" (
    echo === %CPPMODS% not found - submodule did not populate. ===
    exit /b 1
)

findstr /C:"MikMarbleMod" "%CPPMODS%" >nul 2>&1 && goto :registered
echo === Registering MikMarbleMod in cppmods\CMakeLists.txt ===
>>"%CPPMODS%" echo add_subdirectory^("${CMAKE_CURRENT_SOURCE_DIR}/../../src/MikMarbleMod" "MikMarbleMod"^)
:registered

:: The game runs on UE 5.8, which upstream UE4SS does not support. Our 5.8
:: versioned container lives on a fork of the nested Unreal submodule, so the
:: submodule init above has just reset it away. Put it back, otherwise the build
:: succeeds but UE4SS aborts at runtime with "Engine version is not supported."
echo === Restoring UE 5.8 support into the Unreal submodule ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\restore-ue58-support.ps1"
if errorlevel 1 (
    echo.
    echo === WARNING: UE 5.8 support was NOT restored. ===
    echo === The build will still succeed, but the game will fail at startup with
    echo ===   Fatal Error: Engine version is not supported.
    echo === Fix it with: tools\restore-ue58-support.ps1
    echo.
)

echo === Setup complete. Next: build.bat --deploy ===
