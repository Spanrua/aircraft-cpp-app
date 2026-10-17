# 离线 PSC CSV 回放最小迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不替换分段融合、实时扫频或瀑布图实现的前提下，为全流程版本加入 3943B PSC CSV 的导入与按帧离线回放。

**Architecture:** `CeyearPscSweep` 按 `frame_index` 将 CSV 还原为有序 `CeyearPscFrame`；`SweepPage` 用本地 `QTimer` 逐帧复用既有 `appendPscFrame()`。实时采集与离线回放由页面状态互斥。

**Tech Stack:** C++17、Qt 5 Widgets、Qt Test、CMake/Ninja、MSVC 2022。

---

## 文件结构

- `src/services/CeyearPscSweep.h`：CSV 帧读取 API 声明。
- `src/services/CeyearPscSweep.cpp`：解析、排序与零频哨兵 bin 过滤。
- `src/ui/pages/SweepPage.h`：离线回放状态与私有控制方法。
- `src/ui/pages/SweepPage.cpp`：导入入口、80 ms 回放与状态管理。
- `tests/service_tests.cpp`：CSV 正常、哨兵和错误输入测试。
- `tests/ui_tests.cpp`：回放入口和无硬件调用测试。

### Task 1: PSC CSV 服务读取器

**Files:**
- Modify: `src/services/CeyearPscSweep.h:69`
- Modify: `src/services/CeyearPscSweep.cpp:3-6,180`
- Test: `tests/service_tests.cpp:77,1673`

- [ ] **Step 1: 写入失败的服务测试**

在 `ServiceTests` 的 slots 中声明：

```cpp
void ceyearPscSweepCsvReaderRestoresSortedFrames();
void ceyearPscSweepCsvReaderSkipsZeroFrequencySentinelBins();
void ceyearPscSweepCsvReaderRejectsMissingColumnsAndInvalidRows();
```

正常输入测试写入乱序列 CSV，调用下列预期 API，并断言帧按 `frame_index`、bin 按 `bin_index` 恢复：

```cpp
QVector<CeyearPscFrame> frames;
QString error;
QVERIFY2(loadCeyearPscFramesCsv(filePath, &frames, &error), qPrintable(error));
QCOMPARE(frames.size(), 2);
QCOMPARE(frames.at(0).bins.at(0).frequencyHz, 900000000.0);
QCOMPARE(frames.at(0).bins.at(1).frequencyHz, 900500000.0);
```

哨兵测试的输入行是 `0,1,0,200.0`，断言读取成功且该 bin 未进入输出。错误测试分别缺少 `level_dbuv`，以及含 `0,0,not-a-number,-35.0`，断言返回 `false`、帧为空、错误文字非空。

- [ ] **Step 2: 运行测试，确认 RED**

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion --target aircraft_service_tests"
build-fullflow-release-segment-fusion\aircraft_service_tests.exe ceyearPscSweepCsvReaderRestoresSortedFrames
```

Expected: 编译失败，提示 `loadCeyearPscFramesCsv` 未声明。

- [ ] **Step 3: 实现最小 CSV 读取器**

在头文件的写入 API 后声明：

```cpp
bool loadCeyearPscFramesCsv(const QString &filePath,
                            QVector<CeyearPscFrame> *frames,
                            QString *error = nullptr);
```

在实现文件引入 `<QMap>` 和 `<algorithm>`。验证必填列：

```cpp
const QStringList requiredColumns = {
    QStringLiteral("frame_index"),
    QStringLiteral("bin_index"),
    QStringLiteral("frequency_hz"),
    QStringLiteral("level_dbuv"),
};
```

以 `QMap<int, QVector<QPair<int, CeyearPscBin>>>` 聚合有效记录；文件不可读、空表头、缺列、短行、非数值或负 frame/bin 索引均返回 `false` 并设置中文错误。仅跳过无效频率：

```cpp
if (frequencyHz <= 0.0) {
    continue;
}
indexedFrames[frameIndex].append({binIndex, {frequencyHz, levelDbuv}});
```

遍历 `QMap` 时按 `bin_index` 排序，生成 `CeyearPscFrame`。若无有效帧，返回 `false` 并报告 `PSC CSV 不包含任何帧`。

- [ ] **Step 4: 运行测试，确认 GREEN**

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion --target aircraft_service_tests"
ctest --test-dir build-fullflow-release-segment-fusion -R aircraft_service_tests --output-on-failure
```

Expected: `aircraft_service_tests` 通过。

- [ ] **Step 5: 提交服务读取器**

