// mvc_engine.cpp — Windows C ABI voice-conversion engine DLL over the shared
// MeanVC2 pipeline (meanvc_pipe.h). Windows twin of the Android JNI shell
// (meanvc_jni.cpp), same dataflow:
//
//   WASAPI capture (shared mode, event-driven, any burst size) -> in_q
//   -> worker: res_down (48k->16k) -> SKIP (never zero-fill) until 640
//      -> Pipe::feed -> res_up (16k->48k) -> out_q
//   -> WASAPI render (shared mode, event-driven): pre-roll cushion,
//      2 ms seam fades at gaps, steady drain.
//
// Heavy work never runs on the audio threads. The UI (C#) is control-plane
// only: init/start/stop/status/card ops.
//
// Build (from win/engine, paths relative to MeanVC2/runtime_cpp):
//   zig c++ mvc_engine.cpp -shared -I ../../MeanVC2/runtime_cpp/ort_include ^
//       -L ../../MeanVC2/runtime_cpp -lonnxruntime -lwinmm -lole32 -luuid ^
//       -o mvc_engine.dll
#include "meanvc_pipe.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>      // emit definitions for PKEY_* under mingw headers
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <timeapi.h>
#include <functiondiscoverykeys_devpkey.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

using namespace mvc;

// ---------------------------------------------------------------------------
// error surfacing: every entry point must survive; last error is UTF-8
// ---------------------------------------------------------------------------
static std::mutex g_err_mu;
static std::string g_last_err;
static void set_err(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_err_mu);
    g_last_err = msg;
}

// ---------------------------------------------------------------------------
// bounded segment queue (same as meanvc_rt.cpp / meanvc_jni.cpp)
// ---------------------------------------------------------------------------
struct SegQ {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<float>> q;
    size_t maxq;
    bool closed = false;
    std::atomic<long> dropped{0};

    explicit SegQ(size_t mx) : maxq(mx) {}
    bool push(std::vector<float>&& v, bool drop_oldest) {
        std::lock_guard<std::mutex> lk(mu);
        if (closed) return false;
        if (q.size() >= maxq) {
            if (!drop_oldest) { dropped++; return false; }
            q.pop_front();
            dropped++;
        }
        q.push_back(std::move(v));
        cv.notify_one();
        return true;
    }
    bool pop(std::vector<float>& v, int wait_ms) {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                         [&] { return !q.empty() || closed; }))
            return false;
        if (q.empty()) return false;
        v = std::move(q.front());
        q.pop_front();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lk(mu);
        closed = true;
        cv.notify_all();
    }
};

// ---------------------------------------------------------------------------
// engine globals (one engine per process, guarded by lifecycle order — same
// contract as Android: init once; card switches and clones only while stopped)
// ---------------------------------------------------------------------------
static const int STREAM_SR = 48000;
static const int FADE_N = 96;                    // 2 ms @48k
// render/capture shared-mode buffer (hns, 100 ns units)
static const REFERENCE_TIME HNS_BUFFER = 300000; // 30 ms

static Pipe* g_pipe = nullptr;
static StreamResampler* res_down = nullptr;
static StreamResampler* res_up = nullptr;
static std::string g_spk_path;          // ACP; re-applied when the engine is
                                        // rebuilt on a mode switch
static std::string g_models_dir;
static double g_cap_period_ms = 0, g_ren_period_ms = 0, g_buf_ms = 0;
static std::atomic<bool> g_audio_running{false};

// process-wide counters (status readouts only)
static std::atomic<long long> g_nin16{0}, g_nout16{0}, g_played48{0};
static std::atomic<long long> g_gaps{0}, g_preroll{0};
static std::atomic<double> g_proc_ms{0.0}, g_feed_ms{0.0};

// The real-time core shared by the WASAPI session and the file-sim driver:
// queues + playback state. The worker and the playback filler below only
// touch RtCore, so both drivers exercise identical code.
struct RtCore {
    std::atomic<bool> run{false};
    SegQ* in_q = nullptr;                        // owned
    SegQ* out_q = nullptr;                       // owned
    std::vector<float> out_buffer;               // playback accumulation
    std::atomic<long long> buf48{0};
    std::atomic<bool> pre_roll{true};
    std::atomic<bool> was_gap{true};
    size_t prefill = 1920;                       // 40 ms @48k
    double pre_roll_ms = 40.0;
    std::atomic<float> in_gain{1.0f}, out_gain{1.0f};   // linear gains, live-adjustable
    std::chrono::steady_clock::time_point last_trim;             // latency governor
    std::vector<float> fade_in, fade_out;
    std::atomic<int> stream_err{0};              // last fatal error (0 = none)
};

