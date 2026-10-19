[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'

function Invoke-GitCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryPath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [int[]]$AllowedExitCodes = @(0)
    )

    $command = 'git ' + ($Arguments -join ' ')
    $output = @(& git -C $RepositoryPath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE

    if ($AllowedExitCodes -notcontains $exitCode) {
        $details = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        throw "Git command failed for repository '$RepositoryPath' while running '$command': $details"
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output   = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    }
}

function Get-RepositorySnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryPath,

        [switch]$AllowUnbornHead
    )

    $branch = (Invoke-GitCommand -RepositoryPath $RepositoryPath -Arguments @('branch', '--show-current')).Output
    $headResult = Invoke-GitCommand -RepositoryPath $RepositoryPath -Arguments @('rev-parse', '--verify', '--quiet', 'HEAD') -AllowedExitCodes @(0, 1)

    if ($headResult.ExitCode -eq 1 -and -not $AllowUnbornHead) {
        throw "Git command failed for repository '$RepositoryPath' while running 'git rev-parse --verify --quiet HEAD': HEAD is not available."
    }

    [pscustomobject]@{
        name    = $Name
        path    = $RepositoryPath
        branch  = $branch
        head    = if ($headResult.ExitCode -eq 0) { $headResult.Output } else { $null }
        status  = (Invoke-GitCommand -RepositoryPath $RepositoryPath -Arguments @('status', '--porcelain=v1')).Output
        ignored = (Invoke-GitCommand -RepositoryPath $RepositoryPath -Arguments @('status', '--ignored', '--porcelain=v1')).Output
    }
}

$scriptDirectory = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$cppAppRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDirectory '..')).Path
$cppAppParent = (Resolve-Path -LiteralPath (Join-Path $cppAppRoot '..')).Path

if ([IO.Path]::GetFileName($cppAppParent) -eq '.worktrees') {
    $primaryCppAppRoot = (Resolve-Path -LiteralPath (Join-Path $cppAppParent '..')).Path
    $aircraftProjectRoot = (Resolve-Path -LiteralPath (Join-Path $primaryCppAppRoot '..')).Path
}
else {
    $aircraftProjectRoot = $cppAppParent
}

$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $aircraftProjectRoot '..')).Path
$fullReportPath = [IO.Path]::GetFullPath($ReportPath)

if (Test-Path -LiteralPath $fullReportPath) {
    throw "refusing to overwrite existing report '$fullReportPath'."
}

$reportDirectory = [IO.Path]::GetDirectoryName($fullReportPath)
if (-not [string]::IsNullOrWhiteSpace($reportDirectory) -and -not (Test-Path -LiteralPath $reportDirectory)) {
    [IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
}

$report = [ordered]@{
    generatedAt  = [DateTime]::UtcNow.ToString('o')
    repositories = @(
        Get-RepositorySnapshot -Name 'workspace-root' -RepositoryPath $workspaceRoot -AllowUnbornHead
        Get-RepositorySnapshot -Name 'legacy-python' -RepositoryPath $aircraftProjectRoot
        Get-RepositorySnapshot -Name 'active-cpp-app' -RepositoryPath $cppAppRoot
    )
}

$json = $report | ConvertTo-Json -Depth 4
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

try {
    $stream = New-Object IO.FileStream($fullReportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
}
catch [IO.IOException] {
    throw "refusing to overwrite existing report '$fullReportPath'."
}

try {
    $writer = New-Object IO.StreamWriter($stream, $utf8NoBom)
    try {
        $writer.Write($json)
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $stream.Dispose()
}
