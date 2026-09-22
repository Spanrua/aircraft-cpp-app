# 3943B PSC Sweep Location Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a new first-step sweep-location page that drives 3943B PSC multi-segment scanning, renders PSC waterfall data, suggests simple persistent peak candidates, and passes confirmed frequencies to capture.

**Architecture:** Add PSC-specific command/config types, a PSC packet parser, a peak candidate extractor, and a sweep session service in `src/services`. Add a Qt Widgets `SweepPage` before `CapturePage` in `MainWindow`; it uses a table + custom waterfall widget + candidate list, and hands confirmed center frequency to the existing capture page through a narrow setter.

**Tech Stack:** C++17, Qt 5.12.10+/Qt 6 Widgets, Qt Network, Qt Test, CMake/Ninja, NI-VISA, existing `aircraft_core` static library.

---

## File Structure

- Create `src/services/CeyearPscSweep.h/.cpp`: PSC config structs, command-line generation, parser, candidate extraction, and UDP session service.
- Create `src/ui/widgets/WaterfallWidget.h/.cpp`: lightweight `QWidget` painting PSC frames as a waterfall image.
- Create `src/ui/pages/SweepPage.h/.cpp`: first workflow page for PSC sweep setup, live chart, and candidate actions.
- Modify `src/services/CeyearVisaService.h/.cpp`: expose `startPscSweep()` and `stopPscSweep()` through `ICeyearVisaService`.
- Modify `src/ui/pages/CapturePage.h/.cpp`: add `setPresetCenterFrequencyHz(double)` to receive confirmed sweep frequency.
- Modify `src/ui/MainWindow.h/.cpp`: insert `SweepPage` before `CapturePage`, wire confirmed frequency to capture, update workflow labels/count.
- Modify `CMakeLists.txt`, `tests/service_tests.cpp`, and `tests/ui_tests.cpp`.

## Task 1: PSC Commands

**Files:**
- Create: `src/services/CeyearPscSweep.h`
- Create: `src/services/CeyearPscSweep.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing service tests**

Add slots:

```cpp
void ceyearPscSweepCommandsUseOnlyPscReportPath();
void ceyearPscSweepFormatsMultiSegmentConfig();
void ceyearPscSweepStopCommandsAbortAndClearUdp();
```

Add tests asserting:

```cpp
CeyearPscSweepConfig config;
config.pcReceiveIpAddress = QStringLiteral("192.168.1.168");
config.pcReceivePort = 19712;
config.segments = {
    {900000000.0, 1100000000.0, 25000.0},
    {1300000000.0, 1400000000.0, 12500.0},
};
const QStringList lines = ceyearPscSweepCommandLines(config);
QVERIFY(lines.contains(QStringLiteral("FREQ:MODE PSC")));
QVERIFY(lines.contains(QStringLiteral("SWE:MODE:SPE SENS")));
QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:TAG:ON \"192.168.1.168\",19712,PSC")));
QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:FLAG:ON \"192.168.1.168\",19712,\"VOLT:AC\",\"FREQ:RX\",\"FREQ:HIGH:RX\",\"OPT\"")));
QVERIFY(lines.contains(QStringLiteral("PSC:SEGM:CONF \"900.000000MHz,1100.000000MHz,25.000kHz;1300.000000MHz,1400.000000MHz,12.500kHz;\"")));
QVERIFY(lines.contains(QStringLiteral("INIT")));
for (const QString &line : lines) {
    QVERIFY(!line.contains(QStringLiteral("IFPAN"), Qt::CaseInsensitive));
    QVERIFY(!line.contains(QStringLiteral("TRAC:IQ"), Qt::CaseInsensitive));
    QVERIFY(!line.contains(QStringLiteral("SYST:IF:REM"), Qt::CaseInsensitive));
}
QCOMPARE(ceyearPscSweepStopCommandLines(),
         QStringList({QStringLiteral("ABORt"), QStringLiteral("*CLS"), QStringLiteral("TRAC:UDP:DEL ALL")}));
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```powershell
ctest --test-dir build-ninja -R aircraft_service_tests --output-on-failure
```

Expected: compile failure for missing `CeyearPscSweep.h` or missing functions.