// drain out_q into the playback accumulation (called from render thread / sim)
static void drain_out_q(RtCore* s) {
    std::vector<float> seg;
    while (s->out_q->pop(seg, 0))
        s->out_buffer.insert(s->out_buffer.end(), seg.begin(), seg.end());
    s->buf48 = (long long)s->out_buffer.size();
}

// fill up to `need` frames of output (pre-roll / steady / gap, with seam
// fades + output gain). Mirrors the Android out_cb. `dbg` = sim diagnostics.
static void playback_fill(RtCore* s, float* out, size_t need, const char* dbg = "") {
    drain_out_q(s);
    // latency governor: bursty emission lets out_buffer creep ABOVE the
    // pre-roll target and stay there (ratchet) — bleed it back at ≤1 ms per
    // 100 ms (1% time compression, imperceptible) once it exceeds target+20ms
    {
        const size_t target = s->prefill;
        const size_t slack = STREAM_SR / 50;                 // 20 ms tolerance
        auto now = std::chrono::steady_clock::now();
        if (s->out_buffer.size() > target + slack && !s->pre_roll.load() &&
            now - s->last_trim > std::chrono::milliseconds(100))
        {
            size_t excess = s->out_buffer.size() - (target + slack);
            size_t cut = std::min<size_t>(excess, STREAM_SR / 1000);  // ≤1 ms
            s->out_buffer.erase(s->out_buffer.begin(),
                                s->out_buffer.begin() + cut);
            s->last_trim = now;
        }
    }
    if (s->pre_roll.load()) {
        if (s->out_buffer.size() < s->prefill) {
            std::fill(out, out + need, 0.f);
            g_preroll++;
            return;
        }
        s->pre_roll = false;
    }
    if (s->out_buffer.size() >= need) {
        std::copy(s->out_buffer.begin(), s->out_buffer.begin() + need, out);
        s->out_buffer.erase(s->out_buffer.begin(), s->out_buffer.begin() + need);
        s->buf48 = (long long)s->out_buffer.size();
        if (s->was_gap.load()) {
            for (size_t k = 0; k < s->fade_in.size() && k < need; k++)
                out[k] *= s->fade_in[k];
            s->was_gap = false;
        }
        const float og = s->out_gain.load();
        if (og != 1.0f)
            for (size_t k = 0; k < need; k++) out[k] *= og;
        g_played48 += (long long)need;
    } else {
        fprintf(stderr, "[gap] %s buf=%.0fms outq_dropped=%ld\n", dbg,
                s->out_buffer.size() / (STREAM_SR / 1000.0),
                s->out_q->dropped.load());
        fflush(stderr);
        size_t n_avail = s->out_buffer.size();
        if (n_avail) {
            std::copy(s->out_buffer.begin(), s->out_buffer.end(), out);
            if (n_avail >= s->fade_out.size())
                for (size_t k = 0; k < s->fade_out.size(); k++)
                    out[n_avail - s->fade_out.size() + k] *= s->fade_out[k];
            const float og2 = s->out_gain.load();
            if (og2 != 1.0f)
                for (size_t k = 0; k < n_avail; k++) out[k] *= og2;
            g_played48 += (long long)n_avail;
        }
        std::fill(out + n_avail, out + need, 0.f);
        s->out_buffer.clear();
        s->buf48 = 0;
        s->was_gap = true;
        g_gaps++;
    }
}

