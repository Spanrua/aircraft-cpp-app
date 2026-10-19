# Repository Governance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish `aircraft_project/cpp_app` as the only active Git repository, enforce precise source-versus-asset boundaries, and leave every existing asset in place until a separately approved migration.

**Architecture:** The C++ Qt application remains at its current path and receives repository-local policy, audit, and verification tooling. The outer workspace and the legacy Python repository are inspected only; runtime assets, build products, packages, models, and historical copies are ignored by the main repository and are documented for future copy-then-verify migration.

**Tech Stack:** Git, PowerShell 7+, CMake/CTest, Qt Test, C++17/Qt 5.12.10+.

---

## Scope and safety gates

This plan implements the governance foundations in phases A–C of the approved design. It does not execute asset migration or legacy repository retirement.

- Never delete, move, overwrite, reset, clean, or reinitialize `D:\cqt_platform`, `D:\cqt_platform\aircraft_project`, or any nested Git directory.
- Never run `git add -A`, `git clean`, `git reset`, or a bulk file move.
- Do not operate the 3943B, USRP, VISA, or UHD hardware. All verification is offline.
- Do not make the future `_assets` directory in this plan. A later task may create it only after an explicit migration approval and before copying assets.

## Planned file structure

| File | Responsibility |
|---|---|
| `scripts/audit-repository-governance.ps1` | Read-only snapshot of the three existing Git boundaries; writes a new JSON report only when the requested output path does not exist. |
| `scripts/verify-git-boundaries.ps1` | Verifies that generated artifacts are ignored and reviewable source paths are not ignored. |
| `.gitignore` | Keeps generated runtime, package, backup, model, and local editor artifacts out of the main repository. |
| `.gitattributes` | Normalizes text line endings and declares known binary artifact types. |
| `docs/repository-governance.md` | Main contributor-facing source/asset policy and future asset layout. |
| `README.md` and `README.zh-CN.md` | Link contributors to the governance policy and identify `cpp_app` as the development entry point. |
| `docs/superpowers/specs/2026-10-19-repository-governance-design.md` | Approved design; do not rewrite it while executing this plan. |

### Task 1: Create a non-destructive Git-boundary audit

**Files:**
- Create: `scripts/audit-repository-governance.ps1`
- Test output: `D:\cqt_platform\_workspace\reports\repository-governance-audit.json`

- [ ] **Step 1: Capture the pre-change repository state without writing to a repository**

Run:

```powershell
git -C D:\cqt_platform status --short
git -C D:\cqt_platform\aircraft_project status --short
git -C D:\cqt_platform\aircraft_project\cpp_app status --short
```

Expected: the workspace root reports that `master` has no commits, while `aircraft_project` and `cpp_app` report their existing statuses. Do not resolve or stage any listed file in this step.

- [ ] **Step 2: Add the failing audit-script smoke check**

Run before creating the script:

```powershell
pwsh -NoProfile -File .\scripts\audit-repository-governance.ps1 -ReportPath D:\cqt_platform\_workspace\reports\repository-governance-audit.json
```

Expected: FAIL because `scripts/audit-repository-governance.ps1` does not exist.

- [ ] **Step 3: Implement the audit script**

Create `scripts/audit-repository-governance.ps1` with this content:

```powershell
param(
    [Parameter(Mandatory)]
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'

function Get-GitLines {
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryPath,
        [Parameter(Mandatory)]
        [string[]]$Arguments
    )

    $output = & git -C $RepositoryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Git command failed for ${RepositoryPath}: git $($Arguments -join ' ')`n$($output -join [Environment]::NewLine)"
    }
    return @($output | ForEach-Object { $_.ToString() })
}

$cppAppRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$aircraftProjectRoot = (Resolve-Path (Join-Path $cppAppRoot '..')).Path
$workspaceRoot = (Resolve-Path (Join-Path $aircraftProjectRoot '..')).Path
$reportFile = [System.IO.FileInfo]::new($ReportPath)

if ($reportFile.Exists) {
    throw "Audit report already exists; refusing to overwrite it: $($reportFile.FullName)"
}

if (-not $reportFile.Directory.Exists) {
    [void][System.IO.Directory]::CreateDirectory($reportFile.Directory.FullName)
}

$repositories = @(
    [pscustomobject]@{ Name = 'workspace-root'; Path = $workspaceRoot },
    [pscustomobject]@{ Name = 'legacy-python'; Path = $aircraftProjectRoot },
    [pscustomobject]@{ Name = 'active-cpp-app'; Path = $cppAppRoot }
)

