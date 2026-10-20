# PSC 宽带信号告警 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在实时 PSC 扫频和 CSV 回放中，对持续两帧、占用带宽为 16–24 MHz 的信号显示非阻断式疑似无人机宽带告警。

**Architecture:** 新建纯业务的 `PscWidebandAlertDetector`，从每帧高于当前命中阈值的连续频点计算频段与占用带宽，并仅跨相邻帧累计同中心频段。`SweepPage` 在实时和离线共用的 `appendPscFrame` 中调用检测器；`WaterfallWidget` 只负责绘制已经确认的频段覆盖，页面负责告警带和候选表。

**Tech Stack:** C++17、Qt 5 Widgets、Qt Test、CMake/CTest。

---

## File structure

- Create: `src/services/PscWidebandAlertDetector.h` — 宽带频段数据模型与无 UI 依赖的检测器接口。
- Create: `src/services/PscWidebandAlertDetector.cpp` — 连续 bin 分组、带宽计算、跨帧确认与重置。
- Modify: `CMakeLists.txt` — 将检测器加入 `aircraft_core`。
- Modify: `tests/service_tests.cpp` — 边界、连续帧和重置的服务测试。
- Modify: `src/ui/widgets/WaterfallWidget.h` — 接受/清除已确认的告警频段并提供可观测状态。
- Modify: `src/ui/widgets/WaterfallWidget.cpp` — 在时频图上绘制半透明橙色频段覆盖和边界。
- Modify: `src/ui/pages/SweepPage.h` — 保存检测器、当前告警与告警标签。
- Modify: `src/ui/pages/SweepPage.cpp` — 在公共帧入口更新告警，扩展候选表，重置告警 UI。
- Modify: `src/ui/AppTheme.cpp` — 为页面内告警带增加样式。
- Modify: `tests/ui_tests.cpp` — 端到端验证扫频页告警、表格和离线回放路径。

### Task 1: 建立可单测的宽带检测服务

**Files:**
- Create: `src/services/PscWidebandAlertDetector.h`
- Create: `src/services/PscWidebandAlertDetector.cpp`
- Modify: `CMakeLists.txt: AIRCRAFT_CORE_SOURCES`
- Modify: `tests/service_tests.cpp: ServiceTests private slots and PSC tests`

- [ ] **Step 1: 写出失败的服务测试和测试帧构造器。**

在文件现有服务 include 列表中加入 `#include "services/PscWidebandAlertDetector.h"`，并在 PSC 测试辅助函数区域加入：

```cpp
static CeyearPscFrame widebandFrame(double startHz, int binCount, double stepHz, double levelDbuv)
{
    CeyearPscFrame frame;
    frame.bins.reserve(binCount);
    for (int index = 0; index < binCount; ++index) {
        frame.bins.append({startHz + index * stepHz, levelDbuv});
    }
    return frame;
}
```

在 `ServiceTests` 的 `private slots` 声明：

```cpp
void pscWidebandAlertDetectorConfirmsTwoFramesInsideConfiguredRange();
void pscWidebandAlertDetectorRejectsOutOfRangeBandsAndResetClearsState();
```

添加测试；20 个间隔 1 MHz 的 bin 代表 20 MHz 的占用带宽（跨度加一个 bin 步进）：

