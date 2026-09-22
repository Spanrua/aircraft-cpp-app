# 3943B Capture Page CAP Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Qt capture page run the verified 3943B trigger IQ capture path and convert the received `.iqraw` into a local `.cap` file.

**Architecture:** Keep the UI entry inside `CapturePage`, but call `ICeyearVisaService::triggerIqReport()` instead of path-only configuration. After raw bytes are received, convert the raw file with `CeyearIfIqPacketParser::convertRawFileToCap()` and present the `.cap` path in the capture results table.

**Tech Stack:** C++17, Qt Widgets, Qt Test, CMake/qmake compatible sources, NI-VISA runtime, existing 3943B services.

---

### Task 1: UI Behavior Test

**Files:**
- Modify: `tests/ui_tests.cpp`

- [ ] Change `capturePageConfigures3943BPathFromMainButton` to expect `triggerIqReportCount == 1`, `configureCapturePathCount == 0`, `.iqraw` raw output in the service config, and `.cap` in the capture table.
- [ ] Change `capturePagePasses3943BReportPathSeparatelyFromPcProtocol` and `capturePageKeeps3943BStopEnabledDuringBusyState` to wait for `triggerIqReportCount`.
- [ ] Run `aircraft_ui_tests.exe capturePageConfigures3943BPathFromMainButton -txt` and verify it fails before implementation.

### Task 2: CapturePage Implementation

**Files:**
- Modify: `src/ui/pages/CapturePage.h`
- Modify: `src/ui/pages/CapturePage.cpp`

- [ ] Include `services/CeyearIfIqPacketParser.h`.
- [ ] Add a helper that maps raw output path to same-stem `.cap`.
- [ ] Change the 3943B start button text to `开始 3943B IQ 采集`.
- [ ] Change `startCeyearCaptureAsync()` to call `triggerIqReport()`, then convert `.iqraw` to `.cap` on success.
- [ ] Store/display the `.cap` path while logging the raw path.

### Task 3: Build, Test, Deploy

**Files:**
- Deploy into: `dist/aircraft_qt_qt51210_portable`

- [ ] Build `aircraft_qt`, `aircraft_ui_tests`, and `aircraft_service_tests`.
- [ ] Run full `ctest --test-dir build-ninja --output-on-failure`.
- [ ] Copy the built `aircraft_qt.exe` into `dist/aircraft_qt_qt51210_portable`.
- [ ] Verify the portable executable exists and has a fresh timestamp.
