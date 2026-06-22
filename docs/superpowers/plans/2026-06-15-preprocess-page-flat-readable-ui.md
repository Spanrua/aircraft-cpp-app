# Preprocess Page Flat Readable UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Qt6/C++ preprocess page genuinely flat and readable, with larger input controls and restored Chinese UI text.

**Architecture:** Keep the existing `PreprocessPage` behavior and service wiring intact. Replace top-level `QGroupBox` columns with flat `QFrame` workbench columns, use explicit object names for testable layout contracts, and constrain result height so parameter controls keep priority.

**Tech Stack:** Qt6 Widgets, C++17, Qt Test, existing `AppTheme` stylesheet.

---

### Task 1: Tighten UI Regression Tests

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] **Step 1: Write failing layout tests**

Add assertions that preprocess top-level columns are `QFrame`, not `QGroupBox`, and that visible path/parameter inputs expose readable minimum dimensions.

- [ ] **Step 2: Run the targeted tests**

Run from a VS developer shell:

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe preprocessPageUsesThreeColumnWorkbenchLayout preprocessPageUsesFlatPanelsForSummaryAndParameters preprocessPageKeepsParameterInputsReadable
```

Expected before implementation: at least one test fails because the page still uses `QGroupBox` workbench columns and path inputs are not wide enough.

### Task 2: Rebuild Preprocess Layout

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.cpp`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.h`

- [ ] **Step 1: Replace top-level group boxes**

Use `QFrame` columns named `PreprocessInputColumn`, `PreprocessParametersColumn`, and `PreprocessRunColumn`. Add visible title labels inside each column.

- [ ] **Step 2: Increase control readability**

Set path line edits to minimum height `42` and minimum width `520`; set numeric parameter edits to minimum height `42` and minimum width `180`.

- [ ] **Step 3: Restore Chinese strings**

Replace mojibake text in `PreprocessPage.cpp` with normal Chinese literals, preserving existing behavior.

### Task 3: Theme New Flat Panels

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/AppTheme.cpp`

- [ ] **Step 1: Style workbench frames**

Add stylesheet rules for `QFrame#PreprocessInputColumn`, `QFrame#PreprocessParametersColumn`, and `QFrame#PreprocessRunColumn`.

- [ ] **Step 2: Keep nested panels quieter**

Use lower-contrast borders and smaller radius for inner preprocess panels so the page reads as one flat workbench instead of nested cards.

### Task 4: Verify and Relaunch

**Files:**
- Build outputs under `D:/cqt_platform/aircraft_project/cpp_app/build-release`

- [ ] **Step 1: Build from VS environment**

```powershell
cmd /c "`"D:\VS\BuildTools2022\VC\Auxiliary\Build\vcvars64.bat`" && cmake --build `"D:\cqt_platform\aircraft_project\cpp_app\build-release`" --config Release"
```

- [ ] **Step 2: Run targeted UI tests**

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe preprocessPageUsesThreeColumnWorkbenchLayout preprocessPageUsesFlatPanelsForSummaryAndParameters preprocessPageKeepsParameterInputsReadable preprocessPageSeparatesParametersAndResultTabs
```

- [ ] **Step 3: Redeploy Qt runtime if needed and restart app**

```powershell
D:\qt\6.6.1\msvc2019_64\bin\windeployqt.exe D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
Start-Process D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
```
