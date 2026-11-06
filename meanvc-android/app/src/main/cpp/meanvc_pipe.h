// meanvc_pipe.h — shared all-ONNX MeanVC2 40ms pipeline, host-agnostic.
//
// Consumers: meanvc_file.cpp (file-mode parity regression), meanvc_rt.cpp
// (Windows realtime shell), and the upcoming Android port (same class behind
// JNI; only the audio I/O shell differs). Semantics mirror run_rt.py's
// process_chunk with the 40ms geometry:
//   640-sample (40 ms @16k) feeds -> kaldi fbank graph (wave carry host-side)
//   -> FastU2++ fp32 graph (11-frame windows, stride 8, 2 BN/window, att/cnn
//   caches) -> per-window BN interpolation ([carry]+2 BN -> 9 pts, drop carry
//   dup -> 8 cond frames) -> DiT fp32 graph (2-step ODE, flat KV, 4-frame
//   noise cache) -> mel [1,80,4] -> vocos (+2-frame cache) -> istft graph ->
//   overlap stitch (last 160 kept, crossfade next head).
// feed() takes EXACTLY CHUNK samples (the callers' resampler hands out
// CHUNK-sized blocks or nothing — skip semantics, never zero-fill).
#pragma once

#include "onnxruntime_cxx_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace mvc {

// ---------------------------------------------------------------------------
// minimal WAV I/O (RIFF): 16-bit PCM or 32-bit float, mono/stereo (mixdown),
// 32-bit float out in [-1,1]
// ---------------------------------------------------------------------------
inline bool read_wav16(const char* path, std::vector<float>& out, int& sr) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return false;
    }
    bool have_fmt = false;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    long data_pos = -1; uint32_t data_len = 0;
    char chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t sz; memcpy(&sz, chunk + 4, 4);
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t fmtb[16];
            if (fread(fmtb, 1, 16, f) != 16) { fclose(f); return false; }
            memcpy(&fmt, fmtb, 2); memcpy(&ch, fmtb + 2, 2);
            memcpy(&rate, fmtb + 4, 4); memcpy(&bits, fmtb + 14, 2);
            have_fmt = true;
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (!memcmp(chunk, "data", 4)) {
            data_pos = ftell(f); data_len = sz;
            fseek(f, (long)sz, SEEK_CUR);
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    bool ok_fmt = have_fmt && data_pos >= 0 && (ch == 1 || ch == 2) &&
                  ((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32));
    if (!ok_fmt) { fclose(f); return false; }
    sr = rate;
    fseek(f, data_pos, SEEK_SET);
    size_t bytes_per = (size_t)(bits / 8) * ch;
    size_t frames = data_len / bytes_per;
    out.resize(frames);
    if (fmt == 1) {
        std::vector<int16_t> pcm(frames * ch);
        if (fread(pcm.data(), 2, pcm.size(), f) != pcm.size()) { fclose(f); return false; }
        for (size_t i = 0; i < frames; i++) {
            float s = 0;
            for (int c = 0; c < ch; c++) s += pcm[i * ch + c] / 32768.0f;
            out[i] = s / ch;
        }
    } else {
        std::vector<float> pcm(frames * ch);
        if (fread(pcm.data(), 4, pcm.size(), f) != pcm.size()) { fclose(f); return false; }
        for (size_t i = 0; i < frames; i++) {
            float s = 0;
            for (int c = 0; c < ch; c++) s += pcm[i * ch + c];
            out[i] = s / ch;
        }
    }
    fclose(f);
    return true;
}

inline bool write_wav_f32(const char* path, const std::vector<float>& x, int sr) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t data_len = (uint32_t)(x.size() * 4);
    uint32_t riff_len = 36 + data_len + (data_len & 1);
    fwrite("RIFF", 1, 4, f); fwrite(&riff_len, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); uint32_t fmt_sz = 16; fwrite(&fmt_sz, 4, 1, f);
    uint16_t audio_fmt = 3, ch = 1; fwrite(&audio_fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    uint32_t byte_rate = (uint32_t)sr * 4; fwrite(&byte_rate, 4, 1, f);
    uint16_t align = 4; fwrite(&align, 2, 1, f);
    uint16_t bits = 32; fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_len, 4, 1, f);
    fwrite(x.data(), 4, x.size(), f);
    if (data_len & 1) fputc(0, f);
    fclose(f);
    return true;
}

