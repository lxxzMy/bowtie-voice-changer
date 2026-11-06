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
    std::atomic<long> dropped{0};   // bumped under mu, read lock-free by nativeStatus

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

// process-wide counters (status/log readouts only — never used for decisions)
static std::atomic<long long> g_nin16{0}, g_nout16{0}, g_played48{0};
static std::atomic<long long> g_gaps{0}, g_preroll{0};
static std::atomic<double> g_proc_ms{0.0}, g_feed_ms{0.0};

// One start/stop lifecycle. EVERYTHING the audio callbacks and the worker
// touch lives here, so a callback of a stream being torn down can only ever
// reach its own session's state — never the next run's. The session is freed
// by the detached closer only AFTER both AAudioStream_close calls return
// (close joins the callback threads), so no callback can outlive it.
struct Session {
    std::atomic<bool> run{false};
    SegQ* in_q = nullptr;                        // owned
    SegQ* out_q = nullptr;                       // owned
    std::vector<float> out_buffer;               // playback accumulation (out_cb only)
    std::atomic<long long> buf48{0};             // out_buffer size mirror for nativeStatus
    std::atomic<bool> pre_roll{true};
    std::atomic<bool> was_gap{true};
    size_t prefill = 1920;                       // 40 ms @48k
    double pre_roll_ms = 40.0;
    std::vector<float> fade_in, fade_out;        // seam ramps, built in nativeStart
    std::atomic<int> stream_err{0};              // last AAudio error (0 = none)
    AAudioStream* in = nullptr;
    AAudioStream* out = nullptr;
    std::thread worker;
};
static std::atomic<Session*> g_sess{nullptr};    // null = no active session

