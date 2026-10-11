# NOTICE / 声明

## 蝴蝶结变声器 (Bowtie Voice Changer)

Copyright 2026

本应用基于 ASLP-lab 的开源项目
[MeanVC2](https://github.com/ASLP-lab/MeanVC2)(Apache License 2.0,
http://www.apache.org/licenses/LICENSE-2.0)构建。
修改内容:流式推理运行时移植至 C++/Kotlin(ONNX Runtime),
训练Checkpoint 导出为 ONNX 格式,并新增 Android 实时界面。

依 Apache License 2.0 再分发本应用时,须保留本声明及
MeanVC2 的版权与免责声明。

## 模型致谢 / Model Acknowledgements

本应用使用的模型与运行时来自以下开源项目(经 MeanVC2 致谢):

- [F5-TTS](https://github.com/SWivid/F5-TTS) — DiT-based CFM backbone
- [Vocos](https://github.com/gemelo-ai/vocos) — Neural vocoder
- [WavLM](https://github.com/microsoft/unilm/tree/master/wavlm) — Speaker encoder upstream
- [ECAPA-TDNN](https://github.com/lawlict/ECAPA-TDNN) — Speaker embedding model
- [WeNet](https://github.com/wenet-e2e/wenet) — ASR encoder for BN extraction
- [s3prl](https://github.com/s3prl/s3prl) — Self-supervised speech models
- [FunASR](https://github.com/modelscope/FunASR) — ASR for CER evaluation
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)(MIT)— 推理运行时

## 使用声明 / Disclaimer

克隆任何个人的音色前,必须获得其明确同意。严禁将本应用用于
仿冒、欺诈或生成误导性内容;使用者须对其用途的合法性与
正当性承担全部责任。

Voice cloning requires the explicit consent of the person whose
voice is converted. Do not use this application for impersonation,
fraud, or creating misleading audio content. Users are solely
responsible for their use cases.
