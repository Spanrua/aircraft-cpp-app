# Frontend Operability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current end-to-end RF workflow into a safer, faster and more traceable desktop operation surface.

**Architecture:** Reuse the existing Qt Widgets dark Fusion theme. Add narrowly scoped status, provenance and keyboard affordances to the existing workflow pages rather than adding a second navigation layer. Hardware control remains 3943B-only in the visible UI; no physical device command is issued by UI-only changes.

**Tech Stack:** C++17, Qt Widgets 5.12, Qt Test, CTest.

---

### Task 1: Persistent 3943B readiness

**Files:**
- Modify: `src/ui/pages/CapturePage.h`, `src/ui/pages/CapturePage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add a failing `capturePageShowsPersistent3943BReadiness` UI test that requires a visible readiness label, current VISA resource and last-query state.
- [ ] Run `aircraft_ui_tests.exe capturePageShowsPersistent3943BReadiness`; expect failure because the status widget does not exist.
- [ ] Add a labelled status line updated by `finalizeCeyearStatus`, and retain the safe disabled/busy behavior for capture.
- [ ] Re-run the focused test; expect pass.

### Task 2: Workflow-level operational feedback

**Files:**
- Modify: `src/ui/MainWindow.cpp`, `src/ui/AppTheme.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add a failing test for a workflow banner that states the current step, available next action and blocked reason in text.
- [ ] Implement text-first status presentation in `updateWorkflowStatusPresentation` and semantic styles for idle/running/warning/success.
- [ ] Re-run the focused test; expect pass.

### Task 3: Recognition provenance and review handoff

**Files:**
- Modify: `src/ui/pages/RecognitionPage.h`, `src/ui/pages/RecognitionPage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add a failing test requiring model/dataset provenance and an explicit low-confidence review guidance line in the result area.
- [ ] Implement a non-destructive provenance label updated when model/sample/result selection changes.
- [ ] Re-run the focused test; expect pass.

### Task 4: Dataset review throughput

**Files:**
- Modify: `src/ui/pages/DatasetPage.h`, `src/ui/pages/DatasetPage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add a failing test for a review queue summary and keyboard shortcuts that mark the selected candidate approved/rejected/pending.
- [ ] Add `QShortcut` bindings and a text-visible queue counter without changing stored review semantics.
- [ ] Re-run the focused test; expect pass.

### Task 5: Density, accessibility and diagnostics

**Files:**
- Modify: `src/ui/AppTheme.cpp`, `src/ui/pages/CapturePage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add a failing test that requires a visible copy-diagnostics action on the capture page.
- [ ] Raise primary body text to the desktop baseline, add visible focus styling, and implement copy-to-clipboard diagnostics without issuing device commands.
- [ ] Re-run the focused test; expect pass.

### Task 6: Verification

- [ ] Build with `cmake --build build-batch-tests --clean-first`.
- [ ] Run `ctest --test-dir build-batch-tests --output-on-failure` with `QT_QPA_PLATFORM=offscreen`.
- [ ] Run `git diff --check` and report only relevant files.
