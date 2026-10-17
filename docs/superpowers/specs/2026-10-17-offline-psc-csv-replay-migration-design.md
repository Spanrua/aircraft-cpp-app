# 离线 PSC CSV 回放最小迁移设计

## 目标

将已在 `cpp_app_demo_sweep` 验证的离线 PSC CSV 导入与按帧回放能力，最小化迁移到 `cpp_app` 当前的全流程分段融合版本。构建产物为 `build-fullflow-release-segment-fusion`。

## 范围与边界

- 保留目标版本已有的扫频图、分段融合、识别和硬件采集实现。
- 仅迁移 `CeyearPscSweep` 的 CSV 帧读取、`SweepPage` 的导入和定时回放控制，以及相关 Qt Test。
- 不读取以外的方式改动任何原始采集 CSV；不改动已有构建产物、运行时 DLL 或其他全流程源文件。
- 不在离线回放中启动 VISA、UDP 或任何硬件扫描命令。

## 设计

### CSV 解析

`loadCeyearPscFramesCsv()` 读取 3943B 导出的 PSC CSV，并按 `frame_index` 聚合为 `CeyearPscFrame` 序列。

- 必填字段：`frame_index`、`bin_index`、`frequency_hz`、`level_dbuv`。
- CSV 缺列、非数值或负帧/频点索引会报出明确行号并中止导入。
- `frequency_hz <= 0` 的记录视为设备导出的无效哨兵 bin，跳过该记录；例如 `frequency_hz=0, level_dbuv=200.0` 不导致整个文件导入失败。

### 回放

`SweepPage` 增加“导入并回放 CSV”入口。用户选择文件后，页面读取帧序列、根据有效频点配置当前显示范围、清空旧瀑布图，并以 80 ms 间隔逐帧调用既有 `appendPscFrame()`。

- 回放开始、进度、完成和停止均显示“离线演示回放”状态。
- 停止按钮停止定时器并清除回放状态，不影响已有实时扫频的停止逻辑。
- 回放进行时禁止重复启动实时 PSC 或另一份离线回放。

### 与目标版本的关系

每帧仍通过目标版本已有的 `appendPscFrame()` 进入其扫频可视化与后续全流程处理链，因此不替换分段融合或图形绘制路径。

## 测试与验收

- 服务测试：正常多帧 CSV、缺列/无效数值报错、零频哨兵 bin 跳过。
- 界面测试：导入按钮存在、测试帧能够按定时器完成回放并更新状态。
- 构建：仅构建 `build-fullflow-release-segment-fusion` 中的应用与测试目标。
- 验证：运行该构建目录的 CTest 测试集；不修改或覆盖源 CSV。

## 迁移文件

- `src/services/CeyearPscSweep.h`
- `src/services/CeyearPscSweep.cpp`
- `src/ui/pages/SweepPage.h`
- `src/ui/pages/SweepPage.cpp`
- `tests/service_tests.cpp`
- `tests/ui_tests.cpp`