```powershell
git add src/services/CeyearPscSweep.h src/services/CeyearPscSweep.cpp tests/service_tests.cpp
git commit -m "feat: load PSC sweep CSV frames"
```

### Task 2: SweepPage 离线回放控制

**Files:**
- Modify: `src/ui/pages/SweepPage.h:13-80`
- Modify: `src/ui/pages/SweepPage.cpp:10-28,113-264,339-397`
- Test: `tests/ui_tests.cpp:404,655`

- [ ] **Step 1: 写入失败的界面测试**

在 `UiTests` slots 中声明：

```cpp
void sweepPageExposesOfflineCsvReplayButton();
void sweepPageReplaysOfflineFramesWithoutStartingHardware();
```

入口测试定位 `ImportPscSweepCsvButton`，并断言文字 `导入并回放 CSV`、初始可用。回放测试用两帧调用测试入口，并断言：

```cpp
page.startOfflineReplayForTest(frames);
QTRY_COMPARE(waterfall->frameCount(), 2);
QTRY_VERIFY(candidateTable->rowCount() >= 1);
QTRY_VERIFY(statusLabel->text().contains(QStringLiteral("离线演示回放已完成")));
QCOMPARE(servicePtr->startPscSweepCount, 0);
QCOMPARE(servicePtr->stopPscSweepCount, 0);
```

- [ ] **Step 2: 运行测试，确认 RED**

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion --target aircraft_ui_tests"
build-fullflow-release-segment-fusion\aircraft_ui_tests.exe sweepPageExposesOfflineCsvReplayButton
```

Expected: 编译失败，提示测试入口未定义或导入控件不存在。

- [ ] **Step 3: 实现最小离线回放**

在头文件前置声明 `QTimer`，加入测试入口、导入、开始、逐帧、停止、结束、状态更新六个私有方法；加入回放标记、帧队列、索引、计时器与导入按钮成员。两个构造函数均创建计时器：

```cpp
m_offlineReplayTimer = new QTimer(this);
m_offlineReplayTimer->setInterval(80);
connect(m_offlineReplayTimer, &QTimer::timeout, this, &SweepPage::playNextOfflineFrame);
```

创建对象名 `ImportPscSweepCsvButton`、文字 `导入并回放 CSV` 的按钮并连接 `importOfflineSweepCsv()`。该方法用 `QFileDialog::getOpenFileName()` 选择 CSV，调用读取 API；导入失败仅写状态 `离线扫频数据导入失败：...`。

`startOfflineReplay()` 扫描有效 bin 配置显示频段，调用既有 `resetPscVisualization(config)`，禁用开始/导入按钮、启用停止按钮并启动定时器。逐帧处理仅复用现有路径：

```cpp
appendPscFrame(m_offlineReplayFrames.at(m_nextOfflineReplayFrame));
++m_nextOfflineReplayFrame;
++m_pscFrameCount;
```

完成或停止时停表、清空队列、恢复按钮并更新状态。`startSweep()` 在离线回放时返回；`stopSweep()` 在离线回放时停止回放后返回。离线路径不可调用 VISA 服务。

- [ ] **Step 4: 运行测试，确认 GREEN**

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion --target aircraft_ui_tests"
ctest --test-dir build-fullflow-release-segment-fusion -R aircraft_ui_tests --output-on-failure
```

Expected: `aircraft_ui_tests` 通过，Fake VISA 的 PSC start/stop 计数均为 0。

- [ ] **Step 5: 提交回放界面**

```powershell
git add src/ui/pages/SweepPage.h src/ui/pages/SweepPage.cpp tests/ui_tests.cpp
git commit -m "feat: replay PSC sweep CSV offline"
```

### Task 3: 全流程构建与隔离验证

**Files:**
- Modify: 无

- [ ] **Step 1: 构建 Release 应用与测试目标**

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build build-fullflow-release-segment-fusion --target aircraft_qt aircraft_service_tests aircraft_ui_tests"
```

Expected: 三个目标成功链接，且不改动 `CMakeLists.txt`。

- [ ] **Step 2: 运行完整 CTest 集合**

```powershell
ctest --test-dir build-fullflow-release-segment-fusion --output-on-failure
```

Expected: 全部已配置测试通过。

- [ ] **Step 3: 只读检查真实导入文件**

```powershell
Get-Content D:\cqt_platform\aircraft_project\cpp_app\data\sweep\3943b_psc_sweep_20261007_191357_846_part023.csv -TotalCount 2
```

Expected: 文件不被修改，表头包含 `frame_index,bin_index,frequency_hz,level_dbuv`；程序中可通过导入入口选择它回放。