- [ ] **Step 3: Implement minimal PSC config and command generation**

Create:

```cpp
struct CeyearPscSweepSegment { double startHz; double stopHz; double stepHz; };
struct CeyearPscSweepConfig {
    QString pcReceiveIpAddress = QStringLiteral("192.168.1.168");
    int pcReceivePort = 19712;
    QVector<CeyearPscSweepSegment> segments;
};
QStringList ceyearPscSweepCommandLines(const CeyearPscSweepConfig &config);
QStringList ceyearPscSweepStopCommandLines();
```

Format segment strings with `QLocale::c()` as MHz/kHz with six and three decimals respectively.

- [ ] **Step 4: Wire CMake and run GREEN**

Add `src/services/CeyearPscSweep.cpp` to `aircraft_core`. Run the same service test command. Expected: PASS for new command tests.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/services/CeyearPscSweep.* tests/service_tests.cpp
git commit -m "feat: add 3943B PSC sweep commands"
```

## Task 2: PSC Parser and Candidates

**Files:**
- Modify: `src/services/CeyearPscSweep.h/.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing parser and candidate tests**

Add slots:

```cpp
void ceyearPscParserExtractsFrequencyAndLevelBins();
void ceyearPscCandidateExtractorPromotesPersistentPeaks();
```

Build a representative big-endian PSC packet in test helper code:

```cpp
static QByteArray ceyearPscPacket(const QVector<qint16> &levels, quint64 lowHz, quint64 highHz)
```

Assert:

```cpp
const auto frames = CeyearPscPacketParser::parse(packet);
QCOMPARE(frames.size(), 1);
QCOMPARE(frames.first().bins.size(), 3);
QCOMPARE(frames.first().bins.at(0).frequencyHz, 900000000.0);
QCOMPARE(frames.first().bins.at(1).levelDbuv, -35.0);
```

Candidate test should feed three frames where one frequency stays above threshold and one is a one-frame spike; assert only the persistent frequency is returned.

- [ ] **Step 2: Run RED**

Run service tests. Expected: compile failure for missing parser/extractor types.

- [ ] **Step 3: Implement parser and extractor**

Add:

```cpp
struct CeyearPscBin { double frequencyHz; double levelDbuv; };
struct CeyearPscFrame { QVector<CeyearPscBin> bins; };
struct CeyearPscCandidate { double frequencyHz; double peakLevelDbuv; int hitCount; };
class CeyearPscPacketParser { public: static QVector<CeyearPscFrame> parse(const QByteArray &bytes); };
class CeyearPscCandidateExtractor { public: QVector<CeyearPscCandidate> update(const CeyearPscFrame &frame); };
```

Parser assumptions: big-endian common header, magic `0x000EB200`, PSC header value `1201`, level values are `qint16 / 10.0`, frequency range comes from `FREQ:RX` and `FREQ:HIGH:RX` flags when present. For malformed packets, return an empty vector.

Candidate defaults: threshold `-60.0 dBuV`, minimum hits `2`, bin match tolerance `stepHz / 2` when known or `25000 Hz` fallback.

- [ ] **Step 4: Run GREEN**

Run service tests. Expected: parser and candidate tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/services/CeyearPscSweep.* tests/service_tests.cpp
git commit -m "feat: parse PSC sweep frames"
```

## Task 3: Sweep Session Service

**Files:**
- Modify: `src/services/CeyearPscSweep.h/.cpp`
- Modify: `src/services/CeyearVisaService.h/.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing session/interface tests**

Add a service test proving `ICeyearVisaService` exposes PSC methods and that default implementation command wrappers produce writable commands:

```cpp
CeyearVisaService service;
Q_UNUSED(service)
const auto start = pscSweepStartQueries(config);
QVERIFY(std::all_of(start.cbegin(), start.cend(), [](const CeyearVisaQuery &q){ return !q.readOnly; }));
```

- [ ] **Step 2: Run RED**

Expected: missing declarations for PSC query wrappers and interface methods.

- [ ] **Step 3: Implement PSC service interfaces**

Add to `ICeyearVisaService`:

```cpp
virtual CeyearCaptureResult startPscSweep(const QString &resource, const CeyearPscSweepConfig &config) const = 0;
virtual CeyearCaptureResult stopPscSweep(const QString &resource) const = 0;
```

Implement in `CeyearVisaService` by opening VISA, writing `pscSweepStartQueries(config)` or stop queries, then querying `SYSTem:ERRor?`. Do not start IQ receiver or write output files.

- [ ] **Step 4: Run GREEN**

Run service tests. Expected: PASS.

- [ ] **Step 5: Commit**

```powershell
git add src/services/CeyearPscSweep.* src/services/CeyearVisaService.* tests/service_tests.cpp
git commit -m "feat: add PSC sweep service interface"
```

## Task 4: Waterfall Widget

**Files:**
- Create: `src/ui/widgets/WaterfallWidget.h`
- Create: `src/ui/widgets/WaterfallWidget.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui_tests.cpp`

- [ ] **Step 1: Write failing UI test**

Add slot:

```cpp
void waterfallWidgetKeepsStableFrameBuffer();
```

Test:

```cpp
WaterfallWidget widget;
widget.setMaximumRows(2);
widget.appendFrame({{900000000.0, -70.0}, {901000000.0, -35.0}});
widget.appendFrame({{900000000.0, -65.0}, {901000000.0, -32.0}});
widget.appendFrame({{900000000.0, -80.0}, {901000000.0, -31.0}});
QCOMPARE(widget.frameCount(), 2);
QVERIFY(widget.minimumSizeHint().width() >= 320);
```

- [ ] **Step 2: Run RED**

Run UI tests. Expected: compile failure for missing widget.

- [ ] **Step 3: Implement widget**

Implement a `QWidget` that stores `QVector<CeyearPscFrame>`, exposes `setMaximumRows(int)`, `appendFrame(const CeyearPscFrame &)`, `frameCount()`, and paints a nonblank waterfall using `QPainter`. Use stable minimum size around `480x280`.

- [ ] **Step 4: Run GREEN**

Run UI tests. Expected: PASS for widget test.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/ui/widgets/WaterfallWidget.* tests/ui_tests.cpp
git commit -m "feat: add PSC waterfall widget"
```

## Task 5: Sweep Page UI

**Files:**
- Create: `src/ui/pages/SweepPage.h`
- Create: `src/ui/pages/SweepPage.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui_tests.cpp`

- [ ] **Step 1: Write failing UI tests**

Add slots:

```cpp
void sweepPageExposesPscControlsAndWaterfall();
void sweepPageEmitsConfirmedCandidateFrequency();
```

Assert the page has `SweepSegmentTable`, `StartPscSweepButton`, `StopPscSweepButton`, `PscWaterfallWidget`, `PscCandidateTable`, and emits `frequencyConfirmed(double)` when a candidate is selected and the confirm button is clicked.

- [ ] **Step 2: Run RED**

Expected: missing `SweepPage`.

- [ ] **Step 3: Implement page shell**

Subclass `WorkflowPage` with Chinese title `扫频定位`. Build three zones exactly as in the design: segment table and controls, central `WaterfallWidget`, right candidate table/actions. Seed two default rows: `900-1100 MHz / 25 kHz` and `1300-1400 MHz / 12.5 kHz`. Add signals:

```cpp
signals:
    void frequencyConfirmed(double frequencyHz);
```

Use test-friendly object names from Step 1.

- [ ] **Step 4: Run GREEN**

Run UI tests. Expected: PASS for sweep page shell tests.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/ui/pages/SweepPage.* tests/ui_tests.cpp
git commit -m "feat: add sweep location page"
```

## Task 6: Main Workflow Integration