```cpp
void ServiceTests::pscWidebandAlertDetectorConfirmsTwoFramesInsideConfiguredRange()
{
    PscWidebandAlertDetector detector(-60.0);
    const CeyearPscFrame frame = widebandFrame(2400000000.0, 20, 1000000.0, -42.0);

    QVERIFY(detector.update(frame).isEmpty());
    const QVector<PscWidebandAlert> alerts = detector.update(frame);
    QCOMPARE(alerts.size(), 1);
    QCOMPARE(alerts.first().startHz, 2400000000.0);
    QCOMPARE(alerts.first().stopHz, 2419000000.0);
    QCOMPARE(alerts.first().centerHz, 2409500000.0);
    QCOMPARE(alerts.first().bandwidthHz, 20000000.0);
    QCOMPARE(alerts.first().peakLevelDbuv, -42.0);
    QCOMPARE(alerts.first().consecutiveFrameCount, 2);
}

void ServiceTests::pscWidebandAlertDetectorRejectsOutOfRangeBandsAndResetClearsState()
{
    PscWidebandAlertDetector detector(-60.0);
    QVERIFY(detector.update(widebandFrame(2400000000.0, 15, 1000000.0, -40.0)).isEmpty());
    QVERIFY(detector.update(widebandFrame(2400000000.0, 25, 1000000.0, -40.0)).isEmpty());

    const CeyearPscFrame lowerBoundary = widebandFrame(2400000000.0, 16, 1000000.0, -40.0);
    QVERIFY(detector.update(lowerBoundary).isEmpty());
    QCOMPARE(detector.update(lowerBoundary).size(), 1);
    detector.reset();
    QVERIFY(detector.update(lowerBoundary).isEmpty());
}
```

- [ ] **Step 2: 运行服务测试，确认因为检测器尚不存在而失败。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_service_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure -R aircraft_service_tests
```

Expected: 编译失败，明确指出 `services/PscWidebandAlertDetector.h` 或 `PscWidebandAlertDetector` 尚不存在；不是测试运行时失败。

- [ ] **Step 3: 实现最小检测器并加入核心库。**

创建 `src/services/PscWidebandAlertDetector.h`：

```cpp
#pragma once

#include "services/CeyearPscSweep.h"

#include <QVector>

struct PscWidebandAlert
{
    double startHz = 0.0;
    double stopHz = 0.0;
    double centerHz = 0.0;
    double bandwidthHz = 0.0;
    double peakLevelDbuv = 0.0;
    int consecutiveFrameCount = 0;
};

class PscWidebandAlertDetector
{
public:
    explicit PscWidebandAlertDetector(double hitThresholdDbuv = -60.0,
                                      double minimumBandwidthHz = 16000000.0,
                                      double maximumBandwidthHz = 24000000.0,
                                      int minimumConsecutiveFrames = 2);

    QVector<PscWidebandAlert> update(const CeyearPscFrame &frame);
    void reset();

private:
    double m_hitThresholdDbuv;
    double m_minimumBandwidthHz;
    double m_maximumBandwidthHz;
    int m_minimumConsecutiveFrames;
    QVector<PscWidebandAlert> m_previousFrameBands;
};
```

创建 `src/services/PscWidebandAlertDetector.cpp`；完整实现如下。它按频率排序复制 `frame.bins`；用相邻频率的最小正差值作为 bin 步进；将间隔不大于 `binStepHz * 1.5` 的、且电平不低于 `m_hitThresholdDbuv` 的 bin 合并；以 `last.frequencyHz - first.frequencyHz + binStepHz` 得到带宽；仅保留 `[m_minimumBandwidthHz, m_maximumBandwidthHz]`；并按 `qMax(25000.0, binStepHz * 2.0)` 的中心频率容差匹配上一帧频段。

```cpp
#include "services/PscWidebandAlertDetector.h"

#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace
{
double smallestPositiveBinStepHz(const QVector<CeyearPscBin> &bins)
{
    double stepHz = std::numeric_limits<double>::max();
    for (int index = 1; index < bins.size(); ++index) {
        const double differenceHz = bins.at(index).frequencyHz - bins.at(index - 1).frequencyHz;
        if (differenceHz > 0.0) {
            stepHz = qMin(stepHz, differenceHz);
        }
    }
    return stepHz == std::numeric_limits<double>::max() ? 0.0 : stepHz;
}

PscWidebandAlert makeBand(const QVector<CeyearPscBin> &bins, double binStepHz)
{
    PscWidebandAlert band;
    band.startHz = bins.first().frequencyHz;
    band.stopHz = bins.last().frequencyHz;
    band.centerHz = (band.startHz + band.stopHz) / 2.0;
    band.bandwidthHz = band.stopHz - band.startHz + binStepHz;
    band.peakLevelDbuv = bins.first().levelDbuv;
    for (const CeyearPscBin &bin : bins) {
        band.peakLevelDbuv = qMax(band.peakLevelDbuv, bin.levelDbuv);
    }
    return band;
}
} // namespace