$snapshot = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    repositories = @(
        foreach ($repository in $repositories) {
            [ordered]@{
                name = $repository.Name
                path = $repository.Path
                branch = (Get-GitLines -RepositoryPath $repository.Path -Arguments @('branch', '--show-current'))[0]
                head = (Get-GitLines -RepositoryPath $repository.Path -Arguments @('rev-parse', '--verify', 'HEAD'))[0]
                status = Get-GitLines -RepositoryPath $repository.Path -Arguments @('status', '--porcelain=v1')
                ignored = Get-GitLines -RepositoryPath $repository.Path -Arguments @('status', '--ignored', '--porcelain=v1')
            }
        }
    )
}

$snapshot | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportFile.FullName -Encoding utf8NoBOM
Write-Output "Audit report created: $($reportFile.FullName)"
```

- [ ] **Step 4: Make the audit robust for the known no-commit root repository**

Replace the `head` property in the script with the following expression so the uninitialized root is reported rather than treated as an error:

```powershell
head = ((& git -C $repository.Path rev-parse --verify HEAD 2>$null) -join '')
if ([string]::IsNullOrWhiteSpace($head)) {
    $head = $null
}
```

Place those statements immediately before the `[ordered]@{ ... }` object in the `foreach ($repository in $repositories)` loop, and set its property to `head = $head`.

- [ ] **Step 5: Run the audit and validate the resulting JSON**

Run:

```powershell
pwsh -NoProfile -File .\scripts\audit-repository-governance.ps1 -ReportPath D:\cqt_platform\_workspace\reports\repository-governance-audit.json
Get-Content -Raw D:\cqt_platform\_workspace\reports\repository-governance-audit.json | ConvertFrom-Json | Select-Object -ExpandProperty repositories | Format-Table name,path,branch,head
```

Expected: three repository rows; `workspace-root` has a null `head`, and `active-cpp-app` resolves to the current C++ repository and commit. Running the script again with the same path must fail with the explicit “refusing to overwrite” message.

- [ ] **Step 6: Commit the audit tool only**

```powershell
git add scripts/audit-repository-governance.ps1
git diff --cached --check
git commit -m "chore: add repository governance audit"
```

Expected: exactly one tracked script in the commit; the JSON report remains outside the repository.

### Task 2: Encode and test the Git boundary rules

**Files:**
- Modify: `.gitignore`
- Create: `.gitattributes`
- Create: `scripts/verify-git-boundaries.ps1`

- [ ] **Step 1: Record the failing ignore-boundary checks**

Run before changing the rules:

```powershell
git check-ignore -q -- dist\package\aircraft_qt.exe
if ($LASTEXITCODE -eq 0) { throw 'dist should not be ignored before the rule change.' }
git check-ignore -q -- tests\fixtures\minimal.cap
if ($LASTEXITCODE -eq 0) { throw 'The fixture should remain reviewable.' }
```

Expected: the first command is not ignored under the current file; the second command is not ignored.

- [ ] **Step 2: Replace `.gitignore` with the reviewed rule set**

Replace the entire file with:

```gitignore
# CMake and Qt build trees
/build-*/
CMakeFiles/
CMakeCache.txt
cmake_install.cmake
CTestTestfile.cmake
DartConfiguration.tcl
*_autogen/
.qt/
.ninja_deps
.ninja_log
build.ninja

# Compiler and IDE artifacts
*.obj
*.lib
*.exe
*.dll
*.pdb
*.ilk
*.exp
*.user
__pycache__/
*.py[cod]
.pytest_cache/
.worktrees/
.superpowers/

# Local diagnostics and runtime state
/ui-test*.txt
/*.log
/$out
/captures/
/data/
/state/
*.cap
*.iqraw
*.npy
*.pth
*.pt
!tests/fixtures/**/*.cap
!tests/fixtures/**/*.iqraw
!tests/fixtures/**/*.npy

# Local packages, backups, and replay results
/dist/
/backups/
/fullflow-*/
/dji_training_backend_v5.zip
```

- [ ] **Step 3: Add line-ending and binary declarations**

Create `.gitattributes` with:

```gitattributes
* text=auto eol=lf
*.bat text eol=crlf
*.cmd text eol=crlf
*.ps1 text eol=crlf
*.png binary
*.jpg binary
*.jpeg binary
*.pdf binary
*.cap binary
*.iqraw binary
*.npy binary
*.pth binary
*.pt binary
*.zip binary
*.rar binary
```

- [ ] **Step 4: Implement a repeatable boundary verifier**

Create `scripts/verify-git-boundaries.ps1` with:

```powershell
$ErrorActionPreference = 'Stop'

