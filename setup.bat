@echo off
setlocal
set "ROOT=%~dp0"

echo === Initializing RE-UE4SS submodule ===
echo (First run is large and slow - it pulls the UE4SS framework and its deps.)
git -C "%ROOT%." submodule update --init --recursive
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

echo === Setup complete. Next: build.bat --deploy ===
