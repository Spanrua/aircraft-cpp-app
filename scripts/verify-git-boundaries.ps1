[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRootOutput = & git -C $PSScriptRoot rev-parse --show-toplevel 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "Unable to determine the repository root from '$PSScriptRoot': $repoRootOutput"
}

$repoRoot = $repoRootOutput.Trim()

function Assert-Ignored {
    param([Parameter(Mandatory = $true)][string]$Path)

    & git -C $repoRoot check-ignore --no-index -q -- $Path
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

    & git -C $repoRoot check-ignore --no-index -q -- $Path
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 1) {
        return
    }

    if ($exitCode -eq 0) {
        throw "Expected '$Path' to be reviewable, but it is ignored."
    }

    throw "Unable to determine ignore status for '$Path' (git check-ignore exit code $exitCode)."
}

function Get-GitAttributeValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Attribute
    )

    $attributeOutput = & git -C $repoRoot check-attr $Attribute -- $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to read Git attribute '$Attribute' for '$Path' (git check-attr exit code $LASTEXITCODE): $attributeOutput"
    }

    $expectedPrefix = "${Path}: ${Attribute}: "
    $attributeLine = ($attributeOutput | Select-Object -Last 1).ToString()
    if (-not $attributeLine.StartsWith($expectedPrefix, [System.StringComparison]::Ordinal)) {
        throw "Unexpected git check-attr output for '$Path' attribute '$Attribute': $attributeLine"
    }

    return $attributeLine.Substring($expectedPrefix.Length).Trim()
}

function Assert-GitAttribute {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Attribute,
        [Parameter(Mandatory = $true)][string]$ExpectedValue
    )

    $actualValue = Get-GitAttributeValue -Path $Path -Attribute $Attribute
    if ($actualValue -ne $ExpectedValue) {
        throw "Expected Git attribute '$Attribute' for '$Path' to be '$ExpectedValue', but found '$actualValue'."
    }
}

function Assert-NoLfsFilter {
    param([Parameter(Mandatory = $true)][string]$Path)

    $filterValue = Get-GitAttributeValue -Path $Path -Attribute 'filter'
    if ($filterValue -eq 'lfs') {
        throw "Expected '$Path' not to use filter=lfs, but Git attributes set filter=lfs."
    }
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
    '$out',
    'scratch/sample.cap',
    'scratch/sample.iqraw',
    'scratch/sample.npy',
    'scratch/model.pth',
    'scratch/model.pt'
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

Assert-GitAttribute -Path 'scripts/verify-git-boundaries.ps1' -Attribute 'text' -ExpectedValue 'set'
Assert-GitAttribute -Path 'scripts/verify-git-boundaries.ps1' -Attribute 'eol' -ExpectedValue 'crlf'
Assert-GitAttribute -Path 'scratch/sample.cap' -Attribute 'binary' -ExpectedValue 'set'
Assert-NoLfsFilter -Path 'scratch/sample.cap'

Write-Output 'Git boundary verification passed.'
