# 3943B IF/IQ Safe Bring-Up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a safe, evidence-driven C++/Qt workflow for 3943B-S LAN IF/IQ bring-up before continuing `.cap` file output.

**Architecture:** Split the work into three explicit lanes: read-only instrument discovery, low-risk path configuration, and gated stream initiation. The current receiver and `.cap` path stay out of scope until the instrument is proven to emit non-zero IF/IQ bytes.

**Tech Stack:** C++17, Qt Core/Network/Test, NI-VISA SCPI over LAN, 3943B control resource `TCPIP::192.168.50.54::5025::SOCKET`, PC receiver endpoint `192.168.50.20:5030`.

---

## Current Evidence

- Control plane is healthy: `*IDN?` returns `Ceyear Technologies,3943B,,V2.1.0`, and `SYSTem:ERRor?` normally returns `00`.
- PC and 3943B are on an isolated subnet: PC `192.168.50.20/24`, 3943B `192.168.50.54`.
- The previous live sequence with `SYSTem:IF:REMote:MODE SHORT`, UDP `TAG/FLAG`, `TRACe:IQ:DATA:REPort FREE`, and `INITiate` produced zero bytes and can destabilize the instrument. Do not repeat it as a default smoke path.
- The manual-supported network path remains: `TRACe:PATH`, `TRACe:UDP|TCP:TAG`, `TRACe:UDP|TCP:FLAG`, `TRACe:IQ:DATA:REPort`, `SYSTem:IF:REMote:MODE`, and IF tag value `901`.

## File Map

- Modify: `src/services/CeyearVisaQuery.h`
  - Add read-only probe query groups and an enum/classification for capture command risk.
- Modify: `src/services/CeyearVisaQuery.cpp`
  - Generate query-only IF/IQ discovery commands.
  - Separate low-risk path setup commands from high-risk stream start commands.
- Modify: `src/services/CeyearVisaService.h`
  - Add a read-only probe API and a path-configuration API, without starting acquisition.
- Modify: `src/services/CeyearVisaService.cpp`
  - Execute probe query sets and return all responses, including unsupported-command evidence.
  - Ensure high-risk commands are not used by `startCapture()` unless explicitly allowed.
- Modify: `src/tools/ceyear_status_query.cpp`
  - Add a `--probe-if-iq` mode that sends query-only commands.
  - Keep arbitrary writes unavailable unless a separate execute/write mode is added later.
- Modify: `src/tools/ceyear_capture_smoke.cpp`
  - Rename behavior toward `--configure-path-only` and keep `--execute --initiate` gated.
  - Make `FREE` report mode gated the same way as `INITiate`.
- Modify: `tests/service_tests.cpp`
  - Add unit tests for probe queries, risk classification, and command ordering.
- Create: `docs/3943b-if-iq-bringup-checklist.md`
  - Human-readable field checklist for front-panel mode, option/license checks, and safe test order.

---

### Task 1: Add IF/IQ Read-Only Probe Query Set

**Files:**
- Modify: `src/services/CeyearVisaQuery.h`
- Modify: `src/services/CeyearVisaQuery.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing test for probe query coverage**

Add this private slot declaration in `ServiceTests`:

```cpp
void ceyearIfIqProbeQueriesAreReadOnlyAndCoverManualState();
```

Add this test body:

```cpp
void ServiceTests::ceyearIfIqProbeQueriesAreReadOnlyAndCoverManualState()
{
    const auto queries = defaultCeyearIfIqProbeQueries();
    QVERIFY(queries.size() >= std::size_t{12});

    const auto hasCommand = [&](const char *command) {
        return std::any_of(queries.cbegin(), queries.cend(), [&](const CeyearVisaQuery &query) {
            return std::strcmp(query.command.constData(), command) == 0;
        });
    };

    for (const auto &query : queries) {
        QVERIFY2(query.readOnly, query.label.constData());
        QVERIFY2(query.command.endsWith('?'), query.command.constData());
    }

    QVERIFY(hasCommand("*IDN?"));
    QVERIFY(hasCommand("SYSTem:ERRor?"));
    QVERIFY(hasCommand("FREQuency:MODE?"));
    QVERIFY(hasCommand("FREQuency?"));
    QVERIFY(hasCommand("FREQuency:SPAN?"));
    QVERIFY(hasCommand("MEASure:MODE?"));
    QVERIFY(hasCommand("SYSTem:IF:REMote:MODE?"));
    QVERIFY(hasCommand("TRACe:IQ:DATA:NUMBer?"));
    QVERIFY(hasCommand("TRACe:IQ:DATA:REPort?"));
    QVERIFY(hasCommand("TRACe:RECord:SELectivity?"));
    QVERIFY(hasCommand("TRACe:PATH?"));
    QVERIFY(hasCommand("TRACe:UDP?"));
    QVERIFY(hasCommand("TRACe:TCP?"));
}
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```powershell
ctest --test-dir build-ninja --output-on-failure -R aircraft_service_tests
```

