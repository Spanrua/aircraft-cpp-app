param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [string]$BuildDir = (Join-Path $RepoRoot 'build-current-release'),
    [string]$OutputRoot = (Join-Path $RepoRoot 'dist'),
    [string]$QtBin = 'D:\Qt\6.6.1\msvc2019_64\bin',
    [string]$UhdRoot = 'D:\uhd',
    [string]$VisaRoot = 'C:\Program Files\IVI Foundation\VISA\Win64',
    [string]$BundleName = 'aircraft_qt_qt6_release_20260918_final',
    [switch]$CreateZip
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

function Copy-SelectedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string[]]$ExcludeDirectoryNames = @()
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        return
    }

    Ensure-Directory -Path $Destination
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.PSIsContainer -and ($ExcludeDirectoryNames -contains $item.Name)) {
            continue
        }

        $target = Join-Path $Destination $item.Name
        if ($item.PSIsContainer) {
            Copy-SelectedDirectory -Source $item.FullName -Destination $target -ExcludeDirectoryNames $ExcludeDirectoryNames
        }
        else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
}

function Copy-DirectoryContents {
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
            Copy-SelectedDirectory -Source $item.FullName -Destination $target -ExcludeDirectoryNames @('__pycache__')
        }
        else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
}

$ParentRoot = (Resolve-Path (Join-Path $RepoRoot '..')).Path
$BundleRoot = Join-Path $OutputRoot $BundleName
$BundleApp = Join-Path $BundleRoot 'cpp_app'
$BundleDocs = Join-Path $BundleRoot 'docs'
$BundleRuntime = Join-Path $BundleRoot 'runtime-deps'
$BundleBridge = Join-Path $BundleRoot 'preprocess_integration'
$BundleTraining = Join-Path $BundleRoot 'dji_training_backend'
$BundleData = Join-Path $BundleRoot 'data'

if (Test-Path -LiteralPath $BundleRoot) {
    Remove-Item -LiteralPath $BundleRoot -Recurse -Force
}

Ensure-Directory -Path $BundleRoot
Ensure-Directory -Path $BundleApp
Ensure-Directory -Path $BundleDocs
Ensure-Directory -Path $BundleRuntime
Ensure-Directory -Path $BundleData
Ensure-Directory -Path $BundleBridge
Ensure-Directory -Path $BundleTraining

Copy-Item -LiteralPath (Join-Path $ParentRoot 'README.md') -Destination $BundleRoot -Force
Copy-Item -LiteralPath (Join-Path $ParentRoot 'requirements.txt') -Destination $BundleRoot -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'cpp_app.pro') -Destination $BundleRoot -Force
foreach ($runtimeDirectory in @('cache', 'exports', 'features', 'models', 'raw', 'samples', 'state', 'sweep')) {
    Ensure-Directory -Path (Join-Path $BundleData $runtimeDirectory)
}
Copy-DirectoryContents -Source (Join-Path $ParentRoot 'data\state') -Destination (Join-Path $BundleData 'state')
Copy-DirectoryContents -Source (Join-Path $ParentRoot 'data\features') -Destination (Join-Path $BundleData 'features')
Copy-DirectoryContents -Source (Join-Path $ParentRoot 'data\models') -Destination (Join-Path $BundleData 'models')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'README.zh-CN.md') -Destination $BundleDocs -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'requirements.md') -Destination $BundleDocs -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'requirements-python.txt') -Destination $BundleDocs -Force
Copy-SelectedDirectory -Source (Join-Path $RepoRoot 'tools') -Destination (Join-Path $BundleApp 'tools') `
    -ExcludeDirectoryNames @('__pycache__')
Copy-SelectedDirectory -Source (Join-Path $ParentRoot 'preprocess_integration') -Destination $BundleBridge `
    -ExcludeDirectoryNames @('__pycache__')
Copy-SelectedDirectory -Source (Join-Path $ParentRoot 'dji_training_backend') -Destination $BundleTraining `
    -ExcludeDirectoryNames @('__pycache__')
Copy-Item -LiteralPath (Join-Path $BuildDir 'aircraft_qt.exe') -Destination $BundleApp -Force

& (Join-Path $QtBin 'windeployqt.exe') --release --compiler-runtime --no-translations --dir $BundleApp (Join-Path $BundleApp 'aircraft_qt.exe')

Copy-FileSet -SourcePattern (Join-Path $BuildDir 'Qt6*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $BuildDir 'D3Dcompiler_47.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $BuildDir 'opengl32sw.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $UhdRoot 'bin\*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $UhdRoot 'bin\*.dll') -Destination $BundleRuntime
Copy-FileSet -SourcePattern (Join-Path $VisaRoot 'Bin\*.dll') -Destination $BundleApp
Copy-FileSet -SourcePattern (Join-Path $VisaRoot 'Bin\*.dll') -Destination $BundleRuntime

$InstallGuide = @(
    'Offline install checklist',
    '1. Copy the entire release folder to a non-system drive on the target PC.',
    '2. Run cpp_app\aircraft_qt.exe.',
    '3. Preprocess, training, and recognition require Python 3.10+ on the target PC.',
    '4. Set AIRCRAFT_PREPROCESS_PYTHON to the target Python.exe when automatic discovery is insufficient.',
    '5. UHD and NI-VISA runtime DLLs are included, but real hardware workflows still require drivers, network, and hardware.',
    '6. If a Qt platform plugin error appears, verify cpp_app\platforms\qwindows.dll and the Qt DLLs.',
    '7. data\cache, data\exports, data\sweep, and data\samples\preprocess_runs are intentionally excluded and will be created as needed.'
)
$InstallGuide | Set-Content -Encoding UTF8 (Join-Path $BundleDocs 'install-guide.zh-CN.txt')

Get-ChildItem -LiteralPath $BundleRoot -Recurse -File |
    Sort-Object FullName |
    Select-Object -ExpandProperty FullName |
    Set-Content -Encoding UTF8 (Join-Path $BundleRoot 'bundle-manifest.txt')

if ($CreateZip) {
    $ZipPath = Join-Path $OutputRoot "$BundleName.zip"
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Compress-Archive -LiteralPath $BundleRoot -DestinationPath $ZipPath -CompressionLevel Optimal
    Write-Host "Archive created at: $ZipPath"
}

Write-Host "Bundle created at: $BundleRoot"