// worker thread — identical shape to meanvc_jni.cpp worker_loop
static void worker_loop(RtCore* s) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const size_t chunk = g_pipe->cfg.chunk;      // 640 (40ms) / 2560 (160ms)
    std::vector<float> s16(chunk);
    for (;;) {
        std::vector<float> blk;
        if (!s->in_q->pop(blk, 200)) {
            if (s->in_q->closed || !s->run.load()) return;
            continue;
        }
        // drain everything the down-resampler produced: one 48k burst can
        // carry several 16k chunks
        auto t0 = std::chrono::steady_clock::now();
        try {
            res_down->push(blk.data(), blk.size());
            while (s->run.load()) {
                if (!res_down->take(s16, chunk)) break;   // skip, never zero-fill
                g_nin16 += (long long)chunk;
                auto t1 = std::chrono::steady_clock::now();
                std::vector<float> out16 = g_pipe->feed(s16);
                auto t2 = std::chrono::steady_clock::now();
                g_feed_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                g_proc_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
                if (out16.empty()) continue;
                g_nout16 += (long long)out16.size();
                res_up->push(out16.data(), out16.size());
                std::vector<float> o48 = res_up->take_all();
                if (!o48.empty())
                    s->out_q->push(std::move(o48), true); // keep newest flowing
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "[worker] EXCEPTION: %s\n", e.what());
            fflush(stderr);
            set_err(std::string("引擎计算错误: ") + e.what());
            s->stream_err = -1;
            s->run = false;                    // audio threads idle to silence
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// WASAPI session: RtCore + devices + stream threads
// ---------------------------------------------------------------------------
struct Session {
    RtCore core;
    std::wstring in_dev, out_dev;                // endpoint IDs ("" = default)
    HANDLE cap_evt = nullptr, ren_evt = nullptr;
    IAudioClient* cap_client = nullptr;
    IAudioClient* ren_client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    std::thread worker, cap_th, ren_th;
};

static std::atomic<Session*> g_sess{nullptr};    // null = no active session

static void com_init() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

// open a shared-mode stream in our fixed format; Windows converts (48k f32 mono)
static HRESULT open_stream(bool capture, const std::wstring& dev_id,
                           IAudioClient** out_client, HANDLE evt) {
    com_init();
    IMMDeviceEnumerator* de = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&de);
    if (FAILED(hr)) return hr;
    IMMDevice* dev = nullptr;
    if (!dev_id.empty()) hr = de->GetDevice(dev_id.c_str(), &dev);
    else hr = de->GetDefaultAudioEndpoint(
             capture ? eCapture : eRender, eConsole, &dev);
    de->Release();
    if (FAILED(hr)) return hr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       (void**)out_client);
    dev->Release();
    if (FAILED(hr)) return hr;

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 1;
    wf.nSamplesPerSec = STREAM_SR;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = STREAM_SR * 4;
    hr = (*out_client)->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   HNS_BUFFER, 0, &wf, nullptr);
    if (FAILED(hr)) { (*out_client)->Release(); *out_client = nullptr; return hr; }
    hr = (*out_client)->SetEventHandle(evt);
    if (FAILED(hr)) { (*out_client)->Release(); *out_client = nullptr; return hr; }
    return S_OK;
}

// capture thread: event -> drain all packets -> in_q (block copy only)
static void capture_loop(Session* s) {
    com_init();
    RtCore* c = &s->core;
    for (;;) {
        DWORD w = WaitForSingleObject(s->cap_evt, 500);
        if (!c->run.load()) return;
        if (w != WAIT_OBJECT_0) continue;
        UINT32 pk = 0;
        while (SUCCEEDED(s->cap->GetNextPacketSize(&pk)) && pk > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(s->cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            if (frames) {
                std::vector<float> blk(frames);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                    std::fill(blk.begin(), blk.end(), 0.f);
                else
                    std::copy((float*)data, (float*)data + frames, blk.begin());
                const float g = c->in_gain.load();
                if (g != 1.0f)
                    for (auto& v : blk) v *= g;
                c->in_q->push(std::move(blk), false);   // full: count & drop
            }
            s->cap->ReleaseBuffer(frames);
        }
    }
}

// render thread: event -> top the hardware buffer up from RtCore playback
static void render_loop(Session* s) {
    com_init();
    RtCore* c = &s->core;
    UINT32 buf_frames = 0;
    s->ren_client->GetBufferSize(&buf_frames);
    for (;;) {
        DWORD w = WaitForSingleObject(s->ren_evt, 500);
        if (!c->run.load()) return;
        if (w != WAIT_OBJECT_0) continue;
        UINT32 pad = 0;
        if (FAILED(s->ren_client->GetCurrentPadding(&pad))) continue;
        UINT32 avail = buf_frames - pad;
        if (!avail) continue;
        BYTE* data = nullptr;
        if (FAILED(s->ren->GetBuffer(avail, &data))) continue;
        playback_fill(c, (float*)data, avail);
        s->ren->ReleaseBuffer(avail, 0);
    }
}

// retire a session on the caller thread (WASAPI Stop/Release are quick, unlike
// AAudio close, so no detached closer needed — just join everything first)
static void retire_session(Session* s) {
    s->core.run = false;
    s->core.in_q->close();
    if (s->worker.joinable()) s->worker.join();
    if (s->cap_th.joinable()) s->cap_th.join();
    if (s->ren_th.joinable()) s->ren_th.join();
    if (s->cap_client) s->cap_client->Stop();
    if (s->ren_client) s->ren_client->Stop();
    if (s->cap) s->cap->Release();
    if (s->ren) s->ren->Release();
    if (s->cap_client) s->cap_client->Release();
    if (s->ren_client) s->ren_client->Release();
    if (s->cap_evt) CloseHandle(s->cap_evt);
    if (s->ren_evt) CloseHandle(s->ren_evt);
    delete s->core.in_q;
    delete s->core.out_q;
    delete s;
}

// ---------------------------------------------------------------------------
// device enumeration (UTF-8 JSON)
// ---------------------------------------------------------------------------
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}
static std::string json_escape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if ((unsigned char)ch < 0x20) { /* skip control chars */ }
        else o += ch;
    }
    return o;
}