Expected: build or test fails because `defaultCeyearIfIqProbeQueries()` is not declared.

- [ ] **Step 3: Add declaration**

Add to `src/services/CeyearVisaQuery.h`:

```cpp
std::vector<CeyearVisaQuery> defaultCeyearIfIqProbeQueries();
```

- [ ] **Step 4: Add implementation**

Add to `src/services/CeyearVisaQuery.cpp`:

```cpp
std::vector<CeyearVisaQuery> defaultCeyearIfIqProbeQueries()
{
    return {
        {"Identity", "*IDN?"},
        {"System error before probe", "SYSTem:ERRor?"},
        {"Frequency mode", "FREQuency:MODE?"},
        {"Center frequency", "FREQuency?"},
        {"Frequency span", "FREQuency:SPAN?"},
        {"Measure mode", "MEASure:MODE?"},
        {"Measure time", "MEASure:TIME?"},
        {"IF remote mode", "SYSTem:IF:REMote:MODE?"},
        {"IQ point count", "TRACe:IQ:DATA:NUMBer?"},
        {"IQ report mode", "TRACe:IQ:DATA:REPort?"},
        {"Record source", "TRACe:RECord:SOURce?"},
        {"Record selectivity", "TRACe:RECord:SELectivity?"},
        {"Record state", "TRACe:RECord:STATe?"},
        {"Report path", "TRACe:PATH?"},
        {"UDP report targets", "TRACe:UDP?"},
        {"TCP report targets", "TRACe:TCP?"},
        {"System error after probe", "SYSTem:ERRor?"},
    };
}
```

- [ ] **Step 5: Run test and verify it passes**

Run:

```powershell
cmake --build build-ninja --target aircraft_service_tests
ctest --test-dir build-ninja --output-on-failure -R aircraft_service_tests
```

Expected: `aircraft_service_tests` passes.

---

### Task 2: Classify Capture Commands by Risk

**Files:**
- Modify: `src/services/CeyearVisaQuery.h`
- Modify: `src/services/CeyearVisaQuery.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing test for risk classification**

Add this private slot declaration:

```cpp
void ceyearCaptureRiskClassificationBlocksKnownHazards();
```

Add this test body:

```cpp
void ServiceTests::ceyearCaptureRiskClassificationBlocksKnownHazards()
{
    CeyearCaptureConfig config;
    config.pcReceiveProtocol = QStringLiteral("UDP");
    config.reportPath = QStringLiteral("UDP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    config.pcReceivePort = 5030;

    const QStringList lowRisk = ceyearLowRiskPathSetupCommandLines(config);
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.50.20\",5030,IF")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.50.20\",5030,\"OPTIONAL_HEADER\"")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:PATH UDP")));
    QVERIFY(!lowRisk.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!lowRisk.contains(QStringLiteral("INITiate")));
    QVERIFY(!lowRisk.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));

    const QStringList risky = ceyearRiskyStreamStartCommandLines(config);
    QVERIFY(risky.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(risky.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!risky.contains(QStringLiteral("INITiate")));

    config.sendInitiate = true;
    const QStringList riskyWithInitiate = ceyearRiskyStreamStartCommandLines(config);
    QVERIFY(riskyWithInitiate.contains(QStringLiteral("INITiate")));
}
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```powershell
cmake --build build-ninja --target aircraft_service_tests
ctest --test-dir build-ninja --output-on-failure -R aircraft_service_tests
```

Expected: compile failure because command-line helpers do not exist.

- [ ] **Step 3: Add declarations**

Add to `src/services/CeyearVisaQuery.h`:

```cpp
QStringList ceyearLowRiskPathSetupCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearRiskyStreamStartCommandLines(const CeyearCaptureConfig &config);
bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun);
```

- [ ] **Step 4: Implement low-risk and risky command split**

Add to `src/services/CeyearVisaQuery.cpp`:

