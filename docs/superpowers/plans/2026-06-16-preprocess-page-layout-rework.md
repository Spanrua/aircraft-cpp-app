# Preprocess Page Layout Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the Qt Widgets preprocess page so it is readable and non-overlapping across desktop viewport sizes, with a clear task flow from CAP selection to parameter setup to run output and a page-level vertical scrollbar when content exceeds the visible height.

**Architecture:** Keep the existing C++/Qt service wiring unchanged. Fix the UI by making the workflow page content optionally scrollable, reducing global shell height, making the preprocess page use a two-row workbench with a stable left-to-right workflow, and moving low-priority results/log content behind bounded areas that cannot push controls into overlap.

**Tech Stack:** Qt6 Widgets, C++17, Qt Test, existing `WorkflowPage`, `PreprocessPage`, and `AppTheme` stylesheet.

---

## Screenshot Diagnosis

The current screenshot shows these concrete failures:

1. Top shell consumes too much vertical space before the task content starts.
2. The preprocess page still reads as nested panels, not as one work surface.
3. The center parameter area is vertically clipped; path fields and basic fields are partially hidden.
4. The right run panel overlaps: metrics, output summary, primary button, and log compete for the same vertical space.
5. The bottom result panel is visible too early and steals height before any result exists.
6. Text blocks and input fields use similar dark rectangles, so labels, containers, and editable fields are hard to distinguish.
7. The page has no page-level vertical scrollbar, so smaller heights force content to compress instead of scroll.

## Target Layout

At 1600x900 and 1920x1080 desktop sizes, the preprocess page should fit without overlap. At smaller heights, the preprocess content should scroll vertically with a right-side scrollbar while the shell stays fixed:

```text
Header shell: compact title/status row
Step nav: compact 5-step row

Preprocess page content area
  Title + one-line subtitle
  Main workbench:
    Left: CAP file + preview + CAP summary
    Center: output/model paths + basic parameters + advanced toggle
    Right: run status + primary actions + compact log
  Results:
    Collapsed empty state before running
    Bounded tab/table area after results exist
```

The scrollbar belongs to the page content area, not the whole application shell.

---

