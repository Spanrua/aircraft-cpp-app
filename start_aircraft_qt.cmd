@echo off
setlocal EnableExtensions

set "APP_DIR=%~dp0"
set "GPU_PYTHON=D:\cqt_platform\python_envs\aircraft_preprocess_gpu\Scripts\python.exe"
set "TRAINING_BACKEND=%APP_DIR%dji_training_backend_v5\training_backend.py"
set "RECOGNITION_BACKEND=%APP_DIR%dji_training_backend_v5\recognition_backend.py"
set "QT6_BIN=D:\Qt\6.6.1\msvc2019_64\bin"
set "QT6_DEPLOY=%QT6_BIN%\windeployqt.exe"
set "QT5_BIN=D:\Qt\5.12.10\5.12.10\msvc2017_64\bin"
set "QT5_ROOT=D:\Qt\5.12.10\5.12.10\msvc2017_64"
set "QT5_DEPLOY=%QT5_BIN%\windeployqt.exe"
set "UHD_BIN=D:\uhd\bin"
set "VISA_BIN=C:\Program Files\IVI Foundation\VISA\Win64\Bin"
if not exist "%VISA_BIN%\visa64.dll" if exist "%APP_DIR%dist\aircraft_qt_qt51210_portable\visa64.dll" set "VISA_BIN=%APP_DIR%dist\aircraft_qt_qt51210_portable"
if not exist "%VISA_BIN%\visa64.dll" if exist "D:\lzy\visa64.dll" set "VISA_BIN=D:\lzy"
set "EXE="
set "BUILD_DIR="
set "QT_BIN="
set "QT_ROOT="

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

if exist "%APP_DIR%build-fullflow-release-segment-fusion\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-fullflow-release-segment-fusion"
    set "EXE=%APP_DIR%build-fullflow-release-segment-fusion\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
    set "QT_ROOT=%QT5_ROOT%"
) else if exist "%APP_DIR%build-fullflow-release-batchfix\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-fullflow-release-batchfix"
    set "EXE=%APP_DIR%build-fullflow-release-batchfix\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
    set "QT_ROOT=%QT5_ROOT%"
) else if exist "%APP_DIR%build-fullflow-release\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-fullflow-release"
    set "EXE=%APP_DIR%build-fullflow-release\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
    set "QT_ROOT=%QT5_ROOT%"
) else if exist "%APP_DIR%build-qt6\Release\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-qt6\Release"
    set "EXE=%APP_DIR%build-qt6\Release\aircraft_qt.exe"
    set "QT_BIN=%QT6_BIN%"
    set "QT_ROOT=D:\Qt\6.6.1\msvc2019_64"
) else if exist "%APP_DIR%build-ninja\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-ninja"
    set "EXE=%APP_DIR%build-ninja\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
    set "QT_ROOT=%QT5_ROOT%"
) else if exist "%APP_DIR%build-qt6\aircraft_qt.exe" (
    set "BUILD_DIR=%APP_DIR%build-qt6"
    set "EXE=%APP_DIR%build-qt6\aircraft_qt.exe"
    set "QT_BIN=%QT6_BIN%"
    set "QT_ROOT=D:\Qt\6.6.1\msvc2019_64"
) else if exist "%APP_DIR%build-batch-tests\aircraft_qt.exe" (
    rem Isolated debug build used for local verification and direct desktop launch.
    set "BUILD_DIR=%APP_DIR%build-batch-tests"
    set "EXE=%APP_DIR%build-batch-tests\aircraft_qt.exe"
    set "QT_BIN=%QT5_BIN%"
    set "QT_ROOT=%QT5_ROOT%"
) else (
    echo [ERROR] No Qt application executable was found.
    echo         Checked:
    echo         %APP_DIR%build-qt6\aircraft_qt.exe
    echo         %APP_DIR%build-ninja\aircraft_qt.exe
    echo         %APP_DIR%build-batch-tests\aircraft_qt.exe
    echo         %APP_DIR%build-fullflow-release\aircraft_qt.exe
    echo         %APP_DIR%build-fullflow-release-batchfix\aircraft_qt.exe
    echo         %APP_DIR%build-fullflow-release-segment-fusion\aircraft_qt.exe
    pause
    exit /b 1
)

if "%QT_BIN%"=="%QT5_BIN%" if not exist "%BUILD_DIR%\Qt5Cored.dll" (
    if not exist "%QT5_DEPLOY%" (
        echo [ERROR] Qt5 runtime and windeployqt are missing:
        echo         %BUILD_DIR%\Qt5Cored.dll
        echo         %QT5_DEPLOY%
        pause
        exit /b 1
    )
    echo Deploying Qt5 Debug runtime to:
    echo         %BUILD_DIR%
    "%QT5_DEPLOY%" --debug --compiler-runtime --no-translations --verbose 0 "%EXE%"
    if errorlevel 1 (
        echo [ERROR] Qt5 runtime deployment failed.
        pause
        exit /b 1
    )
)

if "%QT_BIN%"=="%QT5_BIN%" if not exist "%BUILD_DIR%\Qt5Cored.dll" (
    echo [ERROR] Qt5 runtime is still missing:
    echo         %BUILD_DIR%\Qt5Cored.dll
    pause
    exit /b 1
)

if "%QT_BIN%"=="%QT6_BIN%" if not exist "%BUILD_DIR%\Qt6Core.dll" (
    if not exist "%QT6_DEPLOY%" (
        echo [ERROR] Qt6 runtime and windeployqt are missing:
        echo         %BUILD_DIR%\Qt6Core.dll
        echo         %QT6_DEPLOY%
        pause
        exit /b 1
    )
    echo Deploying Qt6 runtime to:
    echo         %BUILD_DIR%
    "%QT6_DEPLOY%" --release --compiler-runtime --no-translations --verbose 0 "%EXE%"
    if errorlevel 1 (
        echo [ERROR] Qt6 runtime deployment failed.
        pause
        exit /b 1
    )
)

if "%QT_BIN%"=="%QT6_BIN%" if not exist "%BUILD_DIR%\Qt6Core.dll" (
    echo [ERROR] Qt6 runtime is still missing:
    echo         %BUILD_DIR%\Qt6Core.dll
    pause
    exit /b 1
)

if not exist "%BUILD_DIR%\platforms\qwindows.dll" (
    echo [ERROR] Qt platform plugin is missing:
    echo         %BUILD_DIR%\platforms\qwindows.dll
    pause
    exit /b 1
)

set "AIRCRAFT_PREPROCESS_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_BACKEND=%TRAINING_BACKEND%"
set "AIRCRAFT_RECOGNITION_BACKEND=%RECOGNITION_BACKEND%"
set "PATH=%BUILD_DIR%;%QT_BIN%;%UHD_BIN%;%VISA_BIN%;%PATH%"
set "QT_PLUGIN_PATH=%BUILD_DIR%"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%BUILD_DIR%\platforms"

cd /d "%APP_DIR%"
start "Aircraft RF Workflow" /D "%APP_DIR%" "%EXE%"
exit /b 0