function Assert-Ignored {
    param([Parameter(Mandatory)][string]$Path)
    & git check-ignore -q -- $Path
    if ($LASTEXITCODE -ne 0) {
        throw "Expected ignored path: $Path"
    }
}

function Assert-Reviewable {
    param([Parameter(Mandatory)][string]$Path)
    & git check-ignore -q -- $Path
    if ($LASTEXITCODE -eq 0) {
        throw "Expected reviewable path: $Path"
    }
    if ($LASTEXITCODE -ne 1) {
        throw "Unable to check path: $Path"
    }
}

@(
    'build-ninja/aircraft_qt.exe',
    'dist/release/aircraft_qt.exe',
    'backups/aircraft.zip',
    'fullflow-release-replay/report.json',
    'data/models/best_model.pth',
    'captures/example.iqraw',
    'state/app_state.json',
    'dji_training_backend_v5.zip',
    '$out'
) | ForEach-Object { Assert-Ignored $_ }

@(
    'src/app/ProjectPaths.cpp',
    'tests/service_tests.cpp',
    'docs/repository-governance.md',
    'scripts/package-fullflow-release.ps1',
    'tests/fixtures/minimal.cap',
    'tests/fixtures/minimal.iqraw',
    'tests/fixtures/minimal.npy'
) | ForEach-Object { Assert-Reviewable $_ }

Write-Output 'Git boundary verification passed.'
```

- [ ] **Step 5: Run the boundary checks and inspect only the changed classification**

Run:

```powershell
pwsh -NoProfile -File .\scripts\verify-git-boundaries.ps1
git status --short --ignored | Select-String 'dist|backups|fullflow|dji_training_backend_v5.zip|\$out'
git check-attr --all -- .gitattributes scripts\package-fullflow-release.ps1 tools\sample.cap
```

Expected: the verifier prints `Git boundary verification passed.`; package and backup paths show as ignored; `.ps1` reports CRLF text handling and `.cap` reports `binary`.

- [ ] **Step 6: Commit the policy rules and verifier**

```powershell
git add .gitignore .gitattributes scripts/verify-git-boundaries.ps1
git diff --cached --check
git commit -m "chore: define source and asset boundaries"
```

Expected: the commit contains only the two policy files and verifier, with no existing build output or package added.

### Task 3: Publish the contributor-facing repository policy

**Files:**
- Create: `docs/repository-governance.md`
- Modify: `README.md`
- Modify: `README.zh-CN.md`

- [ ] **Step 1: Add the policy document**

Create `docs/repository-governance.md` with the following content:

```markdown
# Repository Governance

`aircraft_project/cpp_app` is the only active source repository for the Aircraft RF desktop application. Run Git, CMake, and CTest commands from this directory.

## What belongs in Git

- Qt/C++ sources in `src/`, automated tests in `tests/`, and small anonymous fixtures in `tests/fixtures/`.
- Build definitions, dependency manifests, startup scripts, release scripts, and contributor documentation.
- Source code, tests, and fixed configuration in `dji_training_backend_v5/` that the desktop application invokes.

## What stays local

Generated build trees, runtime `data/`, captures, models, training runs, spectrograms, distribution packages, backups, log files, local Python environments, and hardware-specific settings stay out of Git.

The future workspace asset layout is `D:\cqt_platform\_assets\{captures,datasets,models,spectrograms,runs,releases,backups,archives,envs,logs,manuals}`. Do not create, move, or delete assets as part of ordinary source changes. Asset migration requires a separately approved copy, hash verification, runtime-path switch, and offline validation.

## Safe Git workflow

1. Review `git status --short` before staging.
2. Stage explicit paths; never use `git add -A` for this repository.
3. Run `pwsh -NoProfile -File .\scripts\verify-git-boundaries.ps1` when changing ignore rules or release behavior.
4. Build and test offline before submitting a change that affects CMake, runtime paths, or the release script.
5. Use `codex/<topic>` branch names for new work.

## Historical material

The parent Python prototype, `cpp_app_demo_sweep`, AutoDL copies, handover packages, and archives are reference material only. They are not development entry points and must not be removed or rewritten without explicit approval.
```

- [ ] **Step 2: Add a governance link to the English README**

Append this section to `README.md`:

```markdown
## Repository Governance

This directory is the active Git repository. Read [Repository Governance](docs/repository-governance.md) before staging files, changing release scripts, or working with runtime assets.
```

- [ ] **Step 3: Add a governance link to the Chinese README**

Append this section to `README.zh-CN.md`:

```markdown
## 仓库治理

