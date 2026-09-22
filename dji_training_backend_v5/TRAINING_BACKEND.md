# Qt 训练后端接入

这个目录提供一个可以直接放进 Qt 项目的 Python 训练后端。闭集训练使用
`CNNTRANS_v5_3943B.py`，默认配置对齐当前实验最好的长窗口方案：

```text
model_arch       = axis_profile_xlarge
stitch_windows   = 8
stitch_stride    = 4
feature_mode     = complex_signed_mag
classifier_head  = hierarchical
internal_dev     = 0.20
selection_only   = true
```

`stitch_windows=8` 表示把同一 NPZ/source 内连续 8 个 4096 点窗口拼成
32768 点输入；`stitch_stride=4` 是当前最佳实验的滑动步长。训练页若需要
严格单窗口，设置 `stitch_windows=1`，同时通常设置 `stitch_stride=1`。

## 数据与预检

训练页传入一个数据集版本目录。目录中可以直接有 `manifest.jsonl`，也可以在
子目录中有多份 manifest。每一行至少需要 `path`、`label` 和窗口索引字段
（优先 `npz_index`、`local_index`、`sample_index`、`window_index`、
`segment_index`）。

`training_backend.audit_manifest()` 会在启动训练前检查：

- JSONL 语法和对象类型；
- manifest path 是否能定位到唯一 NPZ；
- NPZ 是否包含 `iq`；
- `iq` 是否为 `(N, 2, 4096)`；
- 窗口索引是否在 `[0, N)`；
- 重复的 `(NPZ, window_index)` 记录。

预检失败时不会启动 GPU 训练。命令行预检：

```bash
python training_backend.py \
  --dataset-version /path/to/dataset-version \
  --output-dir /path/to/runs/run-001 \
  --check-only
```

## Qt 启动、进度和取消

推荐在 `QThread` 或 Qt 的后台 worker 中调用 `start_training()`，保存返回的
`TrainingProcess` 对象。它持有真实的 `subprocess.Popen`，因此取消按钮可以
调用 `job.cancel()`，该方法会终止整个 process group（包括 DataLoader worker）。

```python
from training_backend import TrainingConfig, start_training

config = TrainingConfig(
    dataset_version="/data/versions/v001",
    output_dir="/data/runs/2026-09-17-001",
    stitch_windows=8,       # 设为 1 即严格单窗口
    stitch_stride=4,
)

job = start_training(config, on_event=handle_event, python_executable="/opt/venv/bin/python")
# Cancel button: job.cancel()
result = job.wait()
```

stdout 每行都是 JSON 对象。v5 训练脚本在 `--json-events` 下直接发出：

- `started`：实际命令、manifest 统计和输出目录；
- `dataset`：train/selection/official window 数和模型配置；
- `epoch`：完整 epoch 指标，包括 loss、accuracy、macro-F1、best epoch；
- `result`：最终评估范围和最终/最佳准确率；
- `completed` / `failed`：进程退出码和 `run_summary.json`；
- `log`：无法结构化的原始文本，可直接显示在 Qt 日志窗口。

Qt 不需要解析 tqdm 或人类可读的 `Epoch ... summary` 文本；这些文本仍会保留，
用于人工排查。

## 输出目录

每次任务使用独立目录，例如 `runs/2026-09-17-001/`。主要文件：

```text
training_request.json       Qt 提交的配置和预检报告
run_config.json              v5 实际训练配置、设备和 split audit
history.json                 每个 epoch 的指标
best_model.pth               最佳模型权重
best_checkpoint.pth          最佳模型及选择信息
last_checkpoint.pth          可恢复训练 checkpoint
final_model.pth              最后一个 epoch 的权重
run_summary.json             Qt 结果页读取的摘要
internal_dev_predictions.npz  selection-only 模式的预测明细
classification_report.*     非 selection-only 模式的报告
confusion_matrix.*           非 selection-only 模式的混淆矩阵
```

默认 `selection_only=true`，只使用 train/internal-dev 选择 checkpoint，避免
训练页误触 official validation。只有明确需要最终闭集确认时，才在
`extra_args` 中去掉 `--selection-only`。

## Python、CUDA 和 PyTorch 环境

`requirements-training.txt` 只列 Python 包名，不锁定 torch 版本，因为 PyTorch
必须匹配目标机的 CUDA 驱动。部署时：

```bash
python -m venv .venv
. .venv/bin/activate              # Windows: .venv\Scripts\activate
python -m pip install -U pip
python -m pip install -r requirements-training.txt
python CNNTRANS_v5_3943B.py --check-env
python -c "import torch; print(torch.__version__, torch.version.cuda, torch.cuda.is_available())"
```

若目标机有 NVIDIA GPU，请根据其驱动选择官方 CUDA 对应的 PyTorch wheel；不要
把当前机器的 `.venv` 或 `torch` 安装目录打进 Qt 包。Qt 侧应把目标解释器的
绝对路径传给 `python_executable`。

## 开集识别方案

开集不是把 `unknown` 和 `background` 混成一个类别。建议分为：

```text
known       已注册个体，例如 air2_01
unknown     有有效发射，但来自未注册个体
background  无目标发射或非目标信号
```

初版使用 [rf_fingerprint_id/open_set_baseline.py](/root/rf_fingerprint_id/open_set_baseline.py)。
它是独立的 8 类 registered-id baseline，不能直接加载闭集 16 类
`CNNTRANS_v5_3943B.py` 的 checkpoint。流程是先训练开集专用 checkpoint，再评估：

1. 固定 v5 的 `stitch8` 输入和 `axis_profile_xlarge` backbone；
2. 用 train registered sources 导出 embedding/logits；
3. 用 train 建立 source-balanced class prototypes；
4. 在 calibration split 上校准 max-softmax、energy、prototype-cosine 阈值；
5. 在 known_test/unknown_test 上报告 known acceptance、unknown rejection、
   false acceptance、AUROC、AUPR 和 FPR@95TPR；
6. 暂不使用 official validation 调阈值；有真实 unknown/background 校准数据后，
   再考虑 Mahalanobis、EVT 或 OpenMax。

开集命令示例：

```bash
python -m rf_fingerprint_id.open_set_baseline audit \
  --data-root /path/to/dataset-version \
  --output-dir /path/to/runs/open-set-audit \
  --stitch-windows 8

python -m rf_fingerprint_id.open_set_baseline train \
  --data-root /path/to/dataset-version \
  --output-dir /path/to/runs/open-set \
  --stitch-windows 8

python -m rf_fingerprint_id.open_set_baseline evaluate \
  --data-root /path/to/dataset-version \
  --output-dir /path/to/runs/open-set \
  --checkpoint /path/to/runs/open-set/seed_20260818/best_checkpoint.pth \
  --stitch-windows 8
```

`training_backend.build_open_set_command()` 可供 Qt 的“开集评估”按钮复用。
开集输出包括 `split_manifest.json`、`embeddings_fp16.npz`、`prototypes.npz`、
`window_predictions.csv`、`source_predictions.csv`、`metrics.json` 和 ROC 图。