```cpp
QStringList ceyearLowRiskPathSetupCommandLines(const CeyearCaptureConfig &config)
{
    const QString protocol = config.pcReceiveProtocol.trimmed().toUpper();
    const QString tracePath = config.reportPath.trimmed().toUpper();
    return {
        QStringLiteral("TRACe:%1:TAG \"%2\",%3,IF")
            .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)),
        QStringLiteral("TRACe:%1:FLAG \"%2\",%3,\"OPTIONAL_HEADER\"")
            .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)),
        QStringLiteral("TRACe:PATH %1").arg(tracePath),
    };
}

QStringList ceyearRiskyStreamStartCommandLines(const CeyearCaptureConfig &config)
{
    QStringList lines{
        QStringLiteral("SYSTem:IF:REMote:MODE SHORT"),
        QStringLiteral("TRACe:IQ:DATA:NUMBer %1").arg(config.iqPointCount),
        QStringLiteral("TRACe:IQ:DATA:REPort FREE"),
    };
    if (config.sendInitiate) {
        lines.append(QStringLiteral("INITiate"));
    }
    return lines;
}

bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun)
{
    Q_UNUSED(config)
    return execute && !allowRiskyFreeRun;
}
```

- [ ] **Step 5: Run tests**

Run:

```powershell
cmake --build build-ninja --target aircraft_service_tests
ctest --test-dir build-ninja --output-on-failure -R aircraft_service_tests
```

Expected: `aircraft_service_tests` passes.

---

### Task 3: Add Probe Mode to `ceyear_status_query`

**Files:**
- Modify: `src/tools/ceyear_status_query.cpp`
- Test: manual CLI dry run through status query output on a connected device

- [ ] **Step 1: Add argument parsing for `--probe-if-iq`**

In `main`, compute:

```cpp
const bool probeIfIq = std::any_of(commands.begin(), commands.end(), [](const std::string &arg) {
    return arg == "--probe-if-iq";
});
if (probeIfIq) {
    commands.clear();
}
```

- [ ] **Step 2: Select probe queries when requested**

Replace the default query loop source:

```cpp
const auto querySet = probeIfIq ? defaultCeyearIfIqProbeQueries() : defaultCeyearStatusQueries();
for (const auto &query : querySet) {
    std::string response;
    status = queryCommand(device, query.command, &response);
    std::cout << query.label.constData() << " [" << query.command.constData() << "]: ";
    if (status < VI_SUCCESS) {
        std::cout << "ERROR " << visaStatusText(status) << std::endl;
        continue;
    }
    std::cout << response << std::endl;
}
```

- [ ] **Step 3: Build the tool**

Run:

```powershell
cmake --build build-ninja --target ceyear_status_query
```

Expected: target builds successfully.

- [ ] **Step 4: Run read-only probe against instrument**

Run:

```powershell
.\build-ninja\ceyear_status_query.exe TCPIP::192.168.50.54::5025::SOCKET --probe-if-iq
```

Expected:
- `*IDN?` returns the known 3943B identity.
- `SYSTem:ERRor?` remains `00`.
- Unsupported query commands are captured as VISA/read errors or command errors, not treated as evidence to write new commands.

---

### Task 4: Make Smoke Tool Default to Path Configuration Only

**Files:**
- Modify: `src/tools/ceyear_capture_smoke.cpp`
- Modify: `src/services/CeyearVisaQuery.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: Write failing test for no default free-run command**

Update `ceyearSmokeToolDryRunListsCommandsAndRequiresExecute()` so the default dry-run asserts:

```cpp
QVERIFY(!captureLines.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
QVERIFY(!captureLines.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
QVERIFY(captureLines.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.1.20\",5030,IF")));
QVERIFY(captureLines.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.1.20\",5030,\"OPTIONAL_HEADER\"")));
QVERIFY(captureLines.contains(QStringLiteral("TRACe:PATH UDP")));
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```powershell
cmake --build build-ninja --target aircraft_service_tests
ctest --test-dir build-ninja --output-on-failure -R aircraft_service_tests
```

Expected: existing helper still prints `FREE` and `SHORT`, so the test fails.

- [ ] **Step 3: Change smoke command lines to low-risk path setup**

Change `ceyearCaptureSmokeCommandLines()`:

```cpp
QStringList ceyearCaptureSmokeCommandLines(const CeyearCaptureConfig &config)
{
    return ceyearLowRiskPathSetupCommandLines(config);
}
```

- [ ] **Step 4: Add explicit risky flag in `ceyear_capture_smoke`**

Add usage lines:

```cpp
<< "  --allow-risky-free-run Required before sending SYSTem:IF:REMote:MODE SHORT and TRACe:IQ:DATA:REPort FREE\n"
```

Parse:

```cpp
const bool allowRiskyFreeRun = args.contains(QStringLiteral("--allow-risky-free-run"));
```

