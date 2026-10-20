# PSC 离线分片预警 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 使分片式 PSC CSV 回放在单个合格 10M/20M/40M 频段首次播放时立即显示预警，同时保持实时扫频两帧确认。

**Architecture:** `SweepPage` 已区分 `m_offlineReplayActive`。在共同 `appendPscFrame` 入口中，实时路径继续使用 detector 确认结果；离线路径以 detector 的单帧分类结果生成当前片段告警，再复用 `showWidebandAlert` 与表格/瀑布图显示。

**Tech Stack:** C++17、Qt 5.12 Widgets、Qt Test。

---

### Task 1: 暴露单帧宽带分类结果

**Files:**
- Modify: `src/services/PscWidebandAlertDetector.h`
- Modify: `src/services/PscWidebandAlertDetector.cpp`
- Test: `tests/service_tests.cpp`

- [ ] **Step 1: 写失败测试**

添加 `classifyFrame` 测试：单个 10 MHz 高电平分片返回一条 `10M` 告警候选且 `consecutiveFrameCount == 1`；`update` 对该首帧仍为空。

- [ ] **Step 2: 实现单帧分类 API**

新增 `QVector<PscWidebandAlert> classifyFrame(const CeyearPscFrame &frame) const`，复用排序、步进推导、阈值分段和规则表代码；`update` 调用同一分类逻辑后再执行跨帧连续确认。

- [ ] **Step 3: 验证并提交**

构建 `aircraft_service_tests`，运行新增定向测试；提交 detector 与测试。

### Task 2: 离线回放立即展示分片预警

**Files:**
- Modify: `src/ui/pages/SweepPage.cpp`
- Modify: `tests/ui_tests.cpp`

- [ ] **Step 1: 写失败 UI 测试**

构造两帧中心相差 10 MHz 的 10M CSV，导入并播放首帧后断言横幅含 `10M`、Waterfall 有频带和状态 `告警·10M`；第二片播放后断言存在两条独立告警活动。添加实时首帧同样输入仍无横幅的断言。

- [ ] **Step 2: 最小实现**

在 `appendPscFrame` 内，当 `m_offlineReplayActive` 为真时调用 `classifyFrame`，选择峰值最高候选并调用 `showWidebandAlert`；离线历史合并仅在中心频率落入双方容差时发生，因此相差 10 MHz 的片段独立保留。非离线分支保持 `update` 两帧确认。

- [ ] **Step 3: 构建、验证、提交**

构建 `aircraft_qt` 和 UI 测试目标；记录既有 RecognitionPage 链接阻塞；运行可执行服务定向测试，提交 SweepPage/UI 测试。

### Task 3: 更新 Release 包

**Files:**
- Verify: `build-psc-wideband-alert-release/aircraft_qt.exe`
- Replace: `fullflow-release-segment-fusion-wideband-alert/cpp_app/aircraft_qt.exe`

- [ ] **Step 1: 构建 Release EXE**

运行 `cmake --build build-psc-wideband-alert-release --target aircraft_qt`。

- [ ] **Step 2: 部署并启动验证**

在确认 `aircraft_qt.exe` 未运行后，用 Release EXE 覆盖包内 EXE；以启动脚本启动五秒并确认进程存活，随后只结束该验证进程。
