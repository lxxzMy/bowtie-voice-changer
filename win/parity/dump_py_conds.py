# dump_py_conds.py — dump the cond tensors the official Python 120ms runtime
# feeds to the DiT, for bit-parity against the C++ port (mvcExportCondDump).
# Cond frames depend only on fbank->ASR->interp (deterministic), not on the
# generative noise, so this is a sound equivalence gate.
import sys
import numpy as np
import torch

sys.path.insert(0, r"E:\Projects\tts\mean2vc\MeanVC2\runtime")
import run_rt  # noqa: E402

MODEL = sys.argv[1] if len(sys.argv) > 1 else "120ms"
WAV = sys.argv[2] if len(sys.argv) > 2 else r"E:\Projects\tts\mean2vc\MeanVC2\test_audio\source_en_1.wav"
OUT = sys.argv[3] if len(sys.argv) > 3 else r"E:\Projects\tts\mean2vc\win\smoke\py_conds.f32"

runner = run_rt.VCRunner(target_wav=r"E:\Projects\tts\mean2vc\MeanVC2\test_audio\reference_1.wav",
                         device="cpu", model=MODEL, threads=2, backend="onnx")

conds = []
orig_step = runner._vc_step

def step_probe(cond):
    conds.append(cond.detach().cpu().numpy().astype(np.float32).copy())
    return orig_step(cond)

runner._vc_step = step_probe

wav_out = OUT.replace("py_conds.f32", "py_out.wav")
runner.process_file(WAV, wav_out, seed=42, out_sr=16000)

allc = np.concatenate([c.reshape(-1) for c in conds]).astype(np.float32)
allc.tofile(OUT)
print(f"steps={len(conds)} cond_frames={conds[0].shape[1]} floats={allc.size} -> {OUT}")