static bool list_endpoints(EDataFlow flow, std::string& json) {
    com_init();
    IMMDeviceEnumerator* de = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&de);
    if (FAILED(hr)) { set_err("COM 初始化失败"); return false; }
    IMMDeviceCollection* col = nullptr;
    hr = de->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    if (SUCCEEDED(hr)) {
        UINT n = 0; col->GetCount(&n);
        // default-device match by ID string: EnumAudioEndpoints and
        // GetDefaultAudioEndpoint return distinct COM instances for the same
        // endpoint, so pointer identity is unreliable
        std::wstring def_id;
        {
            IMMDevice* def = nullptr;
            if (SUCCEEDED(de->GetDefaultAudioEndpoint(
                    flow == eCapture ? eCapture : eRender, eConsole, &def))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(def->GetId(&id))) {
                    def_id = id;
                    CoTaskMemFree(id);
                }
                def->Release();
            }
        }
        for (UINT i = 0; i < n; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            LPWSTR id = nullptr;
            std::string ids, name;
            if (SUCCEEDED(dev->GetId(&id))) {
                ids = wide_to_utf8(id);
                bool is_def = def_id == id;
                CoTaskMemFree(id);
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v))) {
                        if (v.pwszVal) name = wide_to_utf8(v.pwszVal);
                        PropVariantClear(&v);
                    }
                    ps->Release();
                }
                if (!json.empty()) json += ",";
                json += "{\"id\":\"" + json_escape(ids) + "\",\"name\":\"" +
                        json_escape(name) + "\",\"default\":" +
                        (is_def ? "true" : "false") + "}";
            }
            dev->Release();
        }
        col->Release();
    }
    de->Release();
    return true;
}

static bool cable_installed() {
    std::string ren, cap;
    list_endpoints(eRender, ren);
    list_endpoints(eCapture, cap);
    return ren.find("CABLE Input") != std::string::npos &&
           cap.find("CABLE Output") != std::string::npos;
}
// (cable_installed kept only as a helper; mvcIsCableInstalled is the ABI)// utf-8 -> wide (for device ids/names coming back from C#); file paths go
// through CP_ACP below (fopen is ANSI)
static std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::string utf8_to_acp(const char* s) {
    std::wstring w = utf8_to_wide(s);
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string a(n, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &a[0], n,
                        nullptr, nullptr);
    return a;
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
#define MVC_EXPORT extern "C" __declspec(dllexport)

MVC_EXPORT int mvcInit(const char* models_dir_utf8, const char* spk_path_utf8,
                       int ort_threads, int mode) {
    try {
        if (g_pipe) return 0;                     // idempotent
        std::string md = utf8_to_acp(models_dir_utf8);
        std::string sp = utf8_to_acp(spk_path_utf8);
        g_pipe = new Pipe(mode == 1 ? PipeCfg::mode120() : PipeCfg::mode40());
        g_pipe->ort_threads = ort_threads;
        try {
            g_pipe->load_sessions(md);
            if (!g_pipe->load_spk(sp)) {
                set_err("音色卡加载失败（不是有效的 256 维 .spk.npy）: " + sp);
                delete g_pipe; g_pipe = nullptr;
                return -1;
            }
        } catch (const std::exception& e) {
            set_err(std::string("引擎初始化失败: ") + e.what());
            delete g_pipe; g_pipe = nullptr;
            return -1;
        }
        res_down = new StreamResampler();
        res_up = new StreamResampler();
        res_down->configure(g_pipe->graphs[5], false);
        res_up->configure(g_pipe->graphs[6], true);
        g_spk_path = sp;
        g_models_dir = md;
        return 0;
    } catch (...) { set_err("mvcInit 未知异常"); return -1; }
}

// hot mode switch (engine stopped): rebuild the pipe with the other mode's
// geometry + models and re-apply the current voice card. ~1 s of model
// loading — call from a background thread.
MVC_EXPORT int mvcSetMode(int mode) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        if (g_sess.load()) { set_err("引擎运行中，先停止变声再切档"); return -1; }
        int cur = g_pipe->cfg.chunk == 2560 ? 1 : 0;
        if (mode == cur) return 0;
        PipeCfg cfg = (mode == 1) ? PipeCfg::mode120() : PipeCfg::mode40();
        Pipe* p = new Pipe(cfg);
        p->ort_threads = g_pipe->ort_threads;
        try {
            p->load_sessions(g_models_dir);
            if (!g_spk_path.empty() && !p->load_spk(g_spk_path)) {
                set_err("切换档位后音色卡加载失败");
                delete p;
                return -1;
            }
        } catch (const std::exception& e) {
            set_err(std::string("切换档位失败: ") + e.what());
            delete p;
            return -1;
        }
        delete g_pipe;
        g_pipe = p;
        res_down->configure(g_pipe->graphs[5], false);
        res_up->configure(g_pipe->graphs[6], true);
        return 0;
    } catch (...) { set_err("mvcSetMode 未知异常"); return -1; }
}

