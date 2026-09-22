@echo off
setlocal EnableExtensions

set "APP_DIR=%~dp0"
set "GPU_PYTHON=D:\cqt_platform\python_envs\aircraft_preprocess_gpu\Scripts\python.exe"
set "TRAINING_BACKEND=%APP_DIR%dji_training_backend_v5\training_backend.py"
set "RECOGNITION_BACKEND=%APP_DIR%dji_training_backend_v5\recognition_backend.py"
set "QT6_BIN=D:\Qt\6.6.1\msvc2019_64\bin"
set "QT5_BIN=D:\Qt\5.12.10\5.12.10\msvc2017_64\bin"
set "UHD_BIN=D:\uhd\bin"
set "EXE="
set "QT_BIN="

if not exist "%GPU_PYTHON%" (
    echo [ERROR] GPU Python not found:
    echo         %GPU_PYTHON%
    pause
    exit /b 1
)

if not exist "%TRAINING_BACKEND%" (
    echo [ERROR] Training backend not found:
    echo         %TRAINING_BACKEND%
    pause
    exit /b 1
)

if not exist "%RECOGNITION_BACKEND%" (
    echo [ERROR] Recognition backend not found:
    echo         %RECOGNITION_BACKEND%
    pause
    exit /b 1
)

for /f "tokens=2 delims=," %%P in ('tasklist /fi "imagename eq aircraft_qt.exe" /fo csv /nh 2^>nul') do (
    if not "%%~P"=="" (
        echo The aircraft RF application is already running.
        exit /b 0
    )
)

if exist "%APP_DIR%build-ninja\aircraft_qt.exe" (
    set "EXE=%APP_DIR%build-ninja\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
) else if exist "%APP_DIR%build-qt6\aircraft_qt.exe" (
    set "EXE=%APP_DIR%build-qt6\aircraft_qt.exe"
    set "QT_BIN=%QT6_BIN%"
) else (
    echo [ERROR] No Qt application executable was found.
    echo         Checked:
    echo         %APP_DIR%build-qt6\aircraft_qt.exe
    echo         %APP_DIR%build-ninja\aircraft_qt.exe
    pause
    exit /b 1
)

set "AIRCRAFT_PREPROCESS_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_BACKEND=%TRAINING_BACKEND%"
set "AIRCRAFT_RECOGNITION_BACKEND=%RECOGNITION_BACKEND%"
set "PATH=%QT_BIN%;%UHD_BIN%;%PATH%"
set "QT_ROOT=%QT_BIN%\.."
set "QT_PLUGIN_PATH=%QT_ROOT%\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%QT_ROOT%\plugins\platforms"

cd /d "%APP_DIR%"
start "Aircraft RF Workflow" /D "%APP_DIR%" "%EXE%"
exit /b 0
