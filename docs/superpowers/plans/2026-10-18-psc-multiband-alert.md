# PSC 多档宽带预警 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 PSC 预警从单一 20M 档扩展为 10M、20M、40M 三档，并保留实时与 CSV 回放的统一行为。

**Architecture:** 检测器持有一张不可变的带宽规则表，为每个确认频段写入类型标签。SweepPage 只消费标签来展示横幅和表格状态；WaterfallWidget 无须改变，因为它使用实际频段和带宽。

**Tech Stack:** C++17、Qt 5.12 Widgets、Qt Test。

---

### Task 1: 为检测器引入多档规则与类型

**Files:**
- Modify: `src/services/PscWidebandAlertDetector.h`
- Modify: `src/services/PscWidebandAlertDetector.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: 写失败服务测试**

在 `ServiceTests` 中添加数据驱动测试，连续两帧分别构造 `8/12/16/24/32/48 MHz` 频段，断言第二帧唯一告警的 `bandwidthClass` 分别为 `10M/10M/20M/20M/40M/40M`；再测试 `13、15、25、31、49 MHz` 两帧后为空。添加相同中心频率但 `20M` 后接 `40M` 的帧，断言不会继承前一类型的连续计数。

- [ ] **Step 2: 运行失败测试**

Run: `cmake --build build-psc-wideband-alert --target aircraft_service_tests`，随后在临时 PATH 注入 Qt/VISA DLL 后执行新增 QtTest 函数。

Expected: FAIL，因为 `bandwidthClass` 与规则表尚不存在。

- [ ] **Step 3: 最小实现规则表**

在头文件定义：

```cpp
struct PscWidebandRule { QString bandwidthClass; double minimumBandwidthHz; double maximumBandwidthHz; };
QString bandwidthClass;
static QVector<PscWidebandRule> defaultRules();
```

将 `m_minimumBandwidthHz/m_maximumBandwidthHz` 改为规则表。`appendBand` 计算实际 `bandwidthHz` 后，查找第一个满足闭区间的规则；命中时写入 `bandwidthClass`。跨帧匹配条件增加 `alert.bandwidthClass == previous.bandwidthClass`。

- [ ] **Step 4: 运行定向测试**

Run: 构建服务测试并运行所有 `pscWidebandAlert*` 测试。

Expected: PASS。

- [ ] **Step 5: 提交**

```powershell
git add src/services/PscWidebandAlertDetector.h src/services/PscWidebandAlertDetector.cpp tests/service_tests.cpp
git commit -m "feat: classify PSC alert bandwidths"
```

### Task 2: 在扫频页展示带宽类型

**Files:**
- Modify: `src/ui/pages/SweepPage.cpp`
- Test: `tests/ui_tests.cpp`

- [ ] **Step 1: 写失败 UI 测试**

添加三组 `appendPscFrame` 两帧测试，分别输入 10M、20M、40M 帧；断言 `PscWidebandAlertLabel` 可见且包含 `（10M）`、`（20M）`、`（40M）`，候选表状态列分别为 `告警·10M`、`告警·20M`、`告警·40M`。添加同中心不同类型顺序帧，断言不会错误合并历史行。

- [ ] **Step 2: 运行失败测试**

Run: `cmake --build build-psc-wideband-alert --target aircraft_ui_tests`。

Expected: 测试源码可编译但新断言失败；若最终链接仍被既有 `RecognitionPage::recognitionCompleted()` 阻断，记录该已知基线阻塞。

- [ ] **Step 3: 最小 UI 实现**

将 `showWidebandAlert` 标签格式改为：

```cpp
QStringLiteral("⚠ 疑似无人机宽带信号（%1）：中心 %2 GHz｜带宽 %3 MHz｜峰值 %4 dBuV")
```

使用 `alert.bandwidthClass` 填入 `%1`。`setCandidateRows` 中确认历史行的状态文字使用 `QStringLiteral("告警·%1").arg(alert.bandwidthClass)`；普通候选保持 `候选`。保持峰值最高者更新横幅/瀑布图与现有 reset 行为。

- [ ] **Step 4: 验证**

Run: 重新构建 `aircraft_qt`、UI 测试目标；以临时 DLL PATH 运行可执行的定向 UI 测试。检查 `git diff --check`。

- [ ] **Step 5: 提交**

```powershell
git add src/ui/pages/SweepPage.cpp tests/ui_tests.cpp
git commit -m "feat: show PSC alert bandwidth classes"
```

### Task 3: 回归、发布验证

**Files:**
- Modify: `tests/ui_tests.cpp`（仅在 Task 2 的 CSV 覆盖尚未覆盖 10M/40M 时）
- Verify: `build-psc-wideband-alert/aircraft_qt.exe`

- [ ] **Step 1: CSV 回放覆盖**

用临时 PSC CSV 写入两帧 40M 合格频段，走“导入并回放 CSV”路径，断言第二帧后横幅包含 `40M`、WaterfallWidget 有告警带、表格状态为 `告警·40M`；启动新回放后断言横幅和覆盖清除。

- [ ] **Step 2: 构建和运行验证**

Run: `cmake --build build-psc-wideband-alert --target aircraft_qt aircraft_service_tests aircraft_ui_tests`。

Expected: `aircraft_qt` 与服务测试构建成功；若 UI 测试仍停在既有 RecognitionPage 链接错误，明确记录而不修改无关问题。运行全部可执行的 `pscWidebandAlert*` 服务测试和 UI 定向测试。

- [ ] **Step 3: 更新独立包**

重新运行现有发布脚本，输出到新的 `fullflow-release-segment-fusion-multiband-alert` 目录，不覆盖此前 `wideband-alert` 包。短暂启动 EXE 验证进程存活后结束仅该验证进程。

- [ ] **Step 4: 提交测试（如有改动）**

```powershell
git add tests/ui_tests.cpp
git commit -m "test: cover PSC multiband CSV replay"
```