// internal stop, declared before first use
static void do_stop() {
    Session* s = g_sess.exchange(nullptr);
    if (s) retire_session(s);
    g_audio_running = false;
}

MVC_EXPORT void mvcShutdown() { do_stop(); }

MVC_EXPORT int mvcStart(double pre_roll_ms, double fade_ms, float in_gain,
                        float out_gain, const char* in_dev_utf8,
                        const char* out_dev_utf8) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        if (g_sess.load()) return 0;              // already running
        g_pipe->warmup(3);                        // prime ORT + reset pipe state
        // fresh resampler state: carry from a previous session must not leak
        res_down->configure(g_pipe->graphs[5], false);
        res_up->configure(g_pipe->graphs[6], true);

        Session* s = new Session();
        s->core.in_q = new SegQ(4);
        s->core.out_q = new SegQ(64);
        s->core.last_trim = std::chrono::steady_clock::now();
        s->core.pre_roll_ms = pre_roll_ms;
        s->core.prefill = (size_t)(pre_roll_ms * STREAM_SR / 1000.0);
        s->core.in_gain.store(in_gain > 0.f ? in_gain : 1.0f);
        s->core.out_gain.store(out_gain > 0.f ? out_gain : 1.0f);
        // seam fade length from ms (0 = off); clamp to [1, 20 ms]
        int fn = (int)(fade_ms * STREAM_SR / 1000.0);
        if (fn < 0) fn = 0;
        if (fn > 20 * STREAM_SR / 1000) fn = 20 * STREAM_SR / 1000;
        if (fn <= 1) {
            s->core.fade_in.assign(1, 1.0f);
            s->core.fade_out.assign(1, 1.0f);
        } else {
            s->core.fade_in.resize(fn);
            s->core.fade_out.resize(fn);
            for (int k = 0; k < fn; k++) {
                s->core.fade_in[k] = (float)k / (fn - 1);
                s->core.fade_out[k] = 1.0f - (float)k / (fn - 1);
            }
        }
        s->in_dev = utf8_to_wide(in_dev_utf8);
        s->out_dev = utf8_to_wide(out_dev_utf8);
        s->cap_evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        s->ren_evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        HRESULT hr = open_stream(true, s->in_dev, &s->cap_client, s->cap_evt);
        if (FAILED(hr)) {
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)hr);
            set_err(std::string("打开麦克风失败 (hr=") + hex + ")");
            retire_session(s);
            return -1;
        }
        hr = s->cap_client->GetService(__uuidof(IAudioCaptureClient),
                                       (void**)&s->cap);
        if (FAILED(hr)) {
            set_err("获取采集服务失败 (hr=" + std::to_string((unsigned)hr) + ")");
            retire_session(s);
            return -1;
        }
        hr = open_stream(false, s->out_dev, &s->ren_client, s->ren_evt);
        if (FAILED(hr)) {
            set_err("打开输出设备失败 (hr=" + std::to_string((unsigned)hr) + ")");
            retire_session(s);
            return -1;
        }
        hr = s->ren_client->GetService(__uuidof(IAudioRenderClient),
                                       (void**)&s->ren);
        if (FAILED(hr)) {
            set_err("获取渲染服务失败 (hr=" + std::to_string((unsigned)hr) + ")");
            retire_session(s);
            return -1;
        }

        g_nin16 = 0; g_nout16 = 0; g_played48 = 0;
        g_gaps = 0; g_preroll = 0;

        // real negotiated WASAPI numbers for the audio-info readout
        REFERENCE_TIME dper = 0, mper = 0;
        UINT32 buf_frames = 0;
        if (SUCCEEDED(s->cap_client->GetDevicePeriod(&dper, &mper)))
            g_cap_period_ms = dper / 10000.0;
        if (SUCCEEDED(s->ren_client->GetDevicePeriod(&dper, &mper)))
            g_ren_period_ms = dper / 10000.0;
        if (SUCCEEDED(s->ren_client->GetBufferSize(&buf_frames)))
            g_buf_ms = buf_frames / (STREAM_SR / 1000.0);
        g_audio_running = true;

        s->core.run = true;
        s->worker = std::thread(worker_loop, &s->core);
        s->cap_th = std::thread(capture_loop, s);
        s->ren_th = std::thread(render_loop, s);
        HRESULT rc = s->cap_client->Start();
        HRESULT rr = s->ren_client->Start();
        if (FAILED(rc) || FAILED(rr)) {
            set_err("音频流启动失败");
            retire_session(s);
            return -1;
        }
        g_sess.store(s);
        return 0;
    } catch (const std::exception& e) {
        set_err(std::string("启动失败: ") + e.what());
        return -1;
    } catch (...) { set_err("mvcStart 未知异常"); return -1; }
}

