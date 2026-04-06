@echo off
REM Build the editor app (first time only, or after source changes).
REM If already built, skip to the run line below.

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not found in PATH
    exit /b 1
)

if not exist "%~dp0build\engine_editor_app.exe" (
    echo Building engine_editor_app...
    cmake -S "%~dp0" -B "%~dp0build" || exit /b 1
    cmake --build "%~dp0build" --target engine_editor_app || exit /b 1
)

echo Launching engine_editor_app...
cd /d "%~dp0build"
engine_editor_app.exe
