[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Assert-Ignored {
    param([Parameter(Mandatory = $true)][string]$Path)

    & git check-ignore -q -- $Path
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        return
    }

    if ($exitCode -eq 1) {
        throw "Expected '$Path' to be ignored, but it is reviewable."
    }

    throw "Unable to determine ignore status for '$Path' (git check-ignore exit code $exitCode)."
}

function Assert-Reviewable {
    param([Parameter(Mandatory = $true)][string]$Path)

    & git check-ignore -q -- $Path
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 1) {
        return
    }

    if ($exitCode -eq 0) {
        throw "Expected '$Path' to be reviewable, but it is ignored."
    }

    throw "Unable to determine ignore status for '$Path' (git check-ignore exit code $exitCode)."
}

$ignoredPaths = @(
    'build-ninja/aircraft_qt.exe',
    'dist/release/aircraft_qt.exe',
    'backups/aircraft.zip',
    'fullflow-release-replay/report.json',
    'data/models/best_model.pth',
    'captures/example.iqraw',
    'state/app_state.json',
    'dji_training_backend_v5.zip',
    '$out'
)

$reviewablePaths = @(
    'src/app/ProjectPaths.cpp',
    'tests/service_tests.cpp',
    'docs/repository-governance.md',
    'scripts/package-fullflow-release.ps1',
    'tests/fixtures/minimal.cap',
    'tests/fixtures/minimal.iqraw',
    'tests/fixtures/minimal.npy'
)

foreach ($path in $ignoredPaths) {
    Assert-Ignored -Path $path
}

foreach ($path in $reviewablePaths) {
    Assert-Reviewable -Path $path
}

Write-Output 'Git boundary verification passed.'