MVC_EXPORT void mvcStop() { do_stop(); }

// live gain adjustment while running (also fine while stopped: mvcStart
// re-reads the UI slider values anyway)
MVC_EXPORT void mvcSetGains(double in_gain, double out_gain) {
    Session* s = g_sess.load();
    if (!s) return;
    if (in_gain > 0.0) s->core.in_gain.store((float)in_gain);
    if (out_gain > 0.0) s->core.out_gain.store((float)out_gain);
}

typedef struct {
    double procMs;
    double feedMs;
    double bufMs;
    long long gaps;
    long long preroll;
    long long drops;
    long long odrops;
    long long in16;
    long long out16;
    long long played48;
    int running;
    int streamErr;
} MvcStatus;

MVC_EXPORT void mvcGetStatus(MvcStatus* out) {
    Session* s = g_sess.load();
    if (!out) return;
    out->procMs = g_proc_ms.load();
    out->feedMs = g_feed_ms.load();
    out->bufMs = s ? s->core.buf48.load() / (STREAM_SR / 1000.0) : 0.0;
    out->gaps = (long long)g_gaps.load();
    out->preroll = (long long)g_preroll.load();
    out->drops = s ? (long long)s->core.in_q->dropped.load() : 0;
    out->odrops = s ? (long long)s->core.out_q->dropped.load() : 0;
    out->in16 = (long long)g_nin16.load();
    out->out16 = (long long)g_nout16.load();
    out->played48 = (long long)g_played48.load();
    out->running = s ? 1 : 0;
    out->streamErr = s ? s->core.stream_err.load() : 0;
}

MVC_EXPORT int mvcEnumDevices(char* buf, int buf_len) {
    try {
        std::string cap, ren;
        list_endpoints(eCapture, cap);
        list_endpoints(eRender, ren);
        std::string j = "{\"capture\":[" + cap + "],\"render\":[" + ren + "]}";
        if (!buf || buf_len <= 0) return (int)j.size() + 1;
        int n = (int)j.size() < buf_len - 1 ? (int)j.size() : buf_len - 1;
        memcpy(buf, j.data(), n);
        buf[n] = 0;
        return (int)j.size() + 1;
    } catch (...) { set_err("mvcEnumDevices 未知异常"); return -1; }
}

// virtual-mic chain detection: bitmask — bit0 = VB-Cable ("CABLE Input"/
// "CABLE Output"), bit1 = Voicemeeter ("Voicemeeter Input" render + a
// "Voicemeeter Out B*" capture device as the Discord-facing mic)
MVC_EXPORT int mvcIsCableInstalled() {
    std::string ren, cap;
    list_endpoints(eRender, ren);
    list_endpoints(eCapture, cap);
    int mask = 0;
    if (ren.find("CABLE Input") != std::string::npos &&
        cap.find("CABLE Output") != std::string::npos)
        mask |= 1;
    if (ren.find("Voicemeeter Input") != std::string::npos &&
        cap.find("Voicemeeter Out B") != std::string::npos)
        mask |= 2;
    return mask;
}

// only while stopped (spks_ is read unlocked by feed on the worker thread)
MVC_EXPORT int mvcSetCard(const char* spk_path_utf8) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        std::string p = utf8_to_acp(spk_path_utf8);
        bool ok = g_pipe->load_spk(p);
        if (ok) g_spk_path = p;
        else set_err("音色卡加载失败（不是有效的 256 维 .spk.npy）");
        return ok ? 0 : -1;
    } catch (...) { set_err("mvcSetCard 未知异常"); return -1; }
}

// heavy (loads the 334 MB encoder) — call from a background thread.
// returns 0 on success, else err_buf holds a UTF-8 Chinese message.
MVC_EXPORT int mvcCloneCard(const char* wav_path_utf8, const char* out_npy_utf8,
                            char* err_buf, int err_len) {
    try {
        if (!g_pipe) {
            if (err_buf && err_len > 0) snprintf(err_buf, err_len, "引擎未初始化");
            return -1;
        }
        std::string w = utf8_to_acp(wav_path_utf8);
        std::string o = utf8_to_acp(out_npy_utf8);
        std::string err = g_pipe->clone_card(w, o);
        if (err.empty()) return 0;
        if (err_buf && err_len > 0) {
            // clone_card messages are produced in whatever the header uses
            // (UTF-8 sources read via fopen/ACP paths); pass through as UTF-8
            std::string u = wide_to_utf8(utf8_to_wide(err.c_str()));
            snprintf(err_buf, err_len, "%s", u.c_str());
        }
        return -1;
    } catch (const std::exception& e) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "%s", e.what());
        return -1;
    } catch (...) {
        if (err_buf && err_len > 0) snprintf(err_buf, err_len, "克隆未知异常");
        return -1;
    }
}