PscWidebandAlertDetector::PscWidebandAlertDetector(double hitThresholdDbuv,
                                                     double minimumBandwidthHz,
                                                     double maximumBandwidthHz,
                                                     int minimumConsecutiveFrames)
    : m_hitThresholdDbuv(hitThresholdDbuv),
      m_minimumBandwidthHz(minimumBandwidthHz),
      m_maximumBandwidthHz(maximumBandwidthHz),
      m_minimumConsecutiveFrames(qMax(1, minimumConsecutiveFrames))
{
}

QVector<PscWidebandAlert> PscWidebandAlertDetector::update(const CeyearPscFrame &frame)
{
    QVector<CeyearPscBin> bins = frame.bins;
    std::sort(bins.begin(), bins.end(), [](const CeyearPscBin &left, const CeyearPscBin &right) {
        return left.frequencyHz < right.frequencyHz;
    });
    const double binStepHz = smallestPositiveBinStepHz(bins);
    if (binStepHz <= 0.0) {
        m_previousFrameBands.clear();
        return {};
    }

    QVector<PscWidebandAlert> currentBands;
    QVector<CeyearPscBin> run;
    const auto flushRun = [this, binStepHz, &currentBands](const QVector<CeyearPscBin> &value) {
        if (value.isEmpty()) {
            return;
        }
        const PscWidebandAlert band = makeBand(value, binStepHz);
        if (band.bandwidthHz >= m_minimumBandwidthHz && band.bandwidthHz <= m_maximumBandwidthHz) {
            currentBands.append(band);
        }
    };
    for (const CeyearPscBin &bin : bins) {
        if (bin.levelDbuv < m_hitThresholdDbuv) {
            flushRun(run);
            run.clear();
            continue;
        }
        if (!run.isEmpty() && bin.frequencyHz - run.last().frequencyHz > binStepHz * 1.5) {
            flushRun(run);
            run.clear();
        }
        run.append(bin);
    }
    flushRun(run);

    const double centerToleranceHz = qMax(25000.0, binStepHz * 2.0);
    for (PscWidebandAlert &band : currentBands) {
        band.consecutiveFrameCount = 1;
        for (const PscWidebandAlert &previous : m_previousFrameBands) {
            if (qAbs(band.centerHz - previous.centerHz) <= centerToleranceHz) {
                band.consecutiveFrameCount = previous.consecutiveFrameCount + 1;
                break;
            }
        }
    }
    m_previousFrameBands = currentBands;

    QVector<PscWidebandAlert> confirmed;
    for (const PscWidebandAlert &band : currentBands) {
        if (band.consecutiveFrameCount >= m_minimumConsecutiveFrames) {
            confirmed.append(band);
        }
    }
    return confirmed;
}

void PscWidebandAlertDetector::reset()
{
    m_previousFrameBands.clear();
}
```

`reset()` 只执行 `m_previousFrameBands.clear()`。在 `CMakeLists.txt` 的 `AIRCRAFT_CORE_SOURCES` 中紧邻 `CeyearPscSweep.cpp` 加入：

```cmake
    src/services/PscWidebandAlertDetector.cpp
```

- [ ] **Step 4: 再次运行服务测试，确认通过。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_service_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure -R aircraft_service_tests
```

Expected: `aircraft_service_tests` 通过；15/25 MHz 不告警，16 MHz 和 20 MHz 在第二帧告警，重置后恢复第一帧状态。

- [ ] **Step 5: 提交检测器与服务测试。**

```powershell
git add CMakeLists.txt src/services/PscWidebandAlertDetector.h src/services/PscWidebandAlertDetector.cpp tests/service_tests.cpp
git commit -m "feat: detect persistent PSC wideband signals"
```

### Task 2: 在时频图标记确认后的告警频段

**Files:**
- Modify: `src/ui/widgets/WaterfallWidget.h`
- Modify: `src/ui/widgets/WaterfallWidget.cpp`
- Modify: `tests/ui_tests.cpp: UiTests slots and waterfall tests`

