# 版本与依赖对齐说明

本文用于团队成员对齐 C++ Qt 迁移版的构建、运行、USRP 采集和 Python 预处理桥接环境。

## C++/Qt 构建环境

| 项目 | 推荐版本/路径 | 说明 |
| --- | --- | --- |
| 操作系统 | Windows 10/11 x64 | 当前主要在 Windows 上验证 |
| 编译器 | Visual Studio Build Tools 2022 / MSVC x64 | 使用 VS 2022 开发者环境 |
| C++ 标准 | C++17 | 由 `CMakeLists.txt` 固定 |
| CMake | 3.21+ | 项目最低版本要求 |
| 生成器 | Ninja | 当前命令行示例使用 Ninja |
| Qt | Qt 6.6.1 `msvc2019_64` | 需要 `Core`、`Gui`、`Widgets`；测试需要 `Test` |
| UHD SDK | 默认 `D:/uhd` | 可通过 `-DUHD_ROOT=...` 覆盖 |
| Boost | 头文件即可 | UHD C++ 头文件依赖 Boost 头文件 |

当前不要使用 Anaconda Qt 5.15 的 qmake Kit。该项目是 Qt6/CMake 工程。

## CMake 变量

| 变量 | 示例值 | 用途 |
| --- | --- | --- |
| `CMAKE_PREFIX_PATH` | `D:/Qt/6.6.1/msvc2019_64` | 指向 Qt6 安装目录 |
| `UHD_ROOT` | `D:/uhd` | 指向 UHD SDK 根目录 |
| `BOOST_INCLUDE_DIR` | `E:/anaconda/Library/include` | 可手动指定 Boost 头文件目录 |
| `BUILD_TESTING` | `ON` | 构建 Qt Test 测试目标 |

Boost 自动查找顺序包括：

- `$ENV{BOOST_ROOT}/include`
- `$ENV{CONDA_PREFIX}/Library/include`
- `E:/anaconda/Library/include`
- `C:/local/boost`
- 系统默认头文件路径

## 推荐构建命令

配置：

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-release -G Ninja -DCMAKE_MAKE_PROGRAM=D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe -DCMAKE_PREFIX_PATH=D:/Qt/6.6.1/msvc2019_64 -DUHD_ROOT=D:/uhd"
```

构建：

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build D:\cqt_platform\aircraft_project\cpp_app\build-release --config Release --target aircraft_qt aircraft_ui_tests"
```

测试：

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe
```

## Python 预处理环境

Qt 主程序不是 pip 项目，但预处理桥接脚本会调用 Python 算法。独立 `cpp_app` 仓库当前只包含：

```text
tools/preprocess_runner.py
```

该脚本会向父级工程查找：

```text
preprocess_integration/reprocess_classify_model1_0420.py
```

因此同事运行完整预处理流程时，需要额外同步：

- `preprocess_integration/`
- 模型权重文件，例如团队约定放入 `data/models/`
- Python 依赖，见 [requirements-python.txt](requirements-python.txt)

父级项目当前 Python 依赖口径：

| 包 | 版本 |
| --- | --- |
| numpy | 1.26.4 |
| scipy | 1.12.0 |
| torch | 2.2.0 |
| torchvision | 0.17.0 |
| opencv-python | 4.11.0.86 |
| scikit-learn | 1.4.0 |
| xgboost | 2.0.3 |
| onnxruntime | 1.17.0 |
| PyVISA | 1.14.1 |

如果只运行 C++ UI 和 USRP 采集，不需要安装 Python 依赖。

## USRP B210 参数口径

| 参数 | 推荐/默认 | 说明 |
| --- | --- | --- |
| 设备参数 | 空字符串或 `serial=xxxx` | 空字符串表示 UHD 自动发现 |
| 天线 | `TX/RX` | 对应 RF-A 的 TX/RX 口 |
| 通道 | `0` | B210 单通道常用通道 0 |
| 中心频率 | 任务指定，单位 Hz | 例如 `2400000000` |
| 采样率 | `12800000`，单位 Hz | 根据带宽和 USB 吞吐调整 |
| 带宽 | `10000000`，单位 Hz | 写入 CAP 头部并用于后续解析 |
| 增益 | `30`，单位 dB | 按现场信号强度调整 |
| 采集时长 | 单位 s | 溢出时建议先缩短 |
| CPU 格式 | `sc16` | 当前写 CAP 的代码按 int16 IQ 处理 |
| Wire 格式 | `sc16` | 降低 USB 带宽压力 |
| 输出格式 | `.cap` | 当前固定为 CAP 文件 |

USRP 流格式说明：

- `cpuFormat` 是 UHD 交给主机内存的样本格式。
- `wireFormat` 是 USRP 和主机之间通过 USB 传输的样本格式。
- 当前代码默认并推荐 `sc16/sc16`，即有符号 16 位复数 IQ。保存到 CAP 时也是 int16 I/Q 交织。

## CAP 文件格式口径

当前 C++ 写入和探测 CAP 的规则如下：

| 偏移 | 含义 | 类型 |
| --- | --- | --- |
| `0x0000` | 文件头标记 | 当前写入 `CAP0` |
| `0x0010` | 带宽 Hz | big-endian double |
| `0x0018` | 中心频率 Hz | big-endian double |
| `0x0110` | 样本点数量 | big-endian uint32 |
| `0x0200` 起 | IQ 数据 | big-endian int16 I/Q 交织 |

CAP 探测服务当前会按带宽推导采样率：`sampleRateHz = bandwidthHz * 1.28`。

## 运行期目录约定

| 路径 | 用途 |
| --- | --- |
| `data/state/app_state.json` | 应用状态 |
| `data/raw/` | 原始采集文件 |
| `data/samples/` | 预处理输出样本 |
| `data/models/` | 模型权重 |
| `data/exports/` | 导出结果 |

`build*/` 目录不提交到 Git。