MVC_EXPORT int mvcGetLastError(char* buf, int buf_len) {
    std::lock_guard<std::mutex> lk(g_err_mu);
    if (!buf || buf_len <= 0) return (int)g_last_err.size() + 1;
    int n = (int)g_last_err.size() < buf_len - 1 ? (int)g_last_err.size() : buf_len - 1;
    memcpy(buf, g_last_err.data(), n);
    buf[n] = 0;
    return (int)g_last_err.size() + 1;
}

// audio backend readout: what API/mode is in use + the real negotiated
// device periods and buffer. UTF-8 text for direct display.
MVC_EXPORT int mvcGetAudioInfo(char* buf, int buf_len) {
    char text[192];
    if (g_audio_running.load())
        snprintf(text, sizeof(text),
                 "WASAPI 共享模式（事件驱动）· 采集周期 %.1fms · 渲染周期 %.1fms · 缓冲 %.0fms",
                 g_cap_period_ms, g_ren_period_ms, g_buf_ms);
    else
        snprintf(text, sizeof(text), "WASAPI 共享模式（未运行）");
    if (!buf || buf_len <= 0) return (int)strlen(text) + 1;
    snprintf(buf, buf_len, "%s", text);
    return (int)strlen(text) + 1;
}

// ---------------------------------------------------------------------------
// parity helpers: direct-16k sim (bypasses the resamplers — chunks the 16k
// stream exactly like run_rt.py process_file) + cond-dump export
// ---------------------------------------------------------------------------
MVC_EXPORT int mvcRunSim16(const char* wav_in_utf8, const char* wav_out_utf8) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        if (g_sess.load()) { set_err("引擎运行中"); return -1; }
        g_pipe->warmup(3);
        std::string in_path = utf8_to_acp(wav_in_utf8);
        std::string out_path = utf8_to_acp(wav_out_utf8);
        std::vector<float> src;
        int sr = 0;
        if (!read_wav16(in_path.c_str(), src, sr)) {
            set_err("无法读取输入 wav");
            return -1;
        }
        if (sr != 16000) { set_err("sim16 需要 16k 输入"); return -1; }
        const size_t chunk = g_pipe->cfg.chunk;
        std::vector<float> produced;
        size_t pos = 0;
        int idx = 0;
        while (pos + chunk <= src.size()) {
            std::vector<float> blk(src.begin() + pos, src.begin() + pos + chunk);
            fprintf(stderr, "[sim16] chunk %d\n", idx++);
            fflush(stderr);
            auto out = g_pipe->feed(blk);
            produced.insert(produced.end(), out.begin(), out.end());
            pos += chunk;
        }
        write_wav_f32(out_path.c_str(), produced, 16000);
        return 0;
    } catch (const std::exception& e) {
        set_err(std::string("sim16 失败: ") + e.what());
        return -1;
    }
}

MVC_EXPORT int mvcExportCondDump(const char* path_utf8) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        std::string p = utf8_to_acp(path_utf8);
        FILE* f = fopen(p.c_str(), "wb");
        if (!f) { set_err("无法写入 cond dump"); return -1; }
        fwrite(g_pipe->cond_dump.data(), sizeof(float),
               g_pipe->cond_dump.size(), f);
        fclose(f);
        return 0;
    } catch (...) { set_err("mvcExportCondDump 未知异常"); return -1; }
}