Reject execute unless acknowledged:

```cpp
if (ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, execute, allowRiskyFreeRun)) {
    err << "Refusing to execute IF/IQ FREE-run stream setup without --allow-risky-free-run. "
        << "Use --probe-if-iq first and confirm front-panel IF/IQ prerequisites.\n";
    return 2;
}
```

- [ ] **Step 5: Build and verify refusal**

Run:

```powershell
cmake --build build-ninja --target ceyear_capture_smoke
.\build-ninja\ceyear_capture_smoke.exe --execute --protocol UDP --pc-ip 192.168.50.20 --pc-port 5030
```

Expected: exits `2`, prints refusal, and does not connect to the device for stream start.

---

### Task 5: Define the Manual/Front-Panel Bring-Up Checklist

**Files:**
- Create: `docs/3943b-if-iq-bringup-checklist.md`

- [ ] **Step 1: Create the checklist**

Create `docs/3943b-if-iq-bringup-checklist.md`:

```markdown
# 3943B-S IF/IQ Bring-Up Checklist

## Do Not Repeat

Do not blindly run:

```text
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:REPort FREE
INITiate
```

This combination has produced zero PC bytes and can make the observed 3943B V2.1.0 reboot or drop the control socket.

## Read-Only Snapshot

Run first:

```powershell
.\build-ninja\ceyear_status_query.exe TCPIP::192.168.50.54::5025::SOCKET --probe-if-iq
```

Record:

- `*IDN?`
- `SYSTem:ERRor?`
- `FREQuency:MODE?`
- `MEASure:MODE?`
- `SYSTem:IF:REMote:MODE?`
- `TRACe:IQ:DATA:REPort?`
- `TRACe:PATH?`
- `TRACe:UDP?`
- `TRACe:TCP?`

## Front-Panel Checks

Confirm on the instrument before any stream start:

- The device is in a mode documented to output IF/IQ data.
- IF/IQ or digital IF streaming is enabled on the front panel if a local switch exists.
- Required options/licenses for digital IF/IQ output are present.
- The selected demodulation/measurement mode is compatible with IF/IQ output.
- The instrument remains responsive after enabling IF remote mode manually.

## First Write Test

Only after the above:

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
SYSTem:ERRor?
TRACe:UDP?
TRACe:PATH?
```

Expected:

- Error queue remains `00`.
- `TRACe:UDP?` shows the PC endpoint.
- No IF stream is started yet.

## First Stream Test

Only with receiver already listening and Wireshark filtering `udp.port == 5030`:

```text
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
TRIGger:IQ:REPort:STATe ON
```

Stop immediately after the receive window:

```text
TRIGger:IQ:REPort:STATe OFF
SYSTem:IF:REMote:MODE OFF
TRACe:RECord:SELectivity OFF
TRACe:RECord:STATe OFF
SYSTem:ERRor?
```

Escalate to `FREE` only after `MANual` emits non-zero bytes and the device remains stable.
```

- [ ] **Step 2: Review against the manual**

Confirm the checklist still matches the manual command names:

```text
TRACe:PATH
TRACe:UDP:TAG
TRACe:UDP:FLAG
TRACe:IQ:DATA:REPort
SYSTem:IF:REMote:MODE
TRIGger:IQ:REPort:STATe
```

Expected: all command names are present in the manual extraction or current evidence.

---

### Task 6: Execute Safe Discovery on Real Instrument

**Files:**
- No code changes
- Output evidence file under: `D:\cqt_platform\aircraft_project\cpp_app\logs\3943b-if-iq-probe-YYYYMMDD-HHMMSS.txt`

- [ ] **Step 1: Build the read-only tool**

Run:

```powershell
cmake --build build-ninja --target ceyear_status_query
```

Expected: build succeeds.

- [ ] **Step 2: Run read-only probe**

Run:

```powershell
New-Item -ItemType Directory -Force -Path .\logs | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
.\build-ninja\ceyear_status_query.exe TCPIP::192.168.50.54::5025::SOCKET --probe-if-iq |
  Tee-Object -FilePath ".\logs\3943b-if-iq-probe-$stamp.txt"
