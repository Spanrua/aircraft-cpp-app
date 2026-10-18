# PSC 相对底噪预警与布局修正 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 仅在宽带频段显著高于局部底噪 10 dB 时预警，并扩大候选信号表的可读空间。

**Architecture:** 检测器先用全帧最低 20% bin 的中位数定位候选连续段，再用候选段左右各 1 MHz、连续覆盖的带外样本中位数确认相对底噪；任一侧缺失、覆盖不足或内部断点超过正常 bin 间隔时回退全帧低 20% 中位数。后续分段、分类、实时/离线逻辑不变。SweepPage 仅调整既有水平布局和列宽，不改变数据路径。

**Tech Stack:** C++17、Qt 5.12 Widgets、Qt Test。

---

### Task 1: 局部底噪占用判定

**Files:**
- Modify: `src/services/PscWidebandAlertDetector.h`
- Modify: `src/services/PscWidebandAlertDetector.cpp`
- Modify: `tests/service_tests.cpp`

- [ ] **Step 1: 添加失败服务测试**

构造 5 kHz 步进、-80 dBuV 底噪的 10 MHz 带：带内 -70.1 dBuV 时不分类，带内 -70.0 dBuV 时分类为 `10M`。再构造单侧邻域不足的帧，验证最低 20% bin 中位数回退。

- [ ] **Step 2: 实现带外中位底噪确认**

新增 `RelativeNoiseRiseDb = 10.0`、`SidebandNoiseWindowHz = 1000000.0`。先以全帧低 20% 中位数初筛占用 bin 并形成候选段；候选段左右带外各 1 MHz 都有样本时使用两侧样本的中位数，否则使用全帧低 20% 中位数。仅峰值 `peakDbuv >= baseline + 10.0` 的候选段进入既有分类。

- [ ] **Step 3: 构建并运行定向测试**

构建 `aircraft_service_tests`，通过临时 Qt/VISA PATH 运行新增函数与现有 `pscWidebandAlert*` 相关测试。

- [ ] **Step 4: 提交**

```powershell
git add src/services/PscWidebandAlertDetector.h src/services/PscWidebandAlertDetector.cpp tests/service_tests.cpp
git commit -m "feat: detect PSC bands above local noise"
```

### Task 2: 扫频页布局与回归

**Files:**
- Modify: `src/ui/pages/SweepPage.cpp`
- Modify: `tests/ui_tests.cpp`

- [ ] **Step 1: 添加失败 UI 测试**

验证候选表状态列宽度至少能完整显示 `告警·40M`，并验证外层三列布局中左侧扫频配置组宽度小于右侧候选组宽度。

- [ ] **Step 2: 实现布局修正**

在 SweepPage 的水平外层布局中将左/中/右 stretch 调整为紧凑左列、扩展中列、可读右列；候选表设置状态列最小宽度和合适的水平 header resize mode，避免文字裁切。

- [ ] **Step 3: 构建并验证**

构建 `aircraft_qt` 和 UI 测试目标；若 UI 测试仍被既有 RecognitionPage 链接错误阻塞，记录该阻塞。检查 `git diff --check`。

- [ ] **Step 4: 提交并部署 Release 包**

提交 SweepPage/UI 测试；重建 `build-psc-wideband-alert-release/aircraft_qt.exe`，在程序关闭时覆盖发布包并以启动脚本短暂验证。
