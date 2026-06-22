# 航空射频识别 Qt6/C++ 桌面端

本目录是航空射频工作流桌面端的 Qt6/C++ 迁移版本，当前重点覆盖采集、预处理、数据集、识别和训练等页面骨架，并接入 USRP B210 采集与 CAP 文件探测能力。

英文简介见 [README.md](README.md)，版本和依赖对齐见 [requirements.md](requirements.md)。

## 当前定位

- 主程序：Qt6 Widgets 桌面应用，入口目标为 `aircraft_qt`。
- 核心语言：C++17。
- 主要构建方式：CMake + Ninja + MSVC 2022。
- 当前 USRP 采集对象：USRP B210，天线接 RF-A 的 `TX/RX` 口时，前端天线参数填写 `TX/RX`，通道一般为 `0`。
- 当前保存格式：`.cap` 文件，IQ 载荷为 int16 交织 I/Q 数据。
- 旧 Python/PySide6 程序保留为参考，不作为本仓库的主开发目标。

## 目录结构

- `src/app`：项目路径、运行目录辅助函数。
- `src/core`：工作流记录结构和 JSON 序列化。
- `src/services`：CAP 探测、状态存储、USRP 采集、预处理进程桥接等服务。
- `src/ui`：Qt Widgets 主窗口、页面、主题和复用控件。
- `tools/preprocess_runner.py`：Qt 程序调用 Python 预处理算法的 JSON 请求/响应桥。
- `tests`：Qt Test 测试。
- `data`：运行期状态、样本、模型、导出目录。

## 环境要求

建议同事优先按下面版本对齐：

- Windows 10/11 x64。
- Visual Studio Build Tools 2022，使用 MSVC x64 工具链。
- CMake 3.21 或更高。
- Ninja。
- Qt 6.6.1 `msvc2019_64`，至少需要 `Core`、`Gui`、`Widgets`，测试需要 `Test`。
- UHD SDK，默认查找 `D:/uhd`，可通过 `-DUHD_ROOT=...` 指定。
- Boost 头文件，当前 CMake 会依次查找 `BOOST_ROOT`、`CONDA_PREFIX`、`E:/anaconda/Library/include`、`C:/local/boost` 和系统默认路径。

详细说明见 [requirements.md](requirements.md)。

## 构建

以下命令以当前机器路径为例。如果同事安装路径不同，只替换 VS、Qt、UHD 和源码路径即可。

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-release -G Ninja -DCMAKE_MAKE_PROGRAM=D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe -DCMAKE_PREFIX_PATH=D:/Qt/6.6.1/msvc2019_64 -DUHD_ROOT=D:/uhd"
```

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build D:\cqt_platform\aircraft_project\cpp_app\build-release --config Release --target aircraft_qt aircraft_ui_tests"
```

## 运行

从命令行直接运行时，需要保证 Qt 的 `bin` 目录在 `PATH` 中：

```powershell
$env:PATH = "D:\Qt\6.6.1\msvc2019_64\bin;$env:PATH"
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
```

如果要把 Qt 运行库部署到 exe 旁边：

```powershell
D:\Qt\6.6.1\msvc2019_64\bin\windeployqt.exe D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_qt.exe
```

## 测试

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-release\aircraft_ui_tests.exe
```

也可以通过 CTest 运行已配置的测试目标：

```powershell
ctest --test-dir D:\cqt_platform\aircraft_project\cpp_app\build-release --output-on-failure
```

## 运行期数据

程序会继续使用仓库内的运行目录：

- `data/state/app_state.json`
- `data/raw/`
- `data/samples/`
- `data/models/`
- `data/exports/`

模型权重不随本仓库提交。前端默认让用户选择权重路径，团队内建议统一放在 `data/models/` 下，并在交接文档中记录具体文件名。

## USRP B210 采集说明

当前 C++ 服务通过 UHD API 创建 USRP 并写出 CAP 文件。主要参数：

- 设备参数：空字符串表示 UHD 自动发现；也可以填写类似 `serial=xxxx`。
- 天线：RF-A 的 TX/RX 口填写 `TX/RX`。
- 通道：B210 单通道采集通常用 `0`。
- 中心频率：单位 Hz，按任务频段填写。
- 采样率：单位 Hz，例如 `12800000` 表示 12.8 MSps。
- 带宽：单位 Hz。
- 增益：单位 dB。
- 采集时长：单位 s。
- CPU 格式：默认 `sc16`。
- Wire 格式：默认 `sc16`。

当前 `.cap` 写入规则：

- 文件头长度 `0x200` 字节。
- 头部 `0x0010` 写入带宽，CAP 探测时按带宽推导采样率。
- 头部 `0x0018` 写入中心频率。
- 头部 `0x0110` 写入样本点数量。
- IQ 载荷为大端 int16，按 I/Q/I/Q 交织写入。

## 预处理说明

Qt 程序通过 `tools/preprocess_runner.py` 调用 Python 侧算法。该 runner 当前会在父级项目中查找：

```text
preprocess_integration/reprocess_classify_model1_0420.py
```

也就是说，独立 `cpp_app` 仓库包含 C++ UI 与桥接脚本，但没有内置完整 Python 预处理算法文件和模型权重。同事需要从项目交接包或父级工程同步 `preprocess_integration`、模型权重，以及 Python 环境依赖。

Python 依赖版本可参考 [requirements-python.txt](requirements-python.txt)。

## 常见问题

- 不要用 Anaconda 自带 Qt 5.15 的 qmake Kit 构建本项目，本项目目标是 Qt6/CMake。
- 如果提示找不到 UHD，检查 `-DUHD_ROOT` 是否指向 UHD SDK 根目录。
- 如果提示找不到 Boost，检查 `BOOST_ROOT`、`CONDA_PREFIX` 或 `BOOST_INCLUDE_DIR`。
- 如果启动时报 Qt platform plugin 错误，优先检查 Qt `bin` 目录是否在 `PATH`，或运行 `windeployqt`。
- USRP 出现 overflow 时，优先降低采样率、缩短采集时长、确认 USB 3.0 连接、关闭占用磁盘/CPU 的程序。