- [ ] **Step 1: 写出失败的时频图标记测试。**

在 `UiTests` 中声明并实现：

```cpp
void waterfallWidgetRendersConfirmedWidebandAlert();

void UiTests::waterfallWidgetRendersConfirmedWidebandAlert()
{
    WaterfallWidget widget;
    widget.resize(600, 300);
    widget.setFrequencyRange(2400000000.0, 2430000000.0);
    widget.appendFrame(widebandFrame(2400000000.0, 30, 1000000.0, -42.0));
    widget.setAlertBand(PscWidebandAlert{2405000000.0, 2424500000.0, 2414750000.0,
                                         20000000.0, -42.0, 2});

    QVERIFY(widget.hasAlertBand());
    QImage rendered(widget.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);
    const QRect plot = widget.plotArea();
    const QColor overlay = rendered.pixelColor(plot.center().x(), plot.center().y());
    QVERIFY(overlay.red() > overlay.green());
    widget.clearAlertBand();
    QVERIFY(!widget.hasAlertBand());
}
```

将 `widebandFrame` 也放在 `ui_tests.cpp` 的静态辅助函数区域；确保其定义与 Task 1 完全一致。

- [ ] **Step 2: 运行 UI 测试，确认新接口不存在导致失败。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_ui_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure -R aircraft_ui_tests
```

Expected: 编译失败，指出 `setAlertBand`、`hasAlertBand` 和 `clearAlertBand` 尚未声明。

- [ ] **Step 3: 添加频段覆盖状态和绘制。**

在 `WaterfallWidget.h` 中包含 `<optional>` 和检测器头文件，新增：

```cpp
void setAlertBand(const PscWidebandAlert &alert);
void clearAlertBand();
bool hasAlertBand() const;
```

并增加成员：

```cpp
std::optional<PscWidebandAlert> m_alertBand;
```

在 `.cpp` 中实现 setter：

```cpp
void WaterfallWidget::setAlertBand(const PscWidebandAlert &alert)
{
    m_alertBand = alert;
    update();
}

void WaterfallWidget::clearAlertBand()
{
    m_alertBand.reset();
    update();
}

bool WaterfallWidget::hasAlertBand() const
{
    return m_alertBand.has_value();
}
```

在 `paintEvent` 完成帧 bin 绘制、调用 `drawAxes` 之前，若 `m_alertBand` 存在且 `axisStopHz > axisStartHz`，绘制：

```cpp
const int x0 = plot.left() + frequencyToX(m_alertBand->startHz, axisStartHz, axisStopHz, plot.width());
const int x1 = plot.left() + frequencyToX(m_alertBand->stopHz, axisStartHz, axisStopHz, plot.width());
const QRect bandRect(qMin(x0, x1), plot.top(), qMax(1, qAbs(x1 - x0)), plot.height());
painter.fillRect(bandRect, QColor(245, 158, 11, 52));
painter.setPen(QPen(QColor(251, 191, 36), 2));
painter.drawLine(bandRect.left(), plot.top(), bandRect.left(), plot.bottom());
painter.drawLine(bandRect.right(), plot.top(), bandRect.right(), plot.bottom());
```

对 `bandRect` 使用 `intersected(plot)`，避免频段越界污染坐标轴。

- [ ] **Step 4: 重跑 UI 测试，确认告警覆盖可绘制和清除。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_ui_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure -R aircraft_ui_tests
```

Expected: `aircraft_ui_tests` 通过，绘制结果在告警频段中央具有暖色覆盖；清除后 `hasAlertBand()` 为 false。

- [ ] **Step 5: 提交时频图告警覆盖。**

```powershell
git add src/ui/widgets/WaterfallWidget.h src/ui/widgets/WaterfallWidget.cpp tests/ui_tests.cpp
git commit -m "feat: highlight PSC wideband alert bands"
```

### Task 3: 将检测结果接入扫频页和离线回放