### Task 1: Add Scrollable Page Support

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/WorkflowPage.h`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/WorkflowPage.cpp`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.cpp`
- Test: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] **Step 1: Add a page-level scroll area test**

Add this test:

```cpp
void preprocessPageUsesPageLevelVerticalScrolling();
```

Implementation:

```cpp
void UiTests::preprocessPageUsesPageLevelVerticalScrolling()
{
    PreprocessPage page;
    page.resize(1280, 640);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *scrollArea = page.findChild<QScrollArea *>(QStringLiteral("WorkflowPageScrollArea"));
    QVERIFY(scrollArea != nullptr);
    QCOMPARE(scrollArea->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(scrollArea->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(scrollArea->widgetResizable());
}
```

- [ ] **Step 2: Wrap workflow content in QScrollArea**

Change `WorkflowPage` so the content layout can be hosted inside a `QScrollArea` when requested:

```cpp
explicit WorkflowPage(const QString &title,
                      const QString &subtitle,
                      QWidget *parent = nullptr,
                      bool scrollableContent = false);
```

Use this pattern:

```cpp
if (scrollableContent) {
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("WorkflowPageScrollArea"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_contentWidget = new QWidget(m_scrollArea);
    m_contentWidget->setObjectName(QStringLiteral("WorkflowPageScrollContent"));
    m_contentWidget->setLayout(m_contentLayout);
    m_scrollArea->setWidget(m_contentWidget);
    root->addWidget(m_scrollArea, 1);
} else {
    root->addLayout(m_contentLayout, 1);
}
```

- [ ] **Step 3: Enable scrolling for PreprocessPage**

Construct `PreprocessPage` with `scrollableContent = true` so the preprocess content scrolls independently from the shell.

- [ ] **Step 4: Run and confirm RED/GREEN**

Run the new test before and after the change:

```powershell
cmd /c ""D:\VS\BuildTools2022\VC\Auxiliary\Build\vcvars64.bat" && cmake --build "D:\cqt_platform\aircraft_project\cpp_app\build-release" --config Release --target aircraft_ui_tests && D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe preprocessPageUsesPageLevelVerticalScrolling -o D:\cqt_platform\aircraft_project\cpp_app\build-release\preprocess_scroll_area.txt,txt"
```

Expected before implementation: FAIL because the scroll area is missing. Expected after implementation: PASS.

---

### Task 2: Add Layout Regression Tests

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] **Step 1: Add a geometry overlap helper**

Add this helper near the test file's anonymous utilities:

```cpp
static bool verticallySeparated(const QWidget *top, const QWidget *bottom, int gap = 6)
{
    if (!top || !bottom) {
        return false;
    }
    const QRect topRect = top->geometry();
    const QRect bottomRect = bottom->geometry();
    return topRect.bottom() + gap <= bottomRect.top();
}
```

- [ ] **Step 2: Add a failing preprocess layout test**

Add a test named `preprocessPageAvoidsOverlappingControlsAtDesktopSize()`:

```cpp
void preprocessPageAvoidsOverlappingControlsAtDesktopSize();
```

Implementation:

```cpp
void UiTests::preprocessPageAvoidsOverlappingControlsAtDesktopSize()
{
    PreprocessPage page;
    page.resize(1600, 900);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *outputDirEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessOutputDirEdit"));
    auto *modelPathEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessModelPathEdit"));
    auto *sliceLengthEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessSliceLengthEdit"));
    auto *advancedToggle = page.findChild<QPushButton *>(QStringLiteral("PreprocessAdvancedParametersToggle"));
    auto *runButton = page.findChild<QPushButton *>(QStringLiteral("StartPreprocessButton"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("PreprocessLog"));
    auto *resultsPanel = page.findChild<QWidget *>(QStringLiteral("PreprocessResultsPanel"));

    QVERIFY(outputDirEdit != nullptr);
    QVERIFY(modelPathEdit != nullptr);
    QVERIFY(sliceLengthEdit != nullptr);
    QVERIFY(advancedToggle != nullptr);
    QVERIFY(runButton != nullptr);
    QVERIFY(logOutput != nullptr);
    QVERIFY(resultsPanel != nullptr);

    QVERIFY2(outputDirEdit->isVisible(), "output path input must be visible");
    QVERIFY2(modelPathEdit->isVisible(), "model path input must be visible");
    QVERIFY2(sliceLengthEdit->isVisible(), "basic parameter input must be visible");
    QVERIFY2(verticallySeparated(modelPathEdit, sliceLengthEdit), "path fields must not collide with basic parameters");
    QVERIFY2(verticallySeparated(runButton, logOutput), "run button must not overlap the log");
    QVERIFY2(resultsPanel->height() <= 180, "empty results panel must not steal workbench height");
}
```

- [ ] **Step 3: Run the test and confirm RED**

Run:

```powershell
cmd /c ""D:\VS\BuildTools2022\VC\Auxiliary\Build\vcvars64.bat" && cmake --build "D:\cqt_platform\aircraft_project\cpp_app\build-release" --config Release --target aircraft_ui_tests && D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe preprocessPageAvoidsOverlappingControlsAtDesktopSize -o D:\cqt_platform\aircraft_project\cpp_app\build-release\red_preprocess_no_overlap.txt,txt"
```

Expected before implementation: FAIL on either parameter separation, run/log separation, or results height.

---

### Task 3: Compress the Global Shell

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/MainWindow.cpp`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/AppTheme.cpp`
- Test: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] **Step 1: Reduce shell margins and status card sizes**

In `MainWindow::buildUi()`:

```cpp
root->setContentsMargins(18, 14, 18, 12);
root->setSpacing(10);
systemLayout->setContentsMargins(16, 10, 16, 10);
```

Limit the data path label:

```cpp
m_dataPathLabel->setMaximumWidth(360);
```

- [ ] **Step 2: Reduce top typography**

In `AppTheme.cpp`, set:

```css
QLabel#AppTitle {
    font-size: 20px;
    font-weight: 600;
}

QLabel#DataPathLabel {
    padding: 6px 9px;
}
```

- [ ] **Step 3: Verify main window still survives**

Run:

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe mainWindowStaysAliveAfterShowing -o D:\cqt_platform\aircraft_project\cpp_app\build-release\main_window_shell_compact.txt,txt
```

Expected: PASS.

---

### Task 4: Rebuild Preprocess Page Workbench

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.cpp`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.h`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/AppTheme.cpp`

- [ ] **Step 1: Put the main workbench in a fixed-priority row**

In `PreprocessPage::buildUi()`, set:

```cpp
workbench->setSpacing(12);
workbench->setContentsMargins(0, 0, 0, 0);
workbench->addWidget(inputColumn, 28);
workbench->addWidget(parametersColumn, 44);
workbench->addWidget(runColumn, 28);
contentLayout()->addLayout(workbench, 8);
```

- [ ] **Step 2: Make the input column narrower and summary compact**

Set:

```cpp
inputColumn->setMinimumWidth(360);
inputColumn->setMaximumWidth(430);
m_capPathEdit->setMinimumWidth(0);
m_capPathEdit->setMinimumHeight(40);
summaryBox->setMaximumHeight(140);
```

- [ ] **Step 3: Make the parameter column use two stacked groups**

Use a compact output group:

```cpp
pathGroup->setMinimumHeight(132);
pathGroup->setMaximumHeight(168);
pathLayout->setVerticalSpacing(8);
m_outputDirEdit->setMinimumWidth(0);
m_modelPathEdit->setMinimumWidth(0);
```

Use a compact basic group:

```cpp
basicGroup->setMinimumHeight(118);
basicGroup->setMaximumHeight(150);
m_sliceLengthEdit->setMinimumWidth(0);
m_energyThresholdEdit->setMinimumWidth(0);
m_aiConfidenceEdit->setMinimumWidth(0);
```

- [ ] **Step 4: Rebuild the run column order**

Run column order must be:

```text
title
small metric row
state label
primary button
secondary button
output summary
log
```

The run button must not sit between metric cards and log in a way that can overlap. Use a horizontal metric row:

```cpp
auto *metricRow = new QHBoxLayout();
metricRow->setSpacing(10);
metricRow->addWidget(m_segmentMetric);
metricRow->addWidget(m_sampleMetric);
runLayout->addLayout(metricRow);
```

Set:

```cpp
m_logOutput->setMinimumHeight(120);
m_logOutput->setMaximumHeight(170);
m_outputSummaryLabel->setMaximumHeight(58);
```

---

### Task 5: Collapse Empty Results

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/PreprocessPage.cpp`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] **Step 1: Make the initial result panel compact**

Initial state:

```cpp
resultsGroup->setMaximumHeight(150);
m_resultsTabs->setVisible(false);
```

When `showRunResult()` has rows:

```cpp
m_resultsTabs->setVisible(hasRows);
if (auto *panel = findChild<QWidget *>(QStringLiteral("PreprocessResultsPanel"))) {
    panel->setMaximumHeight(hasRows ? 260 : 150);
}
```

- [ ] **Step 2: Keep the empty state single-line**

Set:

```cpp
m_emptyState->setMaximumHeight(72);
m_emptyState->setWordWrap(true);
```

- [ ] **Step 3: Verify the no-overlap test**

Run:

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe preprocessPageAvoidsOverlappingControlsAtDesktopSize -o D:\cqt_platform\aircraft_project\cpp_app\build-release\green_preprocess_no_overlap.txt,txt
```

Expected: PASS.

---

### Task 6: Visual and Regression Verification

**Files:**
- Build outputs under `D:/cqt_platform/aircraft_project/cpp_app/build-release`

- [ ] **Step 1: Build all targets**

Run:

```powershell
cmd /c ""D:\VS\BuildTools2022\VC\Auxiliary\Build\vcvars64.bat" && cmake --build "D:\cqt_platform\aircraft_project\cpp_app\build-release" --config Release"
```

Expected: exit code `0`.

- [ ] **Step 2: Run full UI tests**

Run:

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe -o D:\cqt_platform\aircraft_project\cpp_app\build-release\all_ui_after_preprocess_layout_rework.txt,txt
```

Expected: all tests PASS.

- [ ] **Step 3: Restart the app for manual review**

Run:

```powershell
Get-Process | Where-Object { $_.ProcessName -eq 'aircraft_qt' } | Stop-Process -Force
D:\qt\6.6.1\msvc2019_64\bin\windeployqt.exe D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
Start-Process D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
```

Expected: the app starts, the preprocess page shows no overlap at the screenshot viewport, and the primary workflow is visually ordered.

## Acceptance Criteria

- No visible overlap in preprocess page at 1600x900 and the current 1920x1080 screenshot environment.
- At smaller heights, the preprocess page shows a right-side scrollbar instead of compressing controls.
- CAP input, output/model paths, and all basic parameters are visible without opening advanced settings.
- The run button and log area are vertically separated.
- Empty results area is compact and does not steal height from the workbench.
- Full UI test suite passes.