// ---------------------------------------------------------------------------
// file-sim driver (same queues/worker/playback as the live session; no audio
// devices). Verifies the DLL against the meanvc_rt --sim baseline.
// ---------------------------------------------------------------------------
MVC_EXPORT int mvcRunSim(const char* wav_in_utf8, const char* wav_out_utf8,
                         double pre_roll_ms) {
    try {
        if (!g_pipe) { set_err("引擎未初始化"); return -1; }
        if (g_sess.load()) { set_err("引擎运行中，不能跑 sim"); return -1; }
        g_pipe->warmup(3);
        res_down->configure(g_pipe->graphs[5], false);
        res_up->configure(g_pipe->graphs[6], true);

        RtCore core;
        core.in_q = new SegQ(4);
        core.out_q = new SegQ(64);
        core.pre_roll_ms = pre_roll_ms;
        core.prefill = (size_t)(pre_roll_ms * STREAM_SR / 1000.0);
        core.fade_in.resize(FADE_N);
        core.fade_out.resize(FADE_N);
        for (int k = 0; k < FADE_N; k++) {
            core.fade_in[k] = (float)k / (FADE_N - 1);
            core.fade_out[k] = 1.0f - (float)k / (FADE_N - 1);
        }
        g_nin16 = 0; g_nout16 = 0; g_played48 = 0; g_gaps = 0; g_preroll = 0;

        core.run = true;
        std::thread th(worker_loop, &core);

        std::string in_path = utf8_to_acp(wav_in_utf8);
        std::string out_path = utf8_to_acp(wav_out_utf8);
        std::vector<float> src16;
        int sr = 0;
        if (!read_wav16(in_path.c_str(), src16, sr)) {
            set_err("无法读取输入 wav");
            core.run = false;
            core.in_q->close();
            th.join();
            delete core.in_q; delete core.out_q;
            return -1;
        }
        // whole-file upsample to 48k (same as meanvc_rt --sim)
        fprintf(stderr, "[sim] read %zu @%dHz\n", src16.size(), sr);
        std::vector<Ort::Value> uin;
        uin.push_back(tensor_f(src16, {1, 1, (int64_t)src16.size()}));
        auto uouts = run_graph(g_pipe->graphs[6], uin);
        size_t n48 = uouts[0].GetTensorTypeAndShapeInfo().GetElementCount();
        const float* p48 = uouts[0].GetTensorData<float>();
        std::vector<float> src48(p48, p48 + n48);
        fprintf(stderr, "[sim] upsampled to %zu @48k\n", src48.size());
        fflush(stderr);

        const size_t BLOCK48 = 1920;              // 40 ms
        std::vector<float> played_all;
        size_t pos = 0;
        auto t_start = std::chrono::steady_clock::now();
        long long blk_id = 0;
        long long tail = 0;
        while (true) {
            if (!core.run.load()) {               // worker died mid-sim
                set_err("worker 提前退出: " + [] {
                    std::lock_guard<std::mutex> lk(g_err_mu);
                    return g_last_err;
                }());
                break;
            }
            auto due = t_start + std::chrono::milliseconds(blk_id * 40);
            std::this_thread::sleep_until(due);
            std::vector<float> outblk(BLOCK48, 0.f);
            if (pos + BLOCK48 <= src48.size()) {
                core.in_q->push(std::vector<float>(src48.begin() + pos,
                                                   src48.begin() + pos + BLOCK48),
                                false);
                pos += BLOCK48;
            } else if (pos < src48.size()) {
                // final partial block: push the remainder (the resampler
                // accepts any 48k block size; the down path emits full 640s)
                core.in_q->push(std::vector<float>(src48.begin() + pos,
                                                   src48.end()),
                                false);
                pos = src48.size();
            }
            std::fill(outblk.begin(), outblk.end(), 0.f);
            {
                char dbg[32];
                snprintf(dbg, sizeof(dbg), "blk=%lld proc=%.1fms", blk_id,
                         g_proc_ms.load());
                playback_fill(&core, outblk.data(), BLOCK48, dbg);
            }
            played_all.insert(played_all.end(), outblk.begin(), outblk.end());
            if (blk_id % 25 == 0) {
                fprintf(stderr, "[sim] blk=%lld pos=%zu played=%.1fs buf48=%lld\n",
                        blk_id, pos, played_all.size() / (double)STREAM_SR,
                        (long long)core.out_buffer.size());
                fflush(stderr);
            }
            blk_id++;
            if (pos >= src48.size()) {
                // input drained: let the worker flush everything through, then
                // stop. Clean check = both queues empty and playback nearly
                // drained; require a few consecutive clean blocks so a worker
                // mid-feed can't be cut off, and a hard stall cap.
                std::lock_guard<std::mutex> lk(core.in_q->mu);
                bool clean = core.in_q->q.empty();
                core.out_q->mu.lock();
                clean = clean && core.out_q->q.empty();
                core.out_q->mu.unlock();
                if (clean && core.out_buffer.size() < BLOCK48) {
                    if (++tail > 5) break;
                } else tail = 0;
                if (tail > 250) break;            // never hang on a stall
            }
        }
        core.run = false;
        core.in_q->close();
        th.join();
        delete core.in_q;
        delete core.out_q;
        write_wav_f32(out_path.c_str(), played_all, STREAM_SR);
        return 0;
    } catch (const std::exception& e) {
        set_err(std::string("sim 失败: ") + e.what());
        return -1;
    }
}