**Files:**
- Modify: `src/ui/pages/SweepPage.h`
- Modify: `src/ui/pages/SweepPage.cpp`
- Modify: `src/ui/AppTheme.cpp`
- Modify: `tests/ui_tests.cpp: UiTests slots and sweep page tests`

- [ ] **Step 1: 写出失败的扫频页告警测试。**

在 `UiTests` 中声明并实现：

```cpp
void sweepPageShowsWidebandAlertForLiveAndOfflineFrames();
void sweepPageClearsWidebandAlertWhenReplayStartsAgain();
```

第一项测试以 `appendPscFrameForTest` 连续送入两帧 20 MHz 数据，并验证：

```cpp
auto *alertLabel = page.findChild<QLabel *>(QStringLiteral("PscWidebandAlertLabel"));
auto *waterfall = page.findChild<WaterfallWidget *>(QStringLiteral("PscWaterfallWidget"));
auto *candidateTable = page.findChild<QTableWidget *>(QStringLiteral("PscCandidateTable"));
QVERIFY(alertLabel != nullptr);
QVERIFY(waterfall != nullptr);
QVERIFY(candidateTable != nullptr);

page.appendPscFrameForTest(widebandFrame(2400000000.0, 20, 1000000.0, -42.0));
page.appendPscFrameForTest(widebandFrame(2400000000.0, 20, 1000000.0, -41.0));

QVERIFY(!alertLabel->isHidden());
QVERIFY(alertLabel->text().contains(QStringLiteral("疑似无人机宽带信号")));
QVERIFY(alertLabel->text().contains(QStringLiteral("20.0 MHz")));
QVERIFY(waterfall->hasAlertBand());
QVERIFY(QTest::qWaitFor([candidateTable]() {
    for (int row = 0; row < candidateTable->rowCount(); ++row) {
        if (candidateTable->item(row, 4) && candidateTable->item(row, 4)->text() == QStringLiteral("告警")) {
            return true;
        }
    }
    return false;
}));
```

第二项测试先触发该告警，再调用 `startOfflineReplayForTest` 传入一帧完全低于阈值的数据；在回放完成后验证 `alertLabel->isHidden()`、`!waterfall->hasAlertBand()`，且候选表中不再存在“告警”。这同时证明 CSV 回放会复用公共帧入口。

- [ ] **Step 2: 运行 UI 测试，确认告警控件与状态尚不存在。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_ui_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure -R aircraft_ui_tests
```

Expected: 测试在 `PscWidebandAlertLabel`、候选表第 5 列或告警状态断言处失败；旧功能测试仍能编译。

- [ ] **Step 3: 以公共帧入口接入告警并更新页面。**

在 `SweepPage.h` 引入 `services/PscWidebandAlertDetector.h` 和 `<optional>`，增加：

```cpp
void updateWidebandAlert(const QVector<PscWidebandAlert> &alerts);
void clearWidebandAlert();
void setCandidateRows(const QVector<CeyearPscCandidate> &candidates,
                      const QVector<PscWidebandAlert> &alerts = {});

PscWidebandAlertDetector m_widebandAlertDetector;
std::optional<PscWidebandAlert> m_activeWidebandAlert;
QLabel *m_widebandAlertLabel = nullptr;
```

在 `buildUi` 中于 `m_statusLabel` 前创建 `PscWidebandAlertLabel`，初始 `hide()`，并把候选表从 3 列扩为 5 列，标题固定为：

```cpp
{QStringLiteral("频率 Hz"), QStringLiteral("峰值 dBuV"), QStringLiteral("命中"),
 QStringLiteral("带宽 MHz"), QStringLiteral("状态")}
