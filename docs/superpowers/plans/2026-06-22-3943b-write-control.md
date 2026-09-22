# 3943B Write Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add conservative 3943B write controls for center frequency, span, demodulation, and detector, then read back status and the error queue.

**Architecture:** Extend the existing VISA service interface with a typed control config and apply method. Reuse the current LAN/VISA resource field, status table, async worker pattern, and query result rendering in `CapturePage`.

**Tech Stack:** Qt6 Widgets, C++17, VISA socket resource `TCPIP::192.168.1.54::5025::SOCKET`, Qt Test.

---

### Task 1: Service Command Coverage

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/services/CeyearVisaQuery.h`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/services/CeyearVisaQuery.cpp`
- Test: `D:/cqt_platform/aircraft_project/cpp_app/tests/service_tests.cpp`

- [ ] Add `CeyearControlConfig` with center frequency Hz, span Hz, demodulation, and detector.
- [ ] Add `defaultCeyearControlCommands(config)` returning four write commands: `FREQuency`, `FREQuency:SPAN`, `DEModulation`, and `DETector:FUNCtion`.
- [ ] Add a service test proving the commands are writes, contain no bracket notation, and cover only the approved controls.
- [ ] Run `aircraft_service_tests.exe ceyearControlCommandsCoverApprovedWrites -txt` and observe the expected failure before implementation, then pass after implementation.

### Task 2: VISA Apply Method

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/services/CeyearVisaService.h`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/services/CeyearVisaService.cpp`
- Test: `D:/cqt_platform/aircraft_project/cpp_app/tests/service_tests.cpp`

- [ ] Add virtual `applyControl(resource, config)` to `ICeyearVisaService`.
- [ ] Implement `CeyearVisaService::applyControl` by opening the same VISA session path as query, sending four write commands, then reading back `defaultCeyearStatusQueries()`.
- [ ] Mark success only when writes and readback succeed and `SYSTem:ERRor?` is empty/`00`/no-error style.
- [ ] Avoid `*RST`, LAN writes, gateway writes, and measure mode writes.

### Task 3: Capture Page UI

**Files:**
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/CapturePage.h`
- Modify: `D:/cqt_platform/aircraft_project/cpp_app/src/ui/pages/CapturePage.cpp`
- Test: `D:/cqt_platform/aircraft_project/cpp_app/tests/ui_tests.cpp`

- [ ] Add editable controls under the 3943B panel: center frequency Hz, frequency span Hz, demodulation, detector.
- [ ] Add `ApplyCeyearControlButton` and connect it to `applyCeyearControlAsync()`.
- [ ] Disable the button while busy or when USRP is selected.
- [ ] Validate frequency/span as positive numbers and demodulation/detector as non-empty strings.
- [ ] On completion, update the existing 3943B status table and log the returned message.

### Task 4: Verification

**Files:**
- Build/test only.

- [ ] Build `aircraft_qt`, `aircraft_service_tests`, `aircraft_ui_tests`, and `ceyear_status_query`.
- [ ] Run targeted UI test `capturePageApplies3943BControlAndRefreshesStatus`.
- [ ] Run all service tests.
- [ ] Run all UI tests.
- [ ] Optionally run live read-only query `ceyear_status_query.exe TCPIP::192.168.1.54::5025::SOCKET`. Do not run live write without explicit values.
