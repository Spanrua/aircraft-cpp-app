# 航空射频识别 Qt/C++ 桌面端

本目录是航空射频工作流桌面端的 Qt/C++ 迁移版本。当前源码以 Qt 5.12.10 为最低兼容基线，并兼容 Qt 5.12.10 以上 Qt5 版本和 Qt6 升级版本；主要覆盖采集、预处理、数据集、识别和训练等页面骨架，并接入 USRP B210 采集与 CAP 文件探测能力。

英文简介见 [README.md](README.md)，版本和依赖对齐见 [requirements.md](requirements.md)。

## 当前定位

- 主程序：Qt Widgets 桌面应用，入口目标为 `aircraft_qt`。
- 核心语言：C++17。
- 推荐构建方式：CMake + Ninja + MSVC 2022。
- 兼容工程文件：仓库根目录提供 `cpp_app.pro`，用于 Qt 5.12.10+ 的 qmake 工程导入和团队检查。
- 当前 USRP 采集对象：USRP B210，天线接 RF-A 的 `TX/RX` 口时，前端天线参数填写 `TX/RX`，通道一般为 `0`。
- 当前保存格式：`.cap` 文件，IQ 载荷为 int16 交织 I/Q 数据。
- 旧 Python/PySide6 程序保留为参考，不作为本仓库的主开发目标。

## 目录结构

- `src/app`：项目路径、运行目录辅助函数和 Qt 版本兼容薄封装。
- `src/core`：工作流记录结构和 JSON 序列化。
- `src/services`：CAP 探测、状态存储、USRP 采集、预处理进程桥接等服务。
- `src/ui`：Qt Widgets 主窗口、页面、主题和复用控件。
- `tools/preprocess_runner.py`：Qt 程序调用 Python 预处理算法的 JSON 请求/响应桥。
- `tests`：Qt Test 测试。
- `data`：运行期状态、样本、模型、导出目录。

## 环境要求

建议同事优先按下面版本对齐：

- Windows 10/11 x64；源码可按匹配工具链构建 32 位或 64 位版本。
- Ubuntu 20.04 或更高版本；需安装对应发行版可用的 Qt 5.12.10+ / Qt6、CMake、编译器和 UHD。
- Visual Studio Build Tools 2022，Windows 下使用 MSVC x64 或 x86 工具链。
- CMake 3.21 或更高。
- Ninja。
- Qt 5.12.10+ 或 Qt 6.x，至少需要 `Core`、`Gui`、`Widgets`，测试需要 `Test`。
- UHD SDK，默认查找 `D:/uhd`，可通过 `-DUHD_ROOT=...` 指定。
- Boost 头文件，当前 CMake 会依次查找 `BOOST_ROOT`、`CONDA_PREFIX`、`E:/anaconda/Library/include`、`C:/local/boost` 和系统默认路径。
- NI-VISA Runtime，用于 3943B VISA 通信；目标机要使用仪表通信时仍需安装匹配驱动。

详细说明见 [requirements.md](requirements.md)。

## 构建

以下命令以当前机器的 Qt 5.12.10 x64 路径为例。如果同事安装路径不同，只替换 VS、Qt、UHD 和源码路径即可。

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-qt5-release-package -G Ninja -DCMAKE_MAKE_PROGRAM=D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe -DCMAKE_PREFIX_PATH=D:/Qt/5.12.10/5.12.10/msvc2017_64 -DUHD_ROOT=D:/uhd -DCMAKE_BUILD_TYPE=Release"
```

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build D:\cqt_platform\aircraft_project\cpp_app\build-qt5-release-package --target aircraft_qt aircraft_ui_tests"
```

如需使用 Qt6，只替换 `CMAKE_PREFIX_PATH` 为 Qt6 安装路径，例如 `D:/Qt/6.6.1/msvc2019_64`。

## qmake 工程

仓库根目录提供 `cpp_app.pro`，用于 Qt Creator 以 qmake 方式导入 Qt 5.12.10+ 工程。默认值：

- `UHD_ROOT = D:/uhd`
- `BOOST_INCLUDE_DIR = E:/anaconda/Library/include`
- Windows x64 使用 NI-VISA Win64 路径，x86 使用 NI-VISA Win32 路径。

如果本机路径不同，在 Qt Creator 的 Build Environment 或 qmake 参数中覆盖这些变量。

## 运行

从命令行直接运行构建目录程序时，需要保证 Qt 的 `bin` 目录在 `PATH` 中：

```powershell
$env:PATH = "D:\Qt\5.12.10\5.12.10\msvc2017_64\bin;$env:PATH"
D:\cqt_platform\aircraft_project\cpp_app\build-qt5-release-package\aircraft_qt.exe
```

如果要制作可拷贝的 Windows 便携包，使用 Qt 5.12.10 的 `windeployqt` 部署运行库：

```powershell
D:\Qt\5.12.10\5.12.10\msvc2017_64\bin\windeployqt.exe --release --compiler-runtime D:\cqt_platform\aircraft_project\cpp_app\dist\aircraft_qt_qt51210_portable\aircraft_qt.exe
```

便携包需要整体拷贝，不能只拷贝 `aircraft_qt.exe`。

## 测试

```powershell
D:\cqt_platform\aircraft_project\cpp_app\build-qt5-release-package\aircraft_ui_tests.exe
```

也可以通过 CTest 运行已配置的测试目标：

```powershell
ctest --test-dir D:\cqt_platform\aircraft_project\cpp_app\build-qt5-release-package --output-on-failure
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

- 不要用 Anaconda 自带 Qt 5.15 的 qmake Kit 构建本项目，当前最低兼容基线是 Qt 5.12.10。
- 如果提示找不到 UHD，检查 `-DUHD_ROOT` 是否指向 UHD SDK 根目录。
- 如果提示找不到 Boost，检查 `BOOST_ROOT`、`CONDA_PREFIX` 或 `BOOST_INCLUDE_DIR`。
- 如果启动时报 Qt platform plugin 错误，优先检查便携包内是否存在 `platforms/qwindows.dll`，或重新运行 `windeployqt`。
- USRP 出现 overflow 时，优先降低采样率、缩短采集时长、确认 USB 3.0 连接、关闭占用磁盘/CPU 的程序。

## 仓库治理

本目录是唯一的正式开发仓库。提交文件、修改打包脚本或处理运行资产前，请先阅读[仓库治理说明](docs/repository-governance.md)。
