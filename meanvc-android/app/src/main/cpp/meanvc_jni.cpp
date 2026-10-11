// meanvc_jni.cpp — Android realtime shell over the shared pipeline
// (meanvc_pipe.h). Architecture identical to run_rt.py run_realtime and the
// Windows meanvc_rt.cpp shell:
//
//   mic (AAudio input callback, 48k float, any burst size) -> in_q
//   -> worker thread: res_down (48k->16k) -> SKIP (never zero-fill) when no
//      full 640 yet -> Pipe::feed (640) -> res_up (16k->48k) -> out_q
//   -> AAudio output callback: pre-roll cushion, 2 ms seam fades, drain.
//
// Heavy work never runs on the audio threads; the audio callbacks contain no
// JNI and no allocation beyond the input block copy. The Kotlin layer is only
// permission + asset copy + start/stop/status.
#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "meanvc_pipe.h"

#define TAG "meanvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace mvc;

// ---------------------------------------------------------------------------
// bounded segment queue (same as meanvc_rt.cpp)
// ---------------------------------------------------------------------------
struct SegQ {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<float>> q;
    size_t maxq;
    bool closed = false;
    long dropped = 0;

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
// engine state (one engine instance per process; guarded by lifecycle order)
// ---------------------------------------------------------------------------
static const int STREAM_SR = 48000;
static const int FADE_N = 96;                    // 2 ms @48k

static Pipe* g_pipe = nullptr;
static StreamResampler* res_down = nullptr;
static StreamResampler* res_up = nullptr;
static SegQ* in_q = nullptr;
static SegQ* out_q = nullptr;
static std::thread* worker_th = nullptr;

static std::atomic<bool> g_run{false};
static std::atomic<bool> g_pre_roll{true};
static std::atomic<bool> g_was_gap{true};
static std::atomic<long long> g_nin16{0}, g_nout16{0}, g_played48{0};
static std::atomic<long long> g_gaps{0}, g_preroll{0};
static std::atomic<double> g_proc_ms{0.0}, g_feed_ms{0.0};
static std::atomic<int> g_stream_err{0};         // last AAudio error (0 = none)
static double g_pre_roll_ms = 40.0;
static size_t g_prefill = 1920;                  // 40 ms @48k

// playback buffer + fade state live entirely on the AAudio output-callback
// thread (single consumer)
static std::vector<float> g_out_buffer;
static std::vector<float> g_fade_in, g_fade_out;

static AAudioStream* g_in_stream = nullptr;
static AAudioStream* g_out_stream = nullptr;

// ---------------------------------------------------------------------------
// audio callbacks (real-time threads — no JNI, no logging, no big allocs)
// ---------------------------------------------------------------------------
static aaudio_data_callback_result_t in_cb(AAudioStream*, void*, void* data,
                                           int32_t n) {
    if (!g_run.load()) return AAUDIO_CALLBACK_RESULT_STOP;
    const float* p = (const float*)data;
    if (!in_q->push(std::vector<float>(p, p + n), false)) {
        // queue full: worker is behind — count and drop (matches run_rt)
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t out_cb(AAudioStream*, void*, void* data,
                                            int32_t n) {
    if (!g_run.load()) return AAUDIO_CALLBACK_RESULT_STOP;
    float* out = (float*)data;
    std::vector<float> seg;
    while (out_q->pop(seg, 0))
        g_out_buffer.insert(g_out_buffer.end(), seg.begin(), seg.end());

    const size_t need = (size_t)n;
    if (g_pre_roll.load()) {
        if (g_out_buffer.size() < g_prefill) {
            std::fill(out, out + n, 0.f);
            g_preroll++;
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        g_pre_roll = false;
        LOGI("pre-roll done (%.0f ms cushion) — playing", g_pre_roll_ms);
    }
    if (g_out_buffer.size() >= need) {
        std::copy(g_out_buffer.begin(), g_out_buffer.begin() + need, out);
        g_out_buffer.erase(g_out_buffer.begin(), g_out_buffer.begin() + need);
        if (g_was_gap.load()) {
            for (int k = 0; k < FADE_N && k < (int)need; k++)
                out[k] *= g_fade_in[k];
            g_was_gap = false;
        }
        g_played48 += (long long)need;
    } else {
        size_t n_avail = g_out_buffer.size();
        if (n_avail) {
            std::copy(g_out_buffer.begin(), g_out_buffer.end(), out);
            if (n_avail >= FADE_N)
                for (int k = 0; k < FADE_N; k++)
                    out[n_avail - FADE_N + k] *= g_fade_out[k];
            g_played48 += (long long)n_avail;
        }
        std::fill(out + n_avail, out + n, 0.f);
        g_out_buffer.clear();
        g_was_gap = true;
        g_gaps++;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void err_cb(AAudioStream*, void*, aaudio_result_t error) {
    LOGE("AAudio error: %s", AAudio_convertResultToText(error));
    // reroute/disconnect events kill the callbacks — surface it so the UI
    // can stop cleanly instead of freezing on starved counters
    g_stream_err = (int)error;
}

// ---------------------------------------------------------------------------
// worker thread (identical to meanvc_rt.cpp)
// ---------------------------------------------------------------------------
static void worker_loop() {
    std::vector<float> s16(CHUNK);
    for (;;) {
        std::vector<float> blk;
        if (!in_q->pop(blk, 200)) {
            if (in_q->closed || !g_run.load()) return;
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();
        res_down->push(blk.data(), blk.size());
        if (!res_down->take(s16, CHUNK)) continue;   // skip, never zero-fill
        g_nin16 += (long long)CHUNK;
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
            out_q->push(std::move(o48), true);       // keep newest flowing
    }
}

// ---------------------------------------------------------------------------
// JNI lifecycle
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL
Java_com_mean2vc_voice_MainActivity_nativeInit(JNIEnv* env, jobject,
                                               jstring jModelsDir,
                                               jstring jSpkPath,
                                               jint ortThreads) {
    const char* models_dir = env->GetStringUTFChars(jModelsDir, nullptr);
    const char* spk_path = env->GetStringUTFChars(jSpkPath, nullptr);
    std::string md(models_dir), sp(spk_path);
    env->ReleaseStringUTFChars(jModelsDir, models_dir);
    env->ReleaseStringUTFChars(jSpkPath, spk_path);

    if (g_pipe) return JNI_TRUE;                  // idempotent
    g_pipe = new Pipe();
    g_pipe->ort_threads = (int)ortThreads;
    try {
        g_pipe->load_sessions(md);
        g_pipe->load_spk(sp);
    } catch (const std::exception& e) {
        LOGE("init failed: %s", e.what());
        delete g_pipe; g_pipe = nullptr;
        return JNI_FALSE;
    }
    res_down = new StreamResampler();
    res_up = new StreamResampler();
    res_down->configure(g_pipe->graphs[5], false);
    res_up->configure(g_pipe->graphs[6], true);
    in_q = new SegQ(4);
    out_q = new SegQ(64);
    g_fade_in.resize(FADE_N);
    g_fade_out.resize(FADE_N);
    for (int k = 0; k < FADE_N; k++) {
        g_fade_in[k] = (float)k / (FADE_N - 1);
        g_fade_out[k] = 1.0f - (float)k / (FADE_N - 1);
    }
    LOGI("engine ready (%d ort threads)", (int)ortThreads);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mean2vc_voice_MainActivity_nativeStart(JNIEnv*, jobject,
                                                jdouble preRollMs,
                                                jint inDevId, jint outDevId,
                                                jboolean aec) {
    if (!g_pipe) return JNI_FALSE;
    if (g_run.load()) return JNI_TRUE;          // already running

    g_pipe->warmup(3);                            // prime ORT + reset state
    g_pre_roll_ms = preRollMs;
    g_prefill = (size_t)(preRollMs * STREAM_SR / 1000.0);
    g_out_buffer.clear();
    g_pre_roll = true;
    g_was_gap = true;
    g_gaps = 0; g_preroll = 0;
    g_nin16 = 0; g_nout16 = 0; g_played48 = 0;
    in_q->dropped = 0; out_q->dropped = 0;
    in_q->closed = false;
    g_stream_err = 0;

    g_run = true;
    worker_th = new std::thread(worker_loop);

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { g_run = false; return JNI_FALSE; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(b, STREAM_SR);
    AAudioStreamBuilder_setChannelCount(b, 1);
    if (inDevId != 0) AAudioStreamBuilder_setDeviceId(b, (int32_t)inDevId);
    if (aec) AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDataCallback(b, in_cb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, err_cb, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_in_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("input stream open failed (dev %d): %s", inDevId,
             AAudio_convertResultToText(r));
        g_run = false;
        return JNI_FALSE;
    }

    b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { g_run = false; return JNI_FALSE; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(b, STREAM_SR);
    AAudioStreamBuilder_setChannelCount(b, 1);
    if (outDevId != 0) AAudioStreamBuilder_setDeviceId(b, (int32_t)outDevId);
    if (aec) AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDataCallback(b, out_cb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, err_cb, nullptr);
    r = AAudioStreamBuilder_openStream(b, &g_out_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("output stream open failed (dev %d): %s", outDevId,
             AAudio_convertResultToText(r));
        AAudioStream_close(g_in_stream); g_in_stream = nullptr;
        g_run = false;
        return JNI_FALSE;
    }
    AAudioStream_requestStart(g_out_stream);
    AAudioStream_requestStart(g_in_stream);
    LOGI("streams started (dev in=%d out=%d aec=%d, in burst=%d, out burst=%d)",
         inDevId, outDevId, (int)aec,
         (int)AAudioStream_getFramesPerBurst(g_in_stream),
         (int)AAudioStream_getFramesPerBurst(g_out_stream));
    return JNI_TRUE;
}

// On-device cloning: heavy (334 MB encoder load + inference) — call from a
// background thread. Returns NULL on success, else the error message.
extern "C" JNIEXPORT jstring JNICALL
Java_com_mean2vc_voice_MainActivity_nativeCloneCard(JNIEnv* env, jobject,
                                                    jstring jWav,
                                                    jstring jOut) {
    if (!g_pipe) return env->NewStringUTF("引擎未初始化");
    const char* w = env->GetStringUTFChars(jWav, nullptr);
    const char* o = env->GetStringUTFChars(jOut, nullptr);
    std::string err = g_pipe->clone_card(w, o);
    env->ReleaseStringUTFChars(jWav, w);
    env->ReleaseStringUTFChars(jOut, o);
    if (err.empty()) {
        LOGI("clone ok");
        return nullptr;
    }
    LOGE("clone failed: %s", err.c_str());
    return env->NewStringUTF(err.c_str());
}

// Swap the active voice card (only while stopped — the Kotlin layer enforces).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_mean2vc_voice_MainActivity_nativeSetCard(JNIEnv* env, jobject,
                                                  jstring jPath) {
    if (!g_pipe) return JNI_FALSE;
    const char* p = env->GetStringUTFChars(jPath, nullptr);
    bool ok = g_pipe->load_spk(p);
    env->ReleaseStringUTFChars(jPath, p);
    if (ok) LOGI("card set");
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mean2vc_voice_MainActivity_nativeStop(JNIEnv*, jobject) {
    if (!g_run.load()) return;
    g_run = false;
    // wake the worker BEFORE touching the streams; and close the streams on a
    // detached thread — AAudio stop/close joins the callback threads and can
    // stall indefinitely on a wedged/rerouting audio HAL (that froze the UI's
    // stop action once). The callbacks early-exit while g_run is false, so
    // swapping the queues underneath them is safe.
    in_q->close();
    AAudioStream* in = g_in_stream; g_in_stream = nullptr;
    AAudioStream* out = g_out_stream; g_out_stream = nullptr;
    if (worker_th && worker_th->joinable()) { worker_th->join(); delete worker_th; }
    worker_th = nullptr;
    delete in_q;  in_q = new SegQ(4);            // fresh queues for next run
    delete out_q; out_q = new SegQ(64);
    if (in || out) std::thread([in, out] {
        if (in) { AAudioStream_requestStop(in); AAudioStream_close(in); }
        if (out) { AAudioStream_requestStop(out); AAudioStream_close(out); }
    }).detach();
    LOGI("stopped: played=%llds gaps=%lld",
         (long long)(g_played48.load() / STREAM_SR), (long long)g_gaps.load());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mean2vc_voice_MainActivity_nativeStatus(JNIEnv* env, jobject) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "proc=%.1fms feed=%.1fms buf=%.0fms gaps=%lld preroll=%lld drops=%ld "
             "in16=%lld out16=%lld played=%lld aerr=%d",
             g_proc_ms.load(), g_feed_ms.load(),
             g_out_buffer.size() / (STREAM_SR / 1000.0),
             (long long)g_gaps.load(), (long long)g_preroll.load(),
             in_q ? in_q->dropped : 0L,
             (long long)g_nin16.load(), (long long)g_nout16.load(),
             (long long)g_played48.load(), g_stream_err.load());
    return env->NewStringUTF(buf);
}