本目录是唯一的正式开发仓库。提交文件、修改打包脚本或处理运行资产前，请先阅读[仓库治理说明](docs/repository-governance.md)。
```

- [ ] **Step 4: Verify documentation links and policy coverage**

Run:

```powershell
rg -n "Repository Governance|仓库治理|唯一.*开发仓库|only active source repository" README.md README.zh-CN.md docs\repository-governance.md
git diff --check -- README.md README.zh-CN.md docs/repository-governance.md
```

Expected: all three files contain the policy entry point and `git diff --check` prints no errors.

- [ ] **Step 5: Commit the contributor policy**

```powershell
git add README.md README.zh-CN.md docs/repository-governance.md
git diff --cached --check
git commit -m "docs: publish repository governance policy"
```

Expected: only contributor documentation and README links are committed.

### Task 4: Classify the existing C++ working tree without sweeping in user changes

**Files:**
- Modify only after human review: explicitly named files already reported by `git status`
- Do not stage: `dist/`, `backups/`, `fullflow-release-segment-fusion-offline-replay/`, `$out`, or archive/package outputs

- [ ] **Step 1: Generate a human-review staging manifest**

Run:

```powershell
git status --short | Set-Content -LiteralPath D:\cqt_platform\_workspace\reports\cpp-app-preexisting-status.txt -Encoding utf8NoBOM
Get-Content D:\cqt_platform\_workspace\reports\cpp-app-preexisting-status.txt
```

Expected: the report records the pre-existing modifications and untracked files without staging any of them.

- [ ] **Step 2: Review the product-source group before staging it**

Review exactly these paths as one coherent application change set:

```text
CMakeLists.txt
cpp_app.pro
src/app/ProjectPaths.cpp
src/app/ProjectPaths.h
src/core/Records.cpp
src/core/Records.h
src/main.cpp
src/services/AppStateStore.cpp
src/services/AppStateStore.h
src/services/CeyearIqReceiver.cpp
src/services/CeyearIqReceiver.h
src/services/CeyearPscSweep.cpp
src/services/CeyearPscSweep.h
src/services/CeyearVisaService.cpp
src/services/RecognitionProcessRunner.cpp
src/services/RecognitionProcessRunner.h
src/services/SignalCleaningProcessRunner.cpp
src/services/SignalCleaningProcessRunner.h
src/ui/AppTheme.cpp
src/ui/MainWindow.cpp
src/ui/MainWindow.h
src/ui/pages/CapturePage.cpp
src/ui/pages/CapturePage.h
src/ui/pages/DatasetPage.cpp
src/ui/pages/DatasetPage.h
src/ui/pages/PreprocessPage.cpp
src/ui/pages/PreprocessPage.h
src/ui/pages/RecognitionPage.cpp
src/ui/pages/RecognitionPage.h
src/ui/pages/SignalCleaningPage.cpp
src/ui/pages/SignalCleaningPage.h
src/ui/pages/SweepPage.cpp
src/ui/pages/SweepPage.h
src/ui/pages/TrainPage.cpp
src/ui/widgets/StepNavigator.cpp
start_aircraft_qt.cmd
```

Run:

```powershell
git diff --check -- CMakeLists.txt cpp_app.pro src start_aircraft_qt.cmd
git diff --stat -- CMakeLists.txt cpp_app.pro src start_aircraft_qt.cmd
```

Expected: all changes are explainable as one reviewed product feature. If any path is unrelated or unclear, leave it unstaged and stop this group.

- [ ] **Step 3: Review the training and test group separately**

Run:

```powershell
git diff --check -- dji_training_backend_v5\recognition_backend.py dji_training_backend_v5\tests\test_training_backend.py tests\service_tests.cpp tests\ui_tests.cpp
git diff --stat -- dji_training_backend_v5\recognition_backend.py dji_training_backend_v5\tests\test_training_backend.py tests\service_tests.cpp tests\ui_tests.cpp tests\test_analyze_sweep_candidates.py
```

Expected: the listed files are reviewed independently from UI/product files. Do not stage a training change simply because it shares a release date with a UI change.

- [ ] **Step 4: Classify untracked docs, scripts, and developer tools explicitly**

Review these possible repository inputs one file at a time before staging:

```text
AUTO_SAVE_README.md
PERFORMANCE_FIX_README.md
SWEEP_FIX_README.md
SWEEP_OPTIMIZATION_SUMMARY.md
WATERFALL_DISABLE_README.md
build_and_run_no_waterfall.bat
build_sweep_fix.bat
build_with_vs2022.bat
rebuild.bat
rebuild_sweep_fix.bat
run_aircraft_qt_qt6.bat
run_aircraft_qt_release.bat
run_with_diagnostics.bat
start_simple.bat
docs/3943b-capture-guide-zh.md
scripts/package-fullflow-release.ps1
tools/analyze_sweep_candidates.py
tools/optimize_sweep_classifier.py
tools/signal_noise_binary.pyw
tests/test_analyze_sweep_candidates.py
```

Use:

```powershell
git diff --no-index -- /dev/null <path>
```

Expected: each candidate has a documented purpose, no embedded local credential, and no dependency on an ignored build or model artifact. Keep unclear candidates out of Git.

- [ ] **Step 5: Stage only reviewed logical groups and verify their contents**

For each approved group, stage explicit paths and inspect the index before committing. For example, after review of a documentation-only group:

```powershell
git add docs/3943b-capture-guide-zh.md AUTO_SAVE_README.md
git diff --cached --name-status
git diff --cached --check
```

Expected: the index contains only the two explicitly reviewed files. If unexpected files appear, run `git restore --staged -- <unexpected-path>` and repeat the inspection; do not reset the working tree.

- [ ] **Step 6: Commit only complete, tested groups**

Use a message that reflects the reviewed group, for example:

```powershell
git commit -m "docs: add capture operation guide"
```

Expected: every commit has a narrow purpose. Files not belonging to that purpose stay unstaged for their eventual owner review.

### Task 5: Run offline integration verification from an isolated worktree

**Files:**
- No source files created or modified by this task
- Create only after confirming its path does not exist: `D:\cqt_platform\_workspace\verify-repository-governance`
- Build output: `D:\cqt_platform\_workspace\verify-repository-governance\build-ninja`

- [ ] **Step 1: Verify the target worktree and build paths are absent**

Run:

```powershell
Test-Path D:\cqt_platform\_workspace\verify-repository-governance
```

Expected: `False`. If it returns `True`, stop and choose a new, explicit path; do not remove the existing directory.

- [ ] **Step 2: Create an isolated worktree at the reviewed governance commit**

Run after the policy commits are present:

```powershell
git worktree add --detach D:\cqt_platform\_workspace\verify-repository-governance HEAD
```

Expected: a detached worktree is created without changing the main working tree or its uncommitted files.

- [ ] **Step 3: Configure an offline, hardware-disabled build**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake -S D:\cqt_platform\_workspace\verify-repository-governance -B D:\cqt_platform\_workspace\verify-repository-governance\build-ninja -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/5.12.10/msvc2017_64 -DAIRCRAFT_ENABLE_UHD=OFF -DAIRCRAFT_ENABLE_NI_VISA=OFF -DBUILD_TESTING=ON"
```

