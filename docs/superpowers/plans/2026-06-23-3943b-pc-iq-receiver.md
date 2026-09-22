# 3943B PC IQ Receiver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C++/Qt PC-side LAN receiver that writes 3943B IQ data reported over TCP/UDP to the PC disk.

**Architecture:** Add a focused receiver service using Qt Network and QFile. The 3943B capture flow starts the local receiver before sending instrument capture commands, then reports the PC output path and received byte count in the UI.

**Tech Stack:** Qt6 Widgets, Qt6 Network, QFile, QTcpServer/QTcpSocket/QUdpSocket, CMake, Qt Test.

---

### Task 1: Receiver Service

**Files:**
- Create: `src/services/CeyearIqReceiver.h`
- Create: `src/services/CeyearIqReceiver.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/service_tests.cpp`

- [ ] Add `CeyearIqReceiverConfig`, `CeyearIqReceiverResult`, and `CeyearIqReceiver` with TCP and UDP modes.
- [ ] Write service tests that send local TCP and UDP payloads and verify the receiver writes exact bytes to a file.
- [ ] Link `Qt6::Network` where needed.

### Task 2: 3943B Capture Integration

**Files:**
- Modify: `src/services/CeyearVisaQuery.h`
- Modify: `src/services/CeyearVisaQuery.cpp`
- Modify: `src/services/CeyearVisaService.h`
- Modify: `src/services/CeyearVisaService.cpp`
- Test: `tests/service_tests.cpp`

- [ ] Extend `CeyearCaptureConfig` with local PC receive protocol, IP, port, timeout, and output path.
- [ ] Start the receiver before sending capture commands and stop it after the capture window.
- [ ] Keep instrument commands reversible and query `SYSTem:ERRor?` after writes.

### Task 3: Capture UI

**Files:**
- Modify: `src/ui/pages/CapturePage.h`
- Modify: `src/ui/pages/CapturePage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] Add 3943B-only controls for PC receive protocol, local IP, port, and PC output file.
- [ ] Display saved file path and received byte count in the capture results table/log.
- [ ] Keep USRP parameters separate from 3943B parameters.

### Task 4: Verification

**Commands:**
- `cmake --build .\build-ninja --target aircraft_service_tests aircraft_ui_tests aircraft_qt`
- `ctest --test-dir .\build-ninja --output-on-failure`

- [ ] Confirm offline tests pass.
- [ ] Do not run hardware data-report commands until the SCPI target path command is verified on the connected 3943B.
