# C++ Qt Widgets App

Chinese setup guide: [README.zh-CN.md](README.zh-CN.md). Dependency/version alignment guide: [requirements.md](requirements.md).

This directory contains the first-stage C++ Qt6 Widgets rewrite of the aircraft RF workflow desktop app.

## Toolchain

- Qt 6.x, Widgets module
- CMake 3.21+
- MSVC 2022 on Windows

Do not use the Anaconda Qt 5.15 qmake kit for this app. Configure CMake with the Qt6 installation path and the VS 2022 Build Tools environment. A working command line on this machine is:

```powershell
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S D:\cqt_platform\aircraft_project\cpp_app -B D:\cqt_platform\aircraft_project\cpp_app\build-ninja -G Ninja -DCMAKE_MAKE_PROGRAM=D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe -DCMAKE_PREFIX_PATH=D:/Qt/6.6.1/msvc2019_64"
cmd /c "call D:\VS\BuildTools2022\Common7\Tools\VsDevCmd.bat -arch=amd64 -host_arch=amd64 && D:\VS\BuildTools2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build D:\cqt_platform\aircraft_project\cpp_app\build-ninja"
```

## Structure

- `src/app`: repository and runtime path helpers
- `src/core`: shared workflow record structs and JSON serialization
- `src/services`: CAP probe, app state JSON store, Python preprocessing bridge
- `src/ui`: Qt Widgets main window, pages, theme, and reusable widgets
- `tools/preprocess_runner.py`: stable JSON request/response bridge to the existing Python preprocessing script
- `tests`: Qt Test service tests

## Runtime Data

The C++ app keeps using the repository runtime directories:

- `data/state/app_state.json`
- `data/samples/`
- `data/models/`
- `data/exports/`

The existing PySide6 app is intentionally left in place as a reference and rollback path.

## Runtime Notes

- The Qt runtime must be on `PATH` when launching binaries from the command line:

```powershell
$env:PATH = "D:\Qt\6.6.1\msvc2019_64\bin;$env:PATH"
```

- `cpp_app/build*/` is ignored by git.