Expected: CMake configures without accessing hardware SDKs. A missing Qt kit is an environment blocker to document, not a reason to alter the project files.

- [ ] **Step 4: Build and run the automated tests**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build D:\cqt_platform\_workspace\verify-repository-governance\build-ninja"
ctest --test-dir D:\cqt_platform\_workspace\verify-repository-governance\build-ninja --output-on-failure
```

Expected: `aircraft_service_tests` and `aircraft_ui_tests` pass with the existing offscreen test environment. Do not launch hardware smoke executables.

- [ ] **Step 5: Re-run repository-boundary validation from the isolated clone**

Run:

```powershell
pwsh -NoProfile -File D:\cqt_platform\_workspace\verify-repository-governance\scripts\verify-git-boundaries.ps1
git -C D:\cqt_platform\_workspace\verify-repository-governance status --short
```

Expected: boundary verification passes and the detached verification worktree is clean after configuration and tests because its build tree is ignored.

- [ ] **Step 6: Preserve the verification worktree for review**

Record the commit ID, CMake output, CTest output, and boundary-verifier result in the task handoff. Do not remove the worktree automatically; deletion requires a later explicit confirmation because it is a recursive filesystem operation.

## Plan self-review

- **Design coverage:** Task 1 implements the read-only baseline; Tasks 2–3 implement source/asset policy; Task 4 protects existing user changes while creating commit boundaries; Task 5 validates a clean offline build. The separately approved asset-copy and legacy-retirement gates remain intentionally excluded.
- **Placeholder scan:** This plan contains no deferred steps, unspecified file paths, or implicit bulk staging.
- **Type consistency:** The same `cpp_app`, `_workspace`, `_assets`, audit report, and verifier paths are used consistently. `_assets` is never created or modified by this plan.