// ---------------------------------------------------------------------------
// audio callbacks (real-time threads — no JNI, no logging, no big allocs).
// ctx is the Session the stream was opened for; the early-exit key is THAT
// session's run flag, so a stale callback from a stream being torn down stays
// dormant even if a new session starts while its close is still in flight.
// ---------------------------------------------------------------------------
static aaudio_data_callback_result_t in_cb(AAudioStream*, void* ctx, void* data,
                                           int32_t n) {
    Session* s = (Session*)ctx;
    if (!s->run.load()) return AAUDIO_CALLBACK_RESULT_STOP;
    const float* p = (const float*)data;
    if (!s->in_q->push(std::vector<float>(p, p + n), false)) {
        // queue full: worker is behind — count and drop (matches run_rt)
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t out_cb(AAudioStream*, void* ctx, void* data,
                                            int32_t n) {
    Session* s = (Session*)ctx;
    if (!s->run.load()) return AAUDIO_CALLBACK_RESULT_STOP;
    float* out = (float*)data;
    std::vector<float>& buf = s->out_buffer;
    std::vector<float> seg;
    while (s->out_q->pop(seg, 0))
        buf.insert(buf.end(), seg.begin(), seg.end());
    s->buf48 = (long long)buf.size();

    const size_t need = (size_t)n;
    if (s->pre_roll.load()) {
        if (buf.size() < s->prefill) {
            std::fill(out, out + n, 0.f);
            g_preroll++;
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        s->pre_roll = false;
        LOGI("pre-roll done (%.0f ms cushion) — playing", s->pre_roll_ms);
    }
    if (buf.size() >= need) {
        std::copy(buf.begin(), buf.begin() + need, out);
        buf.erase(buf.begin(), buf.begin() + need);
        s->buf48 = (long long)buf.size();
        if (s->was_gap.load()) {
            for (size_t k = 0; k < s->fade_in.size() && k < need; k++)
                out[k] *= s->fade_in[k];
            s->was_gap = false;
        }
        g_played48 += (long long)need;
    } else {
        size_t n_avail = buf.size();
        if (n_avail) {
            std::copy(buf.begin(), buf.end(), out);
            if (n_avail >= s->fade_out.size())
                for (size_t k = 0; k < s->fade_out.size(); k++)
                    out[n_avail - s->fade_out.size() + k] *= s->fade_out[k];
            g_played48 += (long long)n_avail;
        }
        std::fill(out + n_avail, out + n, 0.f);
        buf.clear();
        s->buf48 = 0;
        s->was_gap = true;
        g_gaps++;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void err_cb(AAudioStream*, void* ctx, aaudio_result_t error) {
    LOGE("AAudio error: %s", AAudio_convertResultToText(error));
    // reroute/disconnect events kill the callbacks — surface it so the UI
    // can stop cleanly instead of freezing on starved counters. Per-session:
    // a teardown-time error from an old stream must not stop a new session.
    ((Session*)ctx)->stream_err = (int)error;
}

// ---------------------------------------------------------------------------
// worker thread (same shape as meanvc_rt.cpp, but session-scoped)
// ---------------------------------------------------------------------------
static void worker_loop(Session* s) {
    std::vector<float> s16(CHUNK);
    for (;;) {
        std::vector<float> blk;
        if (!s->in_q->pop(blk, 200)) {
            if (s->in_q->closed || !s->run.load()) return;
            continue;
        }
        // drain everything the down-resampler produced: one 48k input burst
        // can carry several 16k chunks — take them all so the input never
        // permanently lags behind on bursts
        auto t0 = std::chrono::steady_clock::now();
        res_down->push(blk.data(), blk.size());
        while (s->run.load()) {
            if (!res_down->take(s16, CHUNK)) break;   // skip, never zero-fill
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
                s->out_q->push(std::move(o48), true);   // keep newest flowing
        }
    }
}

// Retire a session: stop the worker, then hand the streams to a detached
// closer that requestStop+close()s them (close joins the callback threads and
// can stall on a wedged/rerouting HAL — never wait for it on a caller thread)
// and frees the whole session only after both closes have returned.
static void retire_session(Session* s) {
    s->run = false;                    // callbacks + worker wind down on this
    s->in_q->close();                  // wake the worker before touching streams
    if (s->worker.joinable()) s->worker.join();
    AAudioStream* in = s->in; s->in = nullptr;
    AAudioStream* out = s->out; s->out = nullptr;
    std::thread([s, in, out] {
        if (out) { AAudioStream_requestStop(out); AAudioStream_close(out); }
        if (in) { AAudioStream_requestStop(in); AAudioStream_close(in); }
        delete s->in_q;
        delete s->out_q;
        delete s;                      // safe: both callbacks were joined by close()
    }).detach();
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
    // queues and fade tables are per-session now — nativeStart builds them
    LOGI("engine ready (%d ort threads)", (int)ortThreads);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mean2vc_voice_MainActivity_nativeStart(JNIEnv*, jobject,
                                                jdouble preRollMs,
                                                jint inDevId, jint outDevId,
                                                jboolean aec) {
    if (!g_pipe) return JNI_FALSE;
    if (g_sess.load()) return JNI_TRUE;           // already running
    try {
        g_pipe->warmup(3);                        // prime ORT + reset pipe state
        // fresh resampler state too: carry/tail from the previous session
        // would otherwise leak into this one's first blocks
        res_down->configure(g_pipe->graphs[5], false);
        res_up->configure(g_pipe->graphs[6], true);

        Session* s = new Session();
        s->in_q = new SegQ(4);
        s->out_q = new SegQ(64);
        s->pre_roll_ms = preRollMs;
        s->prefill = (size_t)(preRollMs * STREAM_SR / 1000.0);
        s->fade_in.resize(FADE_N);
        s->fade_out.resize(FADE_N);
        for (int k = 0; k < FADE_N; k++) {
            s->fade_in[k] = (float)k / (FADE_N - 1);
            s->fade_out[k] = 1.0f - (float)k / (FADE_N - 1);
        }
        g_nin16 = 0; g_nout16 = 0; g_played48 = 0;
        g_gaps = 0; g_preroll = 0;

        // open BOTH streams before anything runs: data callbacks only fire
        // from the requestStart calls below, so a failed open is a plain
        // delete — no worker to stop, no callback that can outlive the session
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { delete s; return JNI_FALSE; }
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(b, STREAM_SR);
        AAudioStreamBuilder_setChannelCount(b, 1);
        if (inDevId != 0) AAudioStreamBuilder_setDeviceId(b, (int32_t)inDevId);
        if (aec) AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
        AAudioStreamBuilder_setDataCallback(b, in_cb, s);
        AAudioStreamBuilder_setErrorCallback(b, err_cb, s);
        aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->in);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) {
            LOGE("input stream open failed (dev %d): %s", inDevId,
                 AAudio_convertResultToText(r));
            delete s;
            return JNI_FALSE;
        }

        b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
            AAudioStream_close(s->in);
            delete s;
            return JNI_FALSE;
        }
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(b, STREAM_SR);
        AAudioStreamBuilder_setChannelCount(b, 1);
        if (outDevId != 0) AAudioStreamBuilder_setDeviceId(b, (int32_t)outDevId);
        if (aec) AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_VOICE_COMMUNICATION);
        AAudioStreamBuilder_setDataCallback(b, out_cb, s);
        AAudioStreamBuilder_setErrorCallback(b, err_cb, s);
        r = AAudioStreamBuilder_openStream(b, &s->out);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) {
            LOGE("output stream open failed (dev %d): %s", outDevId,
                 AAudio_convertResultToText(r));
            AAudioStream_close(s->in);            // was never started — no callbacks
            delete s;
            return JNI_FALSE;
        }

        // everything armed; publish only once fully started so nativeStop can
        // never see a half-built session. run flips BEFORE the worker starts
        // (its 200 ms pop wait must not see a false run) and before
        // requestStart (the callbacks early-exit on a false run).
        s->run = true;
        s->worker = std::thread(worker_loop, s);
        aaudio_result_t ro = AAudioStream_requestStart(s->out);
        aaudio_result_t ri = AAudioStream_requestStart(s->in);
        if (ro != AAUDIO_OK || ri != AAUDIO_OK) {
            LOGE("requestStart failed (in=%s out=%s)",
                 AAudio_convertResultToText(ri), AAudio_convertResultToText(ro));
            retire_session(s);                    // never published — private retire
            return JNI_FALSE;
        }
        g_sess.store(s);
        LOGI("streams started (dev in=%d out=%d aec=%d, in burst=%d, out burst=%d)",
             inDevId, outDevId, (int)aec,
             (int)AAudioStream_getFramesPerBurst(s->in),
             (int)AAudioStream_getFramesPerBurst(s->out));
        return JNI_TRUE;
    } catch (const std::exception& e) {           // ORT exceptions must not cross JNI
        LOGE("start failed: %s", e.what());
        return JNI_FALSE;
    }
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

// Swap the active voice card. spks_ is read UNLOCKED by feed() on the worker
// thread, so this must only be called while stopped — the Kotlin layer
// enforces it (switches gated on !running, and drained before nativeStart).
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
    // one stop wins; the exchange makes a concurrent stop/start pair unable
    // to retire the wrong (new) session
    Session* s = g_sess.exchange(nullptr);
    if (!s) return;
    retire_session(s);
    LOGI("stopped: played=%llds gaps=%lld",
         (long long)(g_played48.load() / STREAM_SR), (long long)g_gaps.load());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mean2vc_voice_MainActivity_nativeStatus(JNIEnv* env, jobject) {
    char buf[256];
    Session* s = g_sess.load();                   // null during the stop transition
    snprintf(buf, sizeof(buf),
             "proc=%.1fms feed=%.1fms buf=%.0fms gaps=%lld preroll=%lld drops=%ld "
             "in16=%lld out16=%lld played=%lld aerr=%d odrops=%ld",
             g_proc_ms.load(), g_feed_ms.load(),
             s ? s->buf48.load() / (STREAM_SR / 1000.0) : 0.0,
             (long long)g_gaps.load(), (long long)g_preroll.load(),
             s ? s->in_q->dropped.load() : 0L,
             (long long)g_nin16.load(), (long long)g_nout16.load(),
             (long long)g_played48.load(),
             s ? s->stream_err.load() : 0,
             s ? s->out_q->dropped.load() : 0L);
    return env->NewStringUTF(buf);
}
