# Repository Governance

## Active Source Repository

`aircraft_project/cpp_app` is the only active source repository for this application. Run Git, CMake, and CTest commands from this directory. The parent Python prototype may be consulted for reference, but it is not a development entry point.

## What Belongs in Git

Keep the following application sources and configuration in Git:

- `src/` and `tests/`, including small anonymous test fixtures;
- build definitions and dependencies;
- startup and release scripts;
- documentation; and
- source code, tests, and fixed configuration in `dji_training_backend_v5` that the application calls.

## What Stays Local

Keep the following local rather than treating them as ordinary source changes:

- build trees and runtime data;
- captures, models, training runs, and spectrograms;
- distribution packages, backups, and logs;
- Python environments; and
- hardware-specific settings.

The future external asset layout is:

```text
D:\cqt_platform\_assets\{captures,datasets,models,spectrograms,runs,releases,backups,archives,envs,logs,manuals}
```

Ordinary code changes must not create, move, or delete assets. Any asset migration requires separate authorization and must follow this sequence: copy, hash, path switch, and offline validation.

## Safe Git Workflow

1. Start by reviewing `git status`.
2. Stage files explicitly; do not use `git add -A`.
3. Run `pwsh -NoProfile -File .\scripts\verify-git-boundaries.ps1` when modifying ignore rules or release behavior.
4. Run offline tests when the change affects CMake, runtime behavior, or release workflows.
5. Work on a `codex/<topic>` branch.

## Reference-Only Material

The historical parent Python prototype, `cpp_app_demo_sweep`, AutoDL copies, handoff packages, and archives are reference-only. They are not development entry points and must not be deleted or rewritten without explicit authorization.
