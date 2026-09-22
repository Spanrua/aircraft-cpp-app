# 3943B Capture Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an offline-testable 3943B acquisition-control path that is separate from the USRP capture path.

**Architecture:** Extend the existing Ceyear VISA service with a conservative capture-control request/result model. Keep USRP `.cap` acquisition in `IUsrpDeviceService`; route 3943B UI actions through `ICeyearVisaService`, using SCPI commands confirmed from the 3943B manual and fake services for tests.

**Tech Stack:** Qt6 Widgets, C++17, NI-VISA dynamic loading, Qt Test, CMake/Ninja/MSVC.

---

### Task 1: Service Command Model

**Files:**
- Modify: `src/services/CeyearVisaQuery.h`
- Modify: `src/services/CeyearVisaQuery.cpp`
- Test: `tests/service_tests.cpp`

- [ ] Add `CeyearCaptureConfig` with center frequency, span, measure mode, measure time, IQ point count, record enable, record source, report path, and output path.
- [ ] Add `CeyearCaptureResult` with success, message, resource, items, captureName, sampleCount, and logs.
- [ ] Add `defaultCeyearCaptureCommands(config)` and `defaultCeyearCaptureStatusQueries()`.
- [ ] Test that capture commands include conservative writes and `INITiate[:IMMediate]`, without USRP fields.

### Task 2: VISA Capture Execution

**Files:**
- Modify: `src/services/CeyearVisaService.h`
- Modify: `src/services/CeyearVisaService.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Extend `ICeyearVisaService` with `startCapture(resource, config)` and `stopCapture(resource)`.
- [ ] Implement the real service by opening the same VISA resource, writing capture commands, then querying capture status and `SYSTem:ERRor?`.
- [ ] Keep all real write commands behind UI button clicks; tests use fake services only.

### Task 3: CapturePage 3943B UI Routing

**Files:**
- Modify: `src/ui/pages/CapturePage.h`
- Modify: `src/ui/pages/CapturePage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add 3943B-specific acquisition controls under the existing 3943B panel.
- [ ] When 3943B is selected, make the main capture button start 3943B capture instead of disabling it for USRP discovery state.
- [ ] Add a stop-record button for 3943B.
- [ ] Write capture results to the capture table with device `3943B`, not `USRP B210`.

### Task 4: Verification

**Files:**
- Build/test only.

- [ ] Build `aircraft_service_tests`, `aircraft_ui_tests`, and `aircraft_qt`.
- [ ] Run `ctest --test-dir .\build-ninja --output-on-failure` with `QT_QPA_PLATFORM=offscreen`.
- [ ] Do not claim real 3943B hardware validation until a device is connected.
