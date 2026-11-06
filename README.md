# 蝴蝶结变声器（mean2vc）

零样本音色克隆 + 实时变声。同一 C++ 核心（`meanvc_pipe.h`，MeanVC2 引擎）跑在 Android 与 Windows 上；F0-free 架构（无变调旋钮，音色随参考音频）。

## 目录地图

| 目录 | 内容 | 说明 |
|---|---|---|
| `meanvc-android/` | **Android 版 App**（v5.10 蝴蝶结 UI） | Gradle 工程；JNI 壳 `app/src/main/cpp/meanvc_jni.cpp` |
| `win/` | **Windows 版 App**（M1） | 见下"Windows 版" |
| `MeanVC2/` | 上游引擎仓库（独立 git，模型/ORT/测试基准所在） | 内部不要重组；`runtime_cpp/meanvc_pipe.h` 是双端共享核心 |
| `tools/android/` | Android 一次性工具（图标生成、MainActivity 修复） | 历史脚本，保留复用可能 |
| `assets-src/` | UI 素材源（蝴蝶结底图 + 生成 prompt） | 入库 |
| `experiments/` | 引擎判决实验（各带独立 venv/checkpoints，不入库） | `conan-verdict/` = M0 Conan 判决（2026-10，结论：中文口音不达标，英文正常；细节见项目记忆） |
| `attic/` | 历史补丁脚本、调试残留、旧备份 | 只进不出；不再增长 |
| `android_artifacts/` | onnxruntime Android AAR 备份 | jniLibs 的 so 来源 |

## Windows 版（win/）

```
win/
├── engine/        mvc_engine.cpp → mvc_engine.dll（C ABI：Init/Start/Stop/Status/SetCard/CloneCard/EnumDevices/IsCableInstalled/RunSim）
│   └── build.cmd  zig c++ 构建（工具链在 toolchain/，链 onnxruntime_imp.lib）
├── app/           WinUI3 单窗口表单（unpackaged self-contained x64；依赖已落本地源 nuget_local/）
├── smoke/         P/Invoke 冒烟 + 文件环回（与 meanvc_rt --sim 基线 bit-exact）
├── installer/     Inno + redist/VBCABLE_Setup_x64.exe（新机器无虚拟声卡时兜底）
├── toolchain/     便携 zig 0.13.0（gitignore）
└── nuget_local/   NuGet 中国 CDN 闭包（fetch_closure.py 可重建；nupkg gitignore）
```

构建：`win/engine/build.cmd` → `cd win/app && dotnet build -c Release` → 运行 `win/app/bin/Release/net8.0-windows10.0.19041.0/win-x64/BowtieVC.exe`（dev 模式自动回退用 `MeanVC2/onnx_models` 和 `MeanVC2/test_audio/reference_1.spk.npy`）。

虚拟声卡：检测 Voicemeeter（用户机）或 VB-Cable（兜底）。输出选 `Voicemeeter Input`，Voicemeeter 里把 VAIO 条 **B1** 点亮，Discord 麦克风选 `Voicemeeter Out B1`。

## 性能基线（不要在这些数字以下回归）

- 引擎稳态发射延迟：**174ms**（40ms 块 / pre-roll 40ms / 1 线程，`MeanVC2/rt_sim.csv`）
- 单 40ms 块计算预算：≤40ms（1 线程紧循环 ~20.6ms）
- 回归门限：`MeanVC2/verify_40ms_cadence.py`（G1-G9）
- 文件环回 parity：DLL sim 输出 vs `meanvc_rt --sim` 对齐 40ms 后 cos=1.000000

## 约定

- `MeanVC2/`、`experiments/`、`attic/`、`android_artifacts/`、`win/toolchain/`、`win/nuget_local/*.nupkg` 不入 git。
- Windows 版 UI 参数只放引擎真实存在的（pre-roll、线程数）；不造"变调/混响"这类引擎没有的旋钮。
- 文案中的延迟一律用实测数，不用论文/宣传口径。