**Files:**
- Modify: `src/ui/MainWindow.h/.cpp`
- Modify: `src/ui/pages/CapturePage.h/.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] **Step 1: Write failing integration tests**

Add UI tests:

```cpp
void mainWindowStartsWithSweepLocationBeforeCapture();
void mainWindowPassesSweepFrequencyToCapturePage();
```

Assert first step button contains `扫频定位`, second contains `采集`, stack index 0 is `SweepPage`, and emitting a confirmed frequency updates `CapturePage` center frequency editor to that value.

- [ ] **Step 2: Run RED**

Expected: current `MainWindow` starts at capture and `CapturePage` has no setter.

- [ ] **Step 3: Implement capture setter**

Add:

```cpp
void CapturePage::setPresetCenterFrequencyHz(double frequencyHz)
{
    if (m_ceyearCenterFrequencyEdit) {
        m_ceyearCenterFrequencyEdit->setText(QLocale::c().toString(frequencyHz, 'f', 0));
    }
    if (m_deviceTypeCombo) {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    }
}
```

- [ ] **Step 4: Insert sweep page**

In `MainWindow`, keep pointers to `SweepPage` and `CapturePage`, workflow steps become `扫频定位, 采集, 预处理, 数据集, 训练, 识别`, and connect `SweepPage::frequencyConfirmed` to `CapturePage::setPresetCenterFrequencyHz` plus `updateCurrentStep(1)`.

- [ ] **Step 5: Run GREEN**

Run UI tests. Expected: PASS.

- [ ] **Step 6: Commit**

```powershell
git add src/ui/MainWindow.* src/ui/pages/CapturePage.* tests/ui_tests.cpp
git commit -m "feat: place sweep before capture workflow"
```

## Task 7: Live Sweep Wiring

**Files:**
- Modify: `src/ui/pages/SweepPage.h/.cpp`
- Modify: `src/services/CeyearPscSweep.h/.cpp`
- Test: `tests/ui_tests.cpp`, `tests/service_tests.cpp`

- [ ] **Step 1: Write failing behavior tests**

Add tests that clicking start with default segments builds `CeyearPscSweepConfig`, disables start, enables stop, and that feeding a parsed frame updates waterfall and candidate table.

- [ ] **Step 2: Run RED**

Expected: page shell does not yet manage sessions or candidate updates.

- [ ] **Step 3: Implement session ownership**

Let `SweepPage` own an `ICeyearVisaService` and a PSC UDP receiver/session object. Start validates table rows, starts UDP listening, sends PSC commands, appends parsed frames into `WaterfallWidget`, updates candidate table, and logs status. Stop sends PSC cleanup and re-enables start.

- [ ] **Step 4: Run GREEN**

Run both test suites. Expected: PASS.

- [ ] **Step 5: Commit**

```powershell
git add src/ui/pages/SweepPage.* src/services/CeyearPscSweep.* tests/ui_tests.cpp tests/service_tests.cpp
git commit -m "feat: wire live PSC sweep updates"
```

## Task 8: Full Verification

**Files:**
- Modify only if verification exposes issues in files touched above.

- [ ] **Step 1: Configure/build**

Run from `D:\cqt_platform\aircraft_project\cpp_app`:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S . -B build-ninja -G Ninja -DCMAKE_MAKE_PROGRAM=D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/5.12.10/msvc2017_64"
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build build-ninja"
```

- [ ] **Step 2: Run tests**

```powershell
ctest --test-dir build-ninja --output-on-failure
```

Expected: `aircraft_service_tests` and `aircraft_ui_tests` pass.

- [ ] **Step 3: Manual safety dry run**

Inspect generated PSC commands in tests/log output and confirm they contain `PSC` only and no `IF`, `IFPAN`, `TRAC:IQ`, or `SYST:IF:REMote:MODE SHORT`.

- [ ] **Step 4: Final commit**

```powershell
git status --short
git add CMakeLists.txt src/services/CeyearPscSweep.* src/services/CeyearVisaService.* src/ui/widgets/WaterfallWidget.* src/ui/pages/SweepPage.* src/ui/pages/CapturePage.* src/ui/MainWindow.* tests/service_tests.cpp tests/ui_tests.cpp
git commit -m "feat: add 3943B PSC sweep location workflow"
```

## Self-Review Notes

- Spec coverage: workflow order, PSC-only commands, waterfall UI, candidate extraction, no IQ output, stop cleanup, and tests are covered.
- Type consistency: PSC types live in `CeyearPscSweep.*`; UI uses `CeyearPscFrame` for waterfall and `CeyearPscCandidate` for the candidate list.
- Safety: implementation plan explicitly excludes IF/IQ commands from PSC workflow and requires stop cleanup tests.
