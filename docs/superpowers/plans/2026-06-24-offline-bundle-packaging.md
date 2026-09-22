# cpp_app Offline Bundle Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a self-contained offline delivery folder for `aircraft_qt` that includes the app, Qt runtime, UHD runtime, NI-VISA runtime, the `preprocess_integration` bridge, and a concise offline installation note.

**Architecture:** Keep the bundle generator as a single PowerShell script under `docs/superpowers/scripts` so the packaging process is repeatable. The script copies the release binary, deploys Qt with `windeployqt`, mirrors UHD and NI-VISA runtime DLLs, preserves the parent `preprocess_integration` directory needed by `tools/preprocess_runner.py`, and writes a human-readable install checklist beside the bundle.

**Tech Stack:** PowerShell 5.1, existing `build-release` artifacts, Qt 6.6.1 `windeployqt`, UHD SDK runtime files, NI-VISA runtime files.

---

### Task 1: Add offline bundle script and install note

**Files:**
- Create: `D:/cqt_platform/aircraft_project/cpp_app/docs/superpowers/scripts/package-offline-bundle.ps1`
- Create: `D:/cqt_platform/aircraft_project/cpp_app/docs/superpowers/plans/2026-06-24-offline-bundle-packaging.md`

- [ ] **Step 1: Write the bundling script**

```powershell
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path,
    [string]$BuildDir = (Join-Path $RepoRoot 'build-release'),
    [string]$OutputRoot = (Join-Path $RepoRoot 'dist'),
    [string]$QtBin = 'D:\Qt\6.6.1\msvc2019_64\bin',
    [string]$UhdRoot = 'D:\uhd',
    [string]$VisaRoot = 'C:\Program Files\IVI Foundation\VISA\Win64',
    [string]$BundleName = 'cpp_app-offline-bundle'
)

$ErrorActionPreference = 'Stop'

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Copy-DirectoryTree {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        return
    }

    Ensure-Directory -Path $Destination
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        $target = Join-Path $Destination $item.Name
        if ($item.PSIsContainer) {
            Copy-DirectoryTree -Source $item.FullName -Destination $target
        }
        else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
}

function Copy-FileSet {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePattern,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Destination)) {
        Ensure-Directory -Path $Destination
    }

    foreach ($file in Get-ChildItem -Path $SourcePattern -File -ErrorAction SilentlyContinue) {
        Copy-Item -LiteralPath $file.FullName -Destination $Destination -Force
    }
}

$ParentRoot = (Resolve-Path (Join-Path $RepoRoot '..')).Path
$BundleRoot = Join-Path $OutputRoot $BundleName
$BundleApp = Join-Path $BundleRoot 'cpp_app'
$BundleDocs = Join-Path $BundleRoot 'docs'
$BundleRuntime = Join-Path $BundleRoot 'runtime-deps'
$BundleBridge = Join-Path $BundleRoot 'preprocess_integration'
$BundleData = Join-Path $BundleRoot 'data'

if (Test-Path -LiteralPath $BundleRoot) {
    Remove-Item -LiteralPath $BundleRoot -Recurse -Force
}

Ensure-Directory -Path $BundleRoot
Ensure-Directory -Path $BundleApp
Ensure-Directory -Path $BundleDocs
Ensure-Directory -Path $BundleRuntime
Ensure-Directory -Path $BundleBridge
Ensure-Directory -Path $BundleData

Copy-Item -LiteralPath (Join-Path $ParentRoot 'README.md') -Destination $BundleRoot -Force
Copy-Item -LiteralPath (Join-Path $ParentRoot 'requirements.txt') -Destination $BundleRoot -Force
Copy-DirectoryTree -Source (Join-Path $ParentRoot 'data') -Destination $BundleData
Copy-DirectoryTree -Source (Join-Path $RepoRoot 'tools') -Destination (Join-Path $BundleRoot 'cpp_app\tools')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'README.zh-CN.md') -Destination $BundleDocs -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'requirements.md') -Destination $BundleDocs -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'requirements-python.txt') -Destination $BundleDocs -Force
Copy-Item -LiteralPath (Join-Path $BuildDir 'aircraft_qt.exe') -Destination $BundleApp -Force

& (Join-Path $QtBin 'windeployqt.exe') --release --compiler-runtime --no-translations --dir $BundleApp (Join-Path $BundleApp 'aircraft_qt.exe')

Copy-FileSet -SourcePattern (Join-Path $BuildDir 'Qt6*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $BuildDir 'D3Dcompiler_47.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $BuildDir 'opengl32sw.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $UhdRoot 'bin\*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $UhdRoot 'bin\*.dll') -Destination $BundleRuntime
Copy-FileSet -SourcePattern (Join-Path $VisaRoot 'Bin\*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $VisaRoot 'Bin\*.dll') -Destination $BundleRuntime
Copy-DirectoryTree -Source (Join-Path $ParentRoot 'preprocess_integration') -Destination $BundleBridge

@'
离线安装清单
1. 将整个 `cpp_app-offline-bundle` 文件夹拷贝到目标电脑的任意非系统盘目录。
2. 直接运行 `cpp_app\\aircraft_qt.exe`。
3. 若目标机缺少 UHD 或 NI-VISA 驱动层，请先完成随包运行时的离线安装或保留目标机原有安装。
4. 预处理功能依赖 `preprocess_integration` 目录与随包 Python 桥接脚本，不能只拷贝 exe。
5. 如果启动提示 Qt 平台插件错误，检查 `cpp_app` 目录下 DLL 和插件目录是否完整。
6. 如果后续接入新的模型权重，请同步放入 `data\\models`。
'@ | Set-Content -Encoding UTF8 (Join-Path $BundleDocs 'install-guide.zh-CN.txt')

Get-ChildItem -LiteralPath $BundleRoot -Recurse -File |
    Sort-Object FullName |
    Select-Object -ExpandProperty FullName |
    Set-Content -Encoding UTF8 (Join-Path $BundleRoot 'bundle-manifest.txt')

Write-Host "Bundle created at: $BundleRoot"
```

- [ ] **Step 2: Run the script**

Run: `powershell -ExecutionPolicy Bypass -File D:/cqt_platform/aircraft_project/cpp_app/docs/superpowers/scripts/package-offline-bundle.ps1`
Expected: `D:/cqt_platform/aircraft_project/cpp_app/dist/cpp_app-offline-bundle` exists and contains `cpp_app`, `runtime-deps`, `docs`, `data`, and `preprocess_integration`.

- [ ] **Step 3: Verify bundle contents**

Run: `Get-ChildItem D:/cqt_platform/aircraft_project/cpp_app/dist/cpp_app-offline-bundle -Recurse | Select-Object FullName`
Expected: `aircraft_qt.exe`, Qt DLLs, UHD DLLs, VISA DLLs, `install-guide.zh-CN.txt`, and the preprocessing bridge files are present.

- [ ] **Step 4: Commit the packaging assets**

```bash
git add docs/superpowers/plans/2026-06-24-offline-bundle-packaging.md docs/superpowers/scripts/package-offline-bundle.ps1
git commit -m "docs: add offline bundle packaging script"
```