```

在 `resetPscVisualization` 中按当前 `config.hitThresholdDbuv` 重新赋值检测器，并调用 `clearWidebandAlert()`：

```cpp
m_widebandAlertDetector = PscWidebandAlertDetector(config.hitThresholdDbuv);
clearWidebandAlert();
```

`clearWidebandAlert()` 必须清空 `m_activeWidebandAlert`、隐藏并清空标签、以及调用 `m_waterfall->clearAlertBand()`。在 `appendPscFrame` 的 `m_candidateExtractor.update(frame)` 后立即执行：

```cpp
const QVector<PscWidebandAlert> alerts = m_widebandAlertDetector.update(frame);
if (!alerts.isEmpty()) {
    updateWidebandAlert(alerts);
}
```

`updateWidebandAlert()` 选择 `peakLevelDbuv` 最大的告警，写入 `m_activeWidebandAlert`，显示以下中文文本（频率单位 GHz、带宽单位 MHz、电平单位 dBuV），并调用 `m_waterfall->setAlertBand(alert)`：

```cpp
QStringLiteral("⚠ 疑似无人机宽带信号：中心 %1 GHz｜带宽 %2 MHz｜峰值 %3 dBuV")
    .arg(alert.centerHz / 1000000000.0, 0, 'f', 3)
    .arg(alert.bandwidthHz / 1000000.0, 0, 'f', 1)
    .arg(alert.peakLevelDbuv, 0, 'f', 1)
```

刷新候选表时，把普通 `CeyearPscCandidate` 行的第 4、5 列设为 `--`、`候选`；把确认后的 `PscWidebandAlert` 追加成行，中心频率填入第 1 列、峰值和连续次数填入第 2、3 列、带宽填入第 4 列、状态为 `告警`。当本帧没有新确认告警但 `m_activeWidebandAlert` 存在时，仍追加该活动告警且不重复添加，以符合“重置前持续可见”的规则。告警行所有单元格使用浅橙色背景和深色前景；现有“确认并采集”继续读取第 1 列，因此不改变流程。

在 `AppTheme.cpp` 添加 `QLabel#PscWidebandAlertLabel` 的橙色背景、边框、圆角、内边距和高对比文字样式，使其与其它警告组件一致。

- [ ] **Step 4: 运行 UI 与服务测试，确认公共路径均通过。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify --target aircraft_service_tests aircraft_ui_tests"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure
```

Expected: 两个测试目标均通过；新测试证明实时帧与离线回放均触发同一告警路径，重置没有遗留警报。

- [ ] **Step 5: 提交页面集成与 UI 测试。**

```powershell
git add src/ui/pages/SweepPage.h src/ui/pages/SweepPage.cpp src/ui/AppTheme.cpp tests/ui_tests.cpp
git commit -m "feat: surface PSC wideband signal alerts"
```

### Task 4: 完整验证并生成不覆盖现有演示包的新构建

**Files:**
- Verify only: `build-fullflow-release-segment-fusion-replay-verify/`
- Create only after tests pass: `fullflow-release-segment-fusion-wideband-alert/`

- [ ] **Step 1: 配置、构建并运行完整 CTest。**

Run:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake -S . -B build-fullflow-release-segment-fusion-replay-verify -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/5.12.10/msvc2017_64"
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion-replay-verify"
ctest --test-dir build-fullflow-release-segment-fusion-replay-verify --output-on-failure
```

Expected: `aircraft_service_tests` 和 `aircraft_ui_tests` 均通过。

- [ ] **Step 2: 复制为新的演示目录，不改写既有离线回放包。**

验证 `fullflow-release-segment-fusion-wideband-alert` 尚不存在，然后复制新编译的 `aircraft_qt.exe`、使用 `windeployqt --release` 部署 Qt 运行库，并创建与现有离线演示包相同的启动脚本；脚本必须设置：

```bat
set "AIRCRAFT_CPP_APP_ROOT=D:\cqt_platform\aircraft_project\cpp_app"
set "AIRCRAFT_DATA_DIR=D:\cqt_platform\aircraft_project\cpp_app\data"
```

Expected: 新目录独立存在，原 `fullflow-release-segment-fusion-offline-replay` 不变。

- [ ] **Step 3: 启动验证并提交所有本次源代码。**

在受控启动中检查新程序能打开扫频页、能导入既有 PSC CSV 且不崩溃；无需连接 3943B。随后仅提交 Task 1–3 所列源文件（演示包保持未跟踪）：

```powershell
git status --short
git log -3 --oneline
```

Expected: 工作树中不含意外修改；若存在用户原有改动，保持不暂存、不修改。
