param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-fullflow-release'),
    [string]$OutputRoot = 'D:\cqt_platform\deliveries',
    [string]$BundleName = 'aircraft_rf_fullflow_20261015',
    [string]$QtBin = 'D:\Qt\5.12.10\5.12.10\msvc2017_64\bin',
    [string]$VcRedistRoot = '',
    [string]$GpuPythonRoot = 'D:\cqt_platform\python_envs\aircraft_preprocess_gpu',
    [string]$ModelDeliveryRoot = 'D:\cqt_platform\model_delivery_20261015'
)

$ErrorActionPreference = 'Stop'

function Require-Path {
    param([string]$Path, [string]$Description)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "缺少${Description}: ${Path}"
    }
}

$cppAppRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$aircraftProjectRoot = (Resolve-Path (Join-Path $cppAppRoot '..')).Path
$buildDir = (Resolve-Path $BuildDir).Path
$bundleRoot = Join-Path $OutputRoot $BundleName
$bundleApp = Join-Path $bundleRoot 'cpp_app'
$bundlePython = Join-Path $bundleRoot 'runtime\aircraft_preprocess_gpu'

if ([string]::IsNullOrWhiteSpace($VcRedistRoot)) {
    $redistBase = 'D:\VS\BuildTools2022\VC\Redist\MSVC'
    if (Test-Path -LiteralPath $redistBase) {
        $VcRedistRoot = Get-ChildItem -LiteralPath $redistBase -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
    }
}

Require-Path (Join-Path $buildDir 'aircraft_qt.exe') 'Release 程序'
Require-Path (Join-Path $QtBin 'windeployqt.exe') 'Qt 部署工具'
Require-Path (Join-Path $VcRedistRoot 'vcruntime140.dll') 'Visual C++ 运行库'
Require-Path (Join-Path $cppAppRoot 'tools\preprocess_runner.py') '预处理启动脚本'
Require-Path (Join-Path $cppAppRoot 'dji_training_backend_v5\recognition_backend.py') '识别后端'
Require-Path (Join-Path $aircraftProjectRoot 'preprocess_integration\iqraw_preprocess_runner.py') '预处理桥接后端'
Require-Path (Join-Path $GpuPythonRoot 'Scripts\python.exe') 'GPU Python 环境'
Require-Path (Join-Path $ModelDeliveryRoot 'best_model.pth') '交付模型'

if (Test-Path -LiteralPath $bundleRoot) {
    throw "发布目录已存在，为避免覆盖已拒绝继续：${bundleRoot}"
}

New-Item -ItemType Directory -Path $bundleApp, (Split-Path -Parent $bundlePython) | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir 'aircraft_qt.exe') -Destination $bundleApp
& (Join-Path $QtBin 'windeployqt.exe') --release --compiler-runtime --no-translations --dir $bundleApp (Join-Path $bundleApp 'aircraft_qt.exe')
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt 失败，退出码：${LASTEXITCODE}"
}
Copy-Item -LiteralPath (Get-ChildItem -LiteralPath $VcRedistRoot -File | Select-Object -ExpandProperty FullName) -Destination $bundleApp

Copy-Item -LiteralPath (Join-Path $cppAppRoot 'tools') -Destination (Join-Path $bundleApp 'tools') -Recurse
Copy-Item -LiteralPath (Join-Path $cppAppRoot 'dji_training_backend_v5') -Destination (Join-Path $bundleApp 'dji_training_backend_v5') -Recurse
Copy-Item -LiteralPath (Join-Path $aircraftProjectRoot 'preprocess_integration') -Destination (Join-Path $bundleRoot 'preprocess_integration') -Recurse
Copy-Item -LiteralPath $GpuPythonRoot -Destination $bundlePython -Recurse

$modelDestination = Join-Path $bundleRoot 'models\model_delivery_20261015'
New-Item -ItemType Directory -Path $modelDestination | Out-Null
Copy-Item -LiteralPath (Join-Path $ModelDeliveryRoot 'best_model.pth'), (Join-Path $ModelDeliveryRoot 'run_config.json'), (Join-Path $ModelDeliveryRoot 'class_names.json'), (Join-Path $ModelDeliveryRoot 'artifact_report.json') -Destination $modelDestination

New-Item -ItemType Directory -Path (Join-Path $bundleApp 'data') | Out-Null

$launcher = @'
@echo off
setlocal EnableExtensions
set "BUNDLE_ROOT=%~dp0"
set "APP_ROOT=%BUNDLE_ROOT%cpp_app"
set "GPU_PYTHON=%BUNDLE_ROOT%runtime\aircraft_preprocess_gpu\Scripts\python.exe"

if not exist "%APP_ROOT%\aircraft_qt.exe" (
  echo [ERROR] Missing application: %APP_ROOT%\aircraft_qt.exe
  pause
  exit /b 1
)
if not exist "%GPU_PYTHON%" (
  echo [ERROR] Missing GPU Python: %GPU_PYTHON%
  pause
  exit /b 1
)

set "AIRCRAFT_CPP_APP_ROOT=%APP_ROOT%"
set "AIRCRAFT_DATA_DIR=%APP_ROOT%\data"
set "AIRCRAFT_PREPROCESS_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_PYTHON=%GPU_PYTHON%"
set "AIRCRAFT_TRAINING_BACKEND=%APP_ROOT%\dji_training_backend_v5\training_backend.py"
set "AIRCRAFT_RECOGNITION_BACKEND=%APP_ROOT%\dji_training_backend_v5\recognition_backend.py"
set "PATH=%APP_ROOT%;%GPU_PYTHON%\..;%PATH%"

start "Aircraft RF Workflow" /D "%APP_ROOT%" "%APP_ROOT%\aircraft_qt.exe"
endlocal
'@
Set-Content -LiteralPath (Join-Path $bundleRoot '启动全流程系统.cmd') -Value $launcher -Encoding Ascii

$guide = @'
启动方法：双击“启动全流程系统.cmd”。

本包已包含 Qt 与 Visual C++ 运行库、GPU Python 环境、IQRAW/CAP 预处理桥接、训练后端、识别后端及外部模型导入功能。
识别页面模型下拉框右侧的“导入外部模型”可选择 .pth 或 .pt 文件。
随包模型位于 models\model_delivery_20261015\best_model.pth。
运行数据写入 cpp_app\data；不要删除 cpp_app\tools、cpp_app\dji_training_backend_v5、preprocess_integration 或 runtime 目录。
'@
Set-Content -LiteralPath (Join-Path $bundleRoot '使用说明.txt') -Value $guide -Encoding UTF8

Get-ChildItem -LiteralPath $bundleRoot -File -Recurse |
    Sort-Object FullName |
    Get-FileHash -Algorithm SHA256 |
    Select-Object Path, Hash |
    ConvertTo-Json -Depth 3 |
    Set-Content -LiteralPath (Join-Path $bundleRoot 'SHA256SUMS.json') -Encoding UTF8

Write-Output "Bundle created: $bundleRoot"