```

Expected:
- No device reboot.
- No control-port drop.
- Error queue remains clear or gives a concrete unsupported-command error.

- [ ] **Step 3: Decide next hypothesis**

Use the probe output:

- If `TRACe:UDP?` / `TRACe:TCP?` queries are unsupported, path introspection must rely on `SYSTem:ERRor?` after writes.
- If `SYSTem:IF:REMote:MODE?` is unsupported, the device firmware may expose a different digital IF command set.
- If all queries are accepted but no stream emits later, front-panel mode/license is the primary hypothesis.

---

### Task 7: Low-Risk Path Configuration Test

**Files:**
- No code changes
- Output evidence file under: `logs/3943b-path-config-YYYYMMDD-HHMMSS.txt`

- [ ] **Step 1: Start Wireshark or packet capture**

Filter:

```text
host 192.168.50.54 or udp.port == 5030 or tcp.port == 5030
```

Expected: control traffic to port `5025` is visible; no IF/IQ data is expected yet.

- [ ] **Step 2: Send only path setup commands**

Run the tool after Task 4 is implemented:

```powershell
.\build-ninja\ceyear_capture_smoke.exe --execute --protocol UDP --pc-ip 192.168.50.20 --pc-port 5030 --trace-scpi
```

Expected:
- If `--allow-risky-free-run` is absent, the tool refuses risky stream start.
- No `SYSTem:IF:REMote:MODE SHORT`, no `TRACe:IQ:DATA:REPort FREE`, no `INITiate`.

- [ ] **Step 3: Query state after path setup**

Run:

```powershell
.\build-ninja\ceyear_status_query.exe TCPIP::192.168.50.54::5025::SOCKET --probe-if-iq
```

Expected:
- `SYSTem:ERRor?` remains `00`.
- `TRACe:PATH?` returns `UDP` if supported.
- `TRACe:UDP?` includes `192.168.50.20,5030` if supported.

---

### Task 8: First Controlled Stream Test

**Files:**
- No code changes unless Task 7 reveals a concrete command mismatch
- Output: raw packet capture and receiver byte count

- [ ] **Step 1: Confirm human prerequisites**

Before running, manually confirm:

```text
front-panel IF/IQ output condition checked
receiver listening on 192.168.50.20:5030
Wireshark filter active
device is responsive to *IDN?
SYSTem:ERRor? is 00
```

- [ ] **Step 2: Use MANual mode, not FREE**

Send only this candidate sequence after explicit approval:

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
TRIGger:IQ:REPort:STATe ON
```

Expected:
- Wireshark sees traffic from `192.168.50.54` to `192.168.50.20:5030`, or no traffic but device remains stable.
- PC receiver reports non-zero bytes only if the stream actually starts.

- [ ] **Step 3: Stop immediately**

Send:

```text
TRIGger:IQ:REPort:STATe OFF
SYSTem:IF:REMote:MODE OFF
TRACe:RECord:SELectivity OFF
TRACe:RECord:STATe OFF
SYSTem:ERRor?
```

Expected:
- Error queue is clear or contains a concrete command error.
- Device remains reachable through `*IDN?`.

- [ ] **Step 4: Record conclusion**

Classify the result:

- `network-bytes-received`: proceed to packet parser and `.cap`.
- `no-bytes-device-stable`: inspect mode/license/front-panel state before next command change.
- `device-reboot-or-socket-drop`: mark this exact sequence as prohibited and stop live tests.

---

### Task 9: Resume `.cap` Work Only After Non-Zero Bytes

**Files:**
- Modify later: `src/services/CeyearIqReceiver.cpp`
- Modify later: `src/services/UsrpDeviceService.cpp` or create a Ceyear-specific CAP writer
- Test later: `tests/service_tests.cpp`

- [ ] **Step 1: Do not implement parser before stream evidence**

Condition to start:

```text
At least one stable test receives non-zero bytes from 192.168.50.54 to PC port 5030.
```

- [ ] **Step 2: Add packet parser test with captured packet**

Create a fixture from a real packet and assert:

```cpp
QCOMPARE(parsed.magic, quint32(0x000EB200));
QCOMPARE(parsed.tag, quint16(901));
QVERIFY(parsed.iqSamples.size() > 0);
```

- [ ] **Step 3: Write `.cap` only from parsed IF/IQ samples**

Use the existing `.cap` contract:

```text
header length: 0x200
bandwidth offset: 0x0010
center frequency offset: 0x0018
sample count offset: 0x0110
payload: big-endian interleaved int16 I/Q
```

Expected: `CapProbeService` can read the generated `.cap`.

---

## Verification Summary

Run before any final claim:

```powershell
cmake --build build-ninja --target aircraft_service_tests ceyear_status_query ceyear_capture_smoke
ctest --test-dir build-ninja --output-on-failure
```

Expected:

```text
2/2 Passed
```

Live-device success is not proven by tests alone. It requires one saved probe log and one packet-capture note showing either non-zero IF/IQ traffic or stable no-byte behavior with a clear next hypothesis.