inline std::vector<float> read_bin(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<float> v(sz / 4);
    if (fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
    fclose(f);
    return v;
}

// numpy v1.0 writer for a 1-D float32 array (speaker cards; load_spk reads
// them back, but the file must be valid for numpy too). Written to
// path+".tmp", flushed, closed, THEN renamed over the target — a crash
// mid-write must never leave a truncated card that later loads as garbage.
inline bool write_npy_f32(const std::string& path, const float* v, size_t n) {
    char dict[96];
    int len = snprintf(dict, sizeof dict,
        "{'descr': '<f4', 'fortran_order': False, 'shape': (%zu,), }", n);
    if (len <= 0 || len >= (int)sizeof dict) return false;
    int pad = 64 - ((10 + len) % 64);          // spaces + '\n' end the header
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite("\x93NUMPY\x01\x00", 1, 8, f);
    uint16_t hl = (uint16_t)(len + pad);
    fwrite(&hl, 2, 1, f);
    fwrite(dict, 1, len, f);
    for (int i = 0; i < pad - 1; i++) fputc(' ', f);
    fputc('\n', f);
    if (fwrite(v, 4, n, f) != n) { fclose(f); remove(tmp.c_str()); return false; }
    fflush(f);
    fclose(f);
#ifdef _WIN32
    remove(path.c_str());                      // rename() fails if target exists
#endif
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        fprintf(stderr, "ERROR: rename %s -> %s failed\n", tmp.c_str(), path.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ORT plumbing
// ---------------------------------------------------------------------------
struct Graph {
    std::string name;
    std::string path;      // UTF-8/ASCII; widened only on Windows (ORTCHAR_T)
    std::vector<std::string> in_names, out_names;
    std::vector<const char*> in_names_c, out_names_c;
    Ort::Session sess{nullptr};
};

inline Ort::MemoryInfo cpu_mem() {
    return Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
}

// widen an ASCII/UTF-8 path for the Windows ORTCHAR_T API (model paths are
// ASCII on every platform we ship)
inline std::wstring to_w(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

inline void load_graph(Ort::Env& env, Graph& g, int threads = 1) {
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(threads);
#ifdef _WIN32
    std::wstring wpath = to_w(g.path);   // ORTCHAR_T = wchar_t on Windows,
    g.sess = Ort::Session(env, wpath.c_str(), so);  // char elsewhere
#else
    g.sess = Ort::Session(env, g.path.c_str(), so);
#endif
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < g.sess.GetInputCount(); i++) {
        auto p = g.sess.GetInputNameAllocated(i, alloc);
        g.in_names.emplace_back(p.get());
    }
    for (size_t i = 0; i < g.sess.GetOutputCount(); i++) {
        auto p = g.sess.GetOutputNameAllocated(i, alloc);
        g.out_names.emplace_back(p.get());
    }
    for (auto& s : g.in_names) g.in_names_c.push_back(s.c_str());
    for (auto& s : g.out_names) g.out_names_c.push_back(s.c_str());
}

// CreateTensor wraps the CALLER's storage without copying: every buffer passed
// here MUST be a named variable that outlives the Run() call. Brace
// temporaries dangle (bit us twice: cache_val garbage, then t/r ODE scalars).
inline Ort::Value tensor_f(const std::vector<float>& v,
                           const std::vector<int64_t>& shape) {
    static Ort::MemoryInfo mem = cpu_mem();
    return Ort::Value::CreateTensor<float>(
        mem, const_cast<float*>(v.data()), v.size(),
        const_cast<int64_t*>(shape.data()), shape.size());
}

inline Ort::Value tensor_i64(const std::vector<int64_t>& v,
                             const std::vector<int64_t>& shape) {
    static Ort::MemoryInfo mem = cpu_mem();
    return Ort::Value::CreateTensor<int64_t>(
        mem, const_cast<int64_t*>(v.data()), v.size(),
        const_cast<int64_t*>(shape.data()), shape.size());
}

inline std::vector<Ort::Value> run_graph(Graph& g, std::vector<Ort::Value>& ins) {
    try {
        return g.sess.Run(Ort::RunOptions{nullptr}, g.in_names_c.data(),
                          ins.data(), ins.size(), g.out_names_c.data(),
                          g.out_names_c.size());
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[ORT FAIL] graph=%s: %s\n", g.name.c_str(), e.what());
        throw;
    }
}

// ---------------------------------------------------------------------------
// 40ms pipeline constants
// ---------------------------------------------------------------------------
constexpr size_t CHUNK = 640;      // 40 ms @ 16k — one feed
constexpr size_t VC_CHUNK = 8;     // cond frames per VC step
constexpr size_t BLOCK = 4;        // mel frames emitted per VC step
constexpr size_t WIN = 11;         // ASR window (frames)
constexpr size_t STRIDE = 8;       // ASR window stride
constexpr int RCS = 4;             // ASR cache size (windows)
constexpr int N_HOP = 160;         // vocos hop @16k
constexpr int64_t KV_ELEMS = 4 * 1 * 2 * 12 * 64;

// ---------------------------------------------------------------------------
// streaming sinc resampler (port of run_rt._StreamResampler): renders tail +
// new input through the ONNX conv and delivers only the output frames whose
// kernel fits inside the rendered window, tracked by ABSOLUTE output counters
// — delivered rate == input_rate * ratio for ANY push size, and every frame
// is identical to whole-stream rendering (both zero-pad the origin alike).
// ---------------------------------------------------------------------------
struct StreamResampler {
    Graph* g = nullptr;
    int in_step = 1;        // down: 3 (deliveries must sit on the input grid;
                            // non-multiples are stashed for the next push)
    int ctx_in = 0;         // input-domain context carried between renders
    int margin_out = 0;     // untrusted kernel-edge frames, output domain
    bool up = false;        // true: 16k->48k (ratio 3), false: 48k->16k
    std::vector<float> rem_in, tail, carry;
    long long total_in = 0, next_out = 0;

    void configure(Graph& graph, bool upsample) {
        g = &graph;
        up = upsample;
        in_step = upsample ? 1 : 3;
        // run_rt.py: downsample ctx 768/margin 8, upsample ctx 256/margin 320
        ctx_in = upsample ? 256 : 768;
        margin_out = upsample ? 320 : 8;
        rem_in.clear(); tail.clear(); carry.clear();
        total_in = 0; next_out = 0;
    }

    void push(const float* p, size_t n) {
        std::vector<float> new_in(p, p + n);
        if (in_step > 1 && !rem_in.empty()) {
            new_in.insert(new_in.begin(), rem_in.begin(), rem_in.end());
        }
        rem_in.clear();
        size_t use = (new_in.size() / in_step) * in_step;
        if (use) {
            rem_in.assign(new_in.begin() + use, new_in.end());
            new_in.resize(use);
        } else {
            rem_in = std::move(new_in);
            return;
        }
        long long base = total_in - (long long)tail.size();
        total_in += (long long)new_in.size();
        std::vector<float> x = tail;
        x.insert(x.end(), new_in.begin(), new_in.end());
        std::vector<Ort::Value> ins;
        ins.push_back(tensor_f(x, {1, 1, (int64_t)x.size()}));
        auto outs = run_graph(*g, ins);
        const float* y = outs[0].GetTensorData<float>();
        long long ylen = (long long)
            outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        long long out_base = up ? base * 3 : base / 3;
        long long j_start = std::max<long long>(
            base > 0 ? margin_out : 0, next_out - out_base);
        long long j_end = ylen - margin_out;
        if (j_end > j_start) {
            carry.insert(carry.end(), y + j_start, y + j_end);
            next_out = out_base + j_end;
        }
        if ((long long)x.size() > ctx_in)
            x.erase(x.begin(), x.end() - ctx_in);
        tail = std::move(x);
    }

    bool take(std::vector<float>& out, size_t n) {
        if (carry.size() < n) return false;
        out.assign(carry.begin(), carry.begin() + n);
        carry.erase(carry.begin(), carry.begin() + n);
        return true;
    }

    std::vector<float> take_all() {
        std::vector<float> out = std::move(carry);
        carry.clear();
        return out;
    }
};

// ---------------------------------------------------------------------------
// the pipeline
// ---------------------------------------------------------------------------
struct Pipe {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "meanvc"};
    std::vector<Graph> graphs;   // 0 fbank, 1 asr, 2 dit, 3 vocos, 4 istft,
                                 // 5 resample down, 6 resample up
    int ort_threads = 1;

    // host state (mirrors run_rt.py process_chunk); sized in load_sessions()
    std::vector<float> wave_tail;                 // EMPTY at start — zero-pad
                                                  // shifts the fbank grid
    std::vector<float> frame_cache;               // fbank frames, 80 each
    std::vector<float> bn_buffer;                 // interpolated cond frames
    std::vector<float> att;                       // 6*4*RCS*128
    std::vector<float> cnn;                       // 6*1*256*8
    int64_t asr_offset = 4;
    std::vector<float> kv_k;                      // [4,1,2,12,64]
    std::vector<float> kv_v;
    int64_t vc_offset = 0;
    std::vector<float> voc_cache;                 // 160 = [mel-bin][2]
    bool last_wav_set = false;
    std::vector<float> last_wav;                  // N_HOP
    std::vector<float> noise_cache;               // 4*80
    bool noise_cache_set = false;
    bool enc_carry_set = false;
    std::vector<float> enc_carry;                 // 256

    // noise: --noise-file (parity runs) or seeded mt19937 (realtime)
    std::vector<float> noise;
    size_t noise_pos = 0;
    std::mt19937 rng{12345u};
    std::normal_distribution<float> ndist{0.0f, 1.0f};

    // parity / regression dumps (both hosts write the same layouts)
    std::vector<float> cond_dump;   // VC_CHUNK*256 per step
    std::vector<float> xs_dump;     // 8*80 per step (post-ODE)

    // ---------------- lifecycle ------------------------------------------
    // model_dir: "onnx_models" (Windows file runs, relative to cwd) or an
    // absolute filesDir path on Android. Paths are ASCII everywhere we ship.
    void load_sessions(const std::string& model_dir = "onnx_models") {
        models_dir_ = model_dir;
        att.assign(6 * 4 * RCS * 128, 0.f);
        cnn.assign(6 * 1 * 256 * 8, 0.f);
        kv_k.assign((size_t)KV_ELEMS, 0.f);
        kv_v.assign((size_t)KV_ELEMS, 0.f);
        voc_cache.assign(2 * 80, 0.f);
        last_wav.assign(N_HOP, 0.f);
        noise_cache.assign(4 * 80, 0.f);
        enc_carry.assign(256, 0.f);
        spks_.assign(256, 0.f);
        // capacity 8 up front: load_spk_encoder() resize()s to 8 later, and a
        // realloc would dangle the Graph&/Graph* handed out to StreamResampler
        graphs.reserve(8);
        graphs.resize(7);
        graphs[0] = {"fbank", model_dir + "/kaldi_fbank.onnx"};
        graphs[1] = {"asr", model_dir + "/asr_80ms_fp32.onnx"};
        graphs[2] = {"dit", model_dir + "/dit_40ms_fp32.onnx"};
        graphs[3] = {"vocos", model_dir + "/vocos_fp32.onnx"};
        graphs[4] = {"istft", model_dir + "/vocos_istft_t6.onnx"};
        graphs[5] = {"resdown", model_dir + "/resample_48k16k.onnx"};
        graphs[6] = {"resup", model_dir + "/resample_16k48k.onnx"};
        for (auto& g : graphs) load_graph(env, g, ort_threads);
    }

    // Strict numpy reader for voice cards: v1–v3 header, float32 ('<f4' or
    // '=f4'; '>f4' big-endian is REJECTED, not byte-swapped), C order, shape
    // exactly (256,), ≥256 floats of payload (trailing pad tolerated). The
    // header dict is parsed by key — key order and quote style vary between
    // writers (numpy, torch, write_npy_f32) — never by offset. Anything that
    // isn't a valid 256-dim f32 card (e.g. a wav renamed .npy) fails here.
    bool load_spk(const std::string& spk_path) {
        FILE* f = fopen(spk_path.c_str(), "rb");
        if (!f) {
            fprintf(stderr, "ERROR: cannot open spk %s\n", spk_path.c_str());
            return false;
        }
        auto fail = [&](const std::string& why) {
            fclose(f);
            fprintf(stderr, "ERROR: spk npy invalid (%s): %s\n",
                    why.c_str(), spk_path.c_str());
            return false;
        };
        uint8_t magic[8];
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "\x93NUMPY", 6) != 0)
            return fail("bad magic");
        uint8_t major = magic[6];
        if (major < 1 || major > 3) return fail("unsupported npy version");
        uint32_t hl;
        if (major == 1) {                          // v1: 2-byte header length
            uint8_t b2[2];
            if (fread(b2, 1, 2, f) != 2) return fail("truncated header");
            hl = b2[0] | ((uint32_t)b2[1] << 8);
        } else {                                   // v2/v3: 4-byte header length
            uint8_t b4[4];
            if (fread(b4, 1, 4, f) != 4) return fail("truncated header");
            hl = b4[0] | ((uint32_t)b4[1] << 8) | ((uint32_t)b4[2] << 16) |
                 ((uint32_t)b4[3] << 24);
        }
        if (hl == 0 || hl > (1u << 20)) return fail("bad header length");
        std::string dict(hl, '\0');
        if (fread(&dict[0], 1, hl, f) != hl) return fail("truncated header");
        auto val_of = [&](const char* key) -> std::string {
            size_t p = dict.find(std::string("'") + key + "'");
            if (p == std::string::npos)
                p = dict.find(std::string("\"") + key + "\"");
            if (p == std::string::npos) return "";
            p = dict.find(':', p);
            if (p == std::string::npos) return "";
            ++p;
            while (p < dict.size() && (dict[p] == ' ' || dict[p] == '\t')) ++p;
            if (p >= dict.size()) return "";
            char q = dict[p];
            if (q == '\'' || q == '"') {           // quoted scalar (descr)
                size_t e = dict.find(q, p + 1);
                return e == std::string::npos ? ""
                                              : dict.substr(p + 1, e - p - 1);
            }
            std::string v;
            if (dict[p] == '(') {                  // shape tuple: to matching ')'
                size_t e = dict.find(')', p);
                if (e == std::string::npos) return "";
                v = dict.substr(p, e - p + 1);
            } else {                               // scalar: to the separator
                size_t e = p;
                while (e < dict.size() && dict[e] != ',' && dict[e] != '}' &&
                       dict[e] != '\n')
                    ++e;
                v = dict.substr(p, e - p);
            }
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
            return v;
        };
        std::string descr = val_of("descr");
        if (descr != "<f4" && descr != "=f4")
            return fail("descr '" + descr + "' is not float32");
        if (val_of("fortran_order") != "False") return fail("fortran_order");
        std::string shape = val_of("shape"), tight;
        for (char ch : shape)
            if (ch != ' ' && ch != '\t') tight += ch;
        if (tight != "(256,)")
            return fail("shape " + shape + " is not (256,)");
        std::vector<float> spks(256, 0.f);
        if (fread(spks.data(), 4, 256, f) != 256)
            return fail("payload smaller than 256 floats");
        fclose(f);
        // spks stays a named member feeding the DiT tensor
        spks_ = std::move(spks);
        return true;
    }

    // on-device cloning: load the 334 MB Q4 speaker encoder on first use.
    // One-shot job, never concurrent with the RT worker (UI enforces) — give
    // it its own thread pool instead of the 1-thread streaming setting.
    void load_spk_encoder() {
        if (graphs.size() > 7) return;             // already loaded
        graphs.resize(8);                          // storage reserved in load_sessions
        graphs[7] = {"spk", models_dir_ + "/spk_encoder_q4.onnx"};
        load_graph(env, graphs[7], ort_threads < 4 ? 4 : ort_threads);
    }

    // 现场制卡: wav (16k/48k, mono/stereo, 16-bit PCM or 32-bit float) ->
    // 256-dim speaker card at out_npy. Returns "" on success, else an error
    // message for the UI. Uses the 48k->16k graph for resampling; the encoder
    // graph layer-norms internally, so plain [-1,1] samples are the input.
    std::string clone_card(const std::string& wav_path, const std::string& out_npy) {
        std::vector<float> wav;
        int sr = 0;
        if (!read_wav16(wav_path.c_str(), wav, sr))
            return "无法读取wav(支持16/32位PCM,单声道或立体声)";
        if (sr == 48000) {
            StreamResampler r;
            r.configure(graphs[5], false);
            r.push(wav.data(), wav.size());
            wav = r.take_all();
            sr = 16000;
        } else if (sr != 16000) {
            return "采样率仅支持16000或48000";
        }
        if (wav.size() < (size_t)sr) return "音频太短(至少1秒,建议5-10秒)";
        load_spk_encoder();
        std::vector<Ort::Value> ins;
        ins.push_back(tensor_f(wav, {1, (int64_t)wav.size()}));
        auto outs = run_graph(graphs[7], ins);
        if (outs.empty() ||
            outs[0].GetTensorTypeAndShapeInfo().GetElementCount() != 256)
            return "编码器输出异常";
        if (!write_npy_f32(out_npy, outs[0].GetTensorData<float>(), 256))
            return "写入音色卡失败";
        return "";
    }

    void set_noise_file(const std::string& path) {
        if (!path.empty()) noise = read_bin(path.c_str());
    }

    void reset() {
        wave_tail.clear();
        frame_cache.clear();
        bn_buffer.clear();
        std::fill(att.begin(), att.end(), 0.f);
        std::fill(cnn.begin(), cnn.end(), 0.f);
        asr_offset = 4;
        std::fill(kv_k.begin(), kv_k.end(), 0.f);
        std::fill(kv_v.begin(), kv_v.end(), 0.f);
        vc_offset = 0;
        std::fill(voc_cache.begin(), voc_cache.end(), 0.f);
        last_wav_set = false;
        std::fill(noise_cache.begin(), noise_cache.end(), 0.f);
        noise_cache_set = false;
        enc_carry_set = false;
        noise_pos = 0;
        rng.seed(12345u);
        cond_dump.clear();
        xs_dump.clear();
    }

    void warmup(int n = 3) {
        std::vector<float> z(CHUNK, 0.f);
        for (int i = 0; i < n; i++) feed(z);
        reset();
    }

    // ---------------- one 40ms block --------------------------------------
    // Exactly CHUNK 16k samples in; returns converted audio produced by this
    // call (0 / 640 / 1280 samples — often empty while the ASR window fills).
    std::vector<float> feed(const std::vector<float>& block) {
        if (block.size() != CHUNK)
            fprintf(stderr, "feed(): expected %zu samples, got %zu\n",
                    CHUNK, block.size());
        // ---------------- fbank graph on carry + chunk --------------------
        std::vector<float> padded = wave_tail;
        padded.insert(padded.end(), block.begin(), block.end());
        int n_frames = (int)((padded.size() - 400) / 160) + 1;
        wave_tail.assign(padded.begin() + 160 * n_frames, padded.end());
        std::vector<Ort::Value> fin;
        fin.push_back(tensor_f(padded, {1, (int64_t)padded.size()}));
        auto outs = run_graph(graphs[0], fin);
        const float* fb = outs[0].GetTensorData<float>();
        size_t fb_frames =
            outs[0].GetTensorTypeAndShapeInfo().GetElementCount() / 80;

        // ---------------- ASR windows (per-window BN emit) ----------------
        // Offset periodic reset (run_rt.py:288): the FastU2++ positional
        // table is finite (crash: 4-by-6 broadcast at offset ~5000, storm
        // block 2501). Attention relative distances don't involve the raw
        // offset, so rewinding by 4000 (never below the cache span) is free.
        if (asr_offset >= 4000) {
            int64_t cache_t = att.empty() ? 0 : RCS;   // att[6,4,RCS,128]
            asr_offset = std::max<int64_t>(cache_t, asr_offset - 4000);
        }
        std::vector<float> frames_all = frame_cache;
        frames_all.insert(frames_all.end(), fb, fb + fb_frames * 80);
        size_t n_avail = frames_all.size() / 80;
        size_t i = 0;
        while (i + WIN <= n_avail) {
            std::vector<float> w(frames_all.begin() + i * 80,
                                 frames_all.begin() + (i + WIN) * 80);
            auto in_t = tensor_f(w, {1, (int64_t)WIN, 80});
            std::vector<int64_t> off_v(1, asr_offset);   // MUST outlive Run
            std::vector<int64_t> cs_v(1, RCS);
            auto off = tensor_i64(off_v, {});
            auto cs = tensor_i64(cs_v, {});
            auto at = tensor_f(att, {6, 4, RCS, 128});
            auto cn = tensor_f(cnn, {6, 1, 256, 8});
            std::vector<Ort::Value> ins;
            ins.push_back(std::move(in_t)); ins.push_back(std::move(off));
            ins.push_back(std::move(cs)); ins.push_back(std::move(at));
            ins.push_back(std::move(cn));
            auto aouts = run_graph(graphs[1], ins);
            const float* bn = aouts[0].GetTensorData<float>();
            att.assign(aouts[1].GetTensorData<float>(),
                       aouts[1].GetTensorData<float>() + att.size());
            cnn.assign(aouts[2].GetTensorData<float>(),
                       aouts[2].GetTensorData<float>() + cnn.size());
            asr_offset += 4;

            // BN interpolation: [carry frame] + 2 raw BN -> 4x linear
            // (align_corners) -> 9 pts -> drop the duplicated carry point —
            // exactly run_rt's per-window vc_chunk=8 emit block
            size_t len_inp = (enc_carry_set ? 1 : 0) + 2;
            std::vector<float> inp;
            if (enc_carry_set) inp = enc_carry;
            inp.insert(inp.end(), bn, bn + 2 * 256);
            const int N_OUT = 9;
            std::vector<float> block8;
            block8.reserve((N_OUT - 1) * 256);
            for (int j = 1; j < N_OUT; j++) {          // drop carry duplicate
                double p = j * (double)(len_inp - 1) / (N_OUT - 1);
                int lo = (int)p; if (lo > (int)len_inp - 2) lo = (int)len_inp - 2;
                double wgt = p - lo;
                for (int c = 0; c < 256; c++)
                    block8.push_back(inp[lo * 256 + c] * (float)(1 - wgt) +
                                     inp[(lo + 1) * 256 + c] * (float)wgt);
            }
            enc_carry.assign(bn + 256, bn + 2 * 256);
            enc_carry_set = true;
            bn_buffer.insert(bn_buffer.end(), block8.begin(), block8.end());
            i += STRIDE;
        }
        frame_cache.assign(frames_all.begin() + i * 80, frames_all.end());

        // ---------------- VC steps (2 ODE iterations each) ----------------
        std::vector<float> produced;
        while (bn_buffer.size() / 256 >= VC_CHUNK) {
            std::vector<float> cond(bn_buffer.begin(),
                                    bn_buffer.begin() + VC_CHUNK * 256);
            cond_dump.insert(cond_dump.end(), cond.begin(), cond.end());
            bn_buffer.erase(bn_buffer.begin(), bn_buffer.begin() + BLOCK * 256);

            std::vector<float> x(8 * 80, 0.f);
            if (noise_pos + 640 <= noise.size()) {
                memcpy(x.data(), noise.data() + noise_pos, 640 * sizeof(float));
                noise_pos += 640;
            } else {
                for (auto& v : x) v = ndist(rng);
            }
            if (noise_cache_set)
                memcpy(x.data(), noise_cache.data(), 4 * 80 * sizeof(float));
            memcpy(noise_cache.data(), x.data() + 4 * 80,
                   4 * 80 * sizeof(float));
            noise_cache_set = true;

            for (int step = 0; step < 2; step++) {
                float t_v = 1.0f - step * 0.5f;
                float r_v = std::max(0.0f, t_v - 0.5f);
                std::vector<float> t_buf(1, t_v), r_buf(1, r_v);  // outlive Run
                auto in_t = tensor_f(x, {1, 8, 80});
                auto t_t = tensor_f(t_buf, {1});
                auto r_t = tensor_f(r_buf, {1});
                auto c_t = tensor_f(cond, {1, 8, 256});
                auto s_t = tensor_f(spks_, {1, 256});
                std::vector<int64_t> dit_off_v(1, vc_offset);
                auto o_t = tensor_i64(dit_off_v, {});
                auto kk = tensor_f(kv_k, {4, 1, 2, 12, 64});
                auto vv = tensor_f(kv_v, {4, 1, 2, 12, 64});
                std::vector<Ort::Value> dins;
                dins.push_back(std::move(in_t)); dins.push_back(std::move(t_t));
                dins.push_back(std::move(r_t)); dins.push_back(std::move(c_t));
                dins.push_back(std::move(s_t)); dins.push_back(std::move(o_t));
                dins.push_back(std::move(kk)); dins.push_back(std::move(vv));
                auto douts = run_graph(graphs[2], dins);
                const float* u = douts[0].GetTensorData<float>();
                for (size_t j = 0; j < x.size(); j++) x[j] -= 0.5f * u[j];
                kv_k.assign(douts[1].GetTensorData<float>(),
                            douts[1].GetTensorData<float>() + kv_k.size());
                kv_v.assign(douts[2].GetTensorData<float>(),
                            douts[2].GetTensorData<float>() + kv_v.size());
            }
            vc_offset += BLOCK;
            xs_dump.insert(xs_dump.end(), x.begin(), x.end());  // post-ODE

            // mel [4][80] (t-major) -> [80][4] (mel-major)
            std::vector<float> mel(80 * BLOCK);
            for (int t = 0; t < BLOCK; t++)
                for (int m = 0; m < 80; m++)
                    mel[m * BLOCK + t] = x[t * 80 + m];

            // vocoder stitch: 2-frame cache + 4 new = 6 -> real/imag -> istft
            std::vector<float> mel6(80 * (2 + BLOCK));
            for (int m = 0; m < 80; m++) {
                mel6[m * 6 + 0] = voc_cache[m * 2 + 0];
                mel6[m * 6 + 1] = voc_cache[m * 2 + 1];
                mel6[m * 6 + 2] = mel[m * 4 + 0];
                mel6[m * 6 + 3] = mel[m * 4 + 1];
                mel6[m * 6 + 4] = mel[m * 4 + 2];
                mel6[m * 6 + 5] = mel[m * 4 + 3];
            }
            // voc_cache is [80,2] mel-major (= mel6[:, :, -2:]); that slice is
            // NOT contiguous in mel-major mel6 — copy per-m (memcpy scrambled
            // it and cost a parity hunt)
            for (int m = 0; m < 80; m++) {
                voc_cache[m * 2 + 0] = mel6[m * 6 + 4];
                voc_cache[m * 2 + 1] = mel6[m * 6 + 5];
            }
            for (auto& v : mel6) v = (v + 1.0f) / 2.0f;

            std::vector<Ort::Value> min_v;
            min_v.push_back(tensor_f(mel6, {1, 80, 2 + BLOCK}));
            auto vouts = run_graph(graphs[3], min_v);
            size_t n_spec =
                vouts[0].GetTensorTypeAndShapeInfo().GetElementCount();
            std::vector<float> re(vouts[0].GetTensorData<float>(),
                                  vouts[0].GetTensorData<float>() + n_spec);
            std::vector<float> im(vouts[1].GetTensorData<float>(),
                                  vouts[1].GetTensorData<float>() + n_spec);
            size_t frames6 = n_spec / 321;
            auto rt = tensor_f(re, {1, 321, (int64_t)frames6});
            auto it = tensor_f(im, {1, 321, (int64_t)frames6});
            std::vector<Ort::Value> iins;
            iins.push_back(std::move(rt)); iins.push_back(std::move(it));
            auto iouts = run_graph(graphs[4], iins);
            const float* wav6 = iouts[0].GetTensorData<float>();
            const size_t wav6_len = (2 + BLOCK - 1) * N_HOP;   // 800 @16k

            if (!last_wav_set) {
                produced.assign(wav6, wav6 + wav6_len - N_HOP);
            } else {
                produced.reserve(produced.size() + CHUNK);
                for (int k = 0; k < N_HOP; k++) {
                    float d = 1.0f - k / (float)(N_HOP - 1);
                    float uu = k / (float)(N_HOP - 1);
                    produced.push_back(last_wav[k] * d + wav6[k] * uu);
                }
                produced.insert(produced.end(), wav6 + N_HOP,
                                wav6 + wav6_len - N_HOP);
            }
            last_wav.assign(wav6 + wav6_len - N_HOP, wav6 + wav6_len);
            last_wav_set = true;
        }
        return produced;
    }

    std::vector<float> spks_;   // 256, loaded from the npy card
    std::string models_dir_;    // remembered for lazy spk-encoder loading
};

}  // namespace mvc
