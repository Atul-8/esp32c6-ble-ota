@echo off
rem start_gui.bat - launch BLE OTA GUI (keep this file ASCII-only: cmd.exe mangles UTF-8 Chinese comments)
rem Usage: double-click, or run from any terminal. No args.

setlocal
title BLE OTA Test GUI - ESP32-C6

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] python not found. Install Python 3.10+ and add to PATH.
    pause
    exit /b 1
)

python -c "import bleak" >nul 2>nul
if errorlevel 1 (
    echo [INFO] First run: installing bleak ...
    python -m pip install bleak
    if errorlevel 1 (
        echo [ERROR] bleak install failed. Check network and retry.
        pause
        exit /b 1
    )
)

echo [INFO] Starting BLE OTA GUI ...
python "%SCRIPT_DIR%ble_ota_gui.py"
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    echo.
    echo [ERROR] GUI exited with code %RC%
    pause
    exit /b %RC%
)
endlocal
