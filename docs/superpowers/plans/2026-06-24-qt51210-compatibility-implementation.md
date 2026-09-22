# Qt5.12.10 Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `cpp_app` build and run against Qt 5.12.10 as the minimum supported Qt version while keeping newer Qt 5 and later upgrade versions usable.

**Architecture:** Keep one codebase and one primary CMake flow. Replace Qt6-only build APIs with Qt-version-aware selection, add a small compatibility header for source-level guards, and update docs so the supported Qt/toolchain matrix is explicit for Windows 10+ and Ubuntu 20.04+ on both 32-bit and 64-bit targets.

**Tech Stack:** C++17, Qt Widgets, Qt Network, CMake, qmake `.pro` fallback, Qt 5.12.10+, Qt 6.x, MSVC, GCC/Clang.

---

### Task 1: Make CMake choose Qt5 or Qt6 cleanly

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the version-aware CMake logic**

```cmake
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_standard_project_setup()
endif()

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(aircraft_qt
        src/main.cpp
        src/ui/AppTheme.cpp
        src/ui/MainWindow.cpp
        src/ui/pages/WorkflowPage.cpp
        src/ui/pages/CapturePage.cpp
        src/ui/pages/DatasetPage.cpp
        src/ui/pages/PreprocessPage.cpp
        src/ui/pages/RecognitionPage.cpp
        src/ui/pages/TrainPage.cpp
        src/ui/widgets/IllustrationPanel.cpp
        src/ui/widgets/MetricCard.cpp
        src/ui/widgets/StatusBadge.cpp
        src/ui/widgets/StepNavigator.cpp
    )
else()
    add_executable(aircraft_qt
        src/main.cpp
        src/ui/AppTheme.cpp
        src/ui/MainWindow.cpp
        src/ui/pages/WorkflowPage.cpp
        src/ui/pages/CapturePage.cpp
        src/ui/pages/DatasetPage.cpp
        src/ui/pages/PreprocessPage.cpp
        src/ui/pages/RecognitionPage.cpp
        src/ui/pages/TrainPage.cpp
        src/ui/widgets/IllustrationPanel.cpp
        src/ui/widgets/MetricCard.cpp
        src/ui/widgets/StatusBadge.cpp
        src/ui/widgets/StepNavigator.cpp
    )
endif()

target_link_libraries(aircraft_qt PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
```

- [ ] **Step 2: Add Qt-version-aware test targets**

```cmake
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(aircraft_service_tests
        tests/service_tests.cpp
    )
else()
    add_executable(aircraft_service_tests
        tests/service_tests.cpp
    )
endif()

target_link_libraries(aircraft_service_tests PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
```

- [ ] **Step 3: Run a configure-only smoke check**

Run:
```powershell
cmake -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-qt5-smoke -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/msvc2017_64
```
Expected: configure succeeds with Qt 5.12.10 paths substituted for the local kit.

- [ ] **Step 4: Run a Qt6 configure check**

Run:
```powershell
cmake -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-qt6-smoke -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.6.1/msvc2019_64
```
Expected: configure succeeds without Qt6-only assumptions embedded in the build graph.

### Task 2: Add a narrow Qt compatibility layer and remove Qt6-only assumptions

**Files:**
- Create: `src/app/QtCompat.h`
- Modify: `src/main.cpp`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `src/ui/pages/CapturePage.cpp`
- Modify: `src/ui/pages/CapturePage.h`
- Modify: `src/services/CeyearVisaQuery.h`

- [ ] **Step 1: Add a compatibility header**

```cpp
#pragma once

#include <QtGlobal>

#define CPP_APP_QT_AT_LEAST(major, minor, patch) (QT_VERSION >= QT_VERSION_CHECK(major, minor, patch))
#define CPP_APP_QT_51210_OR_NEWER CPP_APP_QT_AT_LEAST(5, 12, 10)
#define CPP_APP_QT_6_OR_NEWER CPP_APP_QT_AT_LEAST(6, 0, 0)
```

- [ ] **Step 2: Replace any Qt6-only assumptions with version guards**

Use `CPP_APP_QT_6_OR_NEWER` only where a code path truly differs, and keep the Qt5.12.10 path as the default branch.

- [ ] **Step 3: Rebuild the app with the Qt5 kit**

Run:
```powershell
cmake --build D:\cqt_platform\aircraft_project\cpp_app\build-qt5-smoke
```
Expected: `aircraft_qt` and test targets compile without Qt6-only APIs.

- [ ] **Step 4: Run the existing unit tests**

Run:
```powershell
ctest --test-dir D:\cqt_platform\aircraft_project\cpp_app\build-qt5-smoke --output-on-failure
```
Expected: service and UI tests pass or produce a narrow, actionable Qt-version failure list.

### Task 3: Align qmake fallback and docs with the new minimum Qt version

**Files:**
- Modify: `cpp_app.pro`
- Modify: `README.md`
- Modify: `README.zh-CN.md`
- Modify: `requirements.md`

- [ ] **Step 1: Make the `.pro` file a valid Qt5 fallback**

```pro
QT += core gui widgets network
CONFIG += c++17
TEMPLATE = app
TARGET = aircraft_qt
```

Keep the qmake file free of Qt6-only syntax and avoid making it the primary build path.

- [ ] **Step 2: Update the English and Chinese README text**

State the supported matrix explicitly:
- Qt 5.12.10+ minimum
- Qt 6.x supported
- Windows 10+ and Ubuntu 20.04+
- 32-bit and 64-bit build targets

- [ ] **Step 3: Update requirements guidance**

Document the exact toolchain expectations for each platform, including the Qt kit, compiler family, and where the third-party UHD/VISA dependencies are expected to come from.

- [ ] **Step 4: Re-read the docs for consistency**

Check that the README, requirements, and build instructions all say the same minimum Qt version and do not mention Qt6 as the only supported path.

### Task 4: Verify the compatibility contract end to end

**Files:**
- Modify: `docs/superpowers/plans/2026-06-24-qt51210-compatibility-implementation.md` only if the plan needs tightening after implementation

- [ ] **Step 1: Re-run configure/build/tests after the code changes**

Run:
```powershell
cmake -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-qt5-final -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/msvc2017_64
cmake --build D:\cqt_platform\aircraft_project\cpp_app\build-qt5-final
ctest --test-dir D:\cqt_platform\aircraft_project\cpp_app\build-qt5-final --output-on-failure
```
Expected: the project configures, builds, and tests with the Qt 5.12.10 kit.

- [ ] **Step 2: Run the app once from the Qt5 bundle**

Launch the packaged executable from the offline bundle and confirm it reaches the main window without plugin or runtime dependency failures.

- [ ] **Step 3: Capture the final state**

Record the final build/test command outputs and leave the worktree with only intended source, build-script, and doc changes.
