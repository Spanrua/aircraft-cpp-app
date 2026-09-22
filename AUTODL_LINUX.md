# AutoDL Linux 部署说明

当前仓库已经包含完整 C++ Qt 源码工程，不需要上传 Windows 发布包来运行 Linux 版。

## 1. 上传范围

推荐上传本次生成的 `autodl_linux_source_*.zip`。如果手动上传，目录关系必须保持如下：

```text
<upload-root>/
├── aircraft_project/
│   ├── requirements.txt
│   ├── preprocess_integration/
│   └── cpp_app/
│       ├── cpp_app.pro
│       ├── CMakeLists.txt
│       ├── src/
│       ├── tests/
│       ├── tools/
│       ├── scripts/
│       └── requirements-python.txt
└── dji_training_backend/
    ├── training_backend.py
    ├── recognition_backend.py
    └── CNNTRANS_v7_rfaug_sampler.py
```

只上传源码和脚本即可。不要上传 `build-*`、`dist/`、Windows DLL、实验原始数据或 Python 虚拟环境。

## 2. AutoDL 无硬件模式

该模式可以在没有 UHD、NI-VISA 和 3943B 仪器的容器中编译并启动 Qt GUI。USRP 和 3943B 页面仍会显示，但实际硬件操作会返回“当前构建未启用”提示。

```bash
cd /root/autodl-tmp/aircraft_project/cpp_app

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libgl1-mesa-dev \
  libxkbcommon-x11-0 libxcb-xinerama0 libxcb-cursor0

# 使用系统 Qt 6，或将 Qt 安装目录写入 CMAKE_PREFIX_PATH
sudo apt-get install -y qt6-base-dev qt6-base-dev-tools

bash scripts/build_linux.sh
```

如果使用自定义 Qt 6：

```bash
export CMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64
export QT_ROOT=/path/to/Qt/6.x.x/gcc_64
bash scripts/build_linux.sh
```

## 3. Python 预处理/训练/识别

建议单独创建 Linux 虚拟环境，避免把 AutoDL 系统 Python 污染：

```bash
python3 -m venv /root/autodl-tmp/venvs/aircraft
source /root/autodl-tmp/venvs/aircraft/bin/activate
python -m pip install -U pip
python -m pip install -r ../requirements.txt
python -m pip install -r requirements-python.txt
```

如需 GPU，请根据 AutoDL 镜像中的 CUDA/PyTorch 版本安装匹配的 PyTorch，而不是盲目使用固定版本。

启动：

```bash
cd /root/autodl-tmp/aircraft_project/cpp_app
export AIRCRAFT_PREPROCESS_PYTHON=/root/autodl-tmp/venvs/aircraft/bin/python
export AIRCRAFT_TRAINING_PYTHON=/root/autodl-tmp/venvs/aircraft/bin/python
bash scripts/start_linux.sh
```

## 4. 有 UHD 时启用 USRP

AutoDL 容器需要先安装 Linux UHD 开发包和对应设备驱动。确认以下文件存在后再启用：

```bash
export UHD_ROOT=/path/to/uhd
test -f "$UHD_ROOT/include/uhd/usrp/multi_usrp.hpp"
test -f "$UHD_ROOT/lib/libuhd.so" || test -f "$UHD_ROOT/lib64/libuhd.so"
```

然后重新构建：

```bash
export AIRCRAFT_ENABLE_UHD=ON
bash scripts/build_linux.sh
```

NI-VISA 不是普通 Linux AutoDL 镜像的默认组件。只有确认已经安装 Linux VISA 兼容实现并拥有 `visa.h` 与 `libvisa.so` 时，才设置：

```bash
export AIRCRAFT_ENABLE_NI_VISA=ON
export NI_VISA_INCLUDE_DIR=/path/to/include
export NI_VISA_LIBRARY=/path/to/libvisa.so
bash scripts/build_linux.sh
```

## 5. 图形界面访问

AutoDL 通常是无桌面服务器。需要使用桌面转发、VNC、Xpra 或容器平台提供的 GUI 入口，并正确设置 `DISPLAY`。仅做算法处理时不需要启动 Qt GUI，直接使用 `preprocess_integration` 下的 Python 脚本即可。

## 6. 当前边界

- Windows `.exe`、`.dll`、`platforms/qwindows.dll` 不能在 Linux 直接使用。
- AutoDL 无仪器时可以运行 GUI、预处理、训练和识别流程，但不能真实采集 USRP 或控制 3943B。
- 真实硬件功能需要 Linux 版 UHD/设备驱动和可用 VISA 实现，并且还需要容器能访问设备或仪器网络。
- CMake 默认使用 `AIRCRAFT_ENABLE_UHD=OFF`、`AIRCRAFT_ENABLE_NI_VISA=OFF`，因此没有硬件 SDK 也可以编译 GUI。
