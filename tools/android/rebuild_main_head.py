"""Rebuild the structurally-broken head of MainActivity.kt (lines 1-177)."""
import io
p = "MainActivity.kt"
s = io.open(p, encoding="utf-8").read()
lines = s.split("\n")

# everything from the start of initEngine (line 177) to EOF is the surviving
# body; only the head (class opening through onRequestPermissionsResult) needs
# reconstruction.
body_start = None
d = 0
for i, ln in enumerate(lines, 1):
    for ch in ln:
        if ch == "{": d += 1
        elif ch == "}": d -= 1
    # initEngine body starts around 177
body = "\n".join(lines[176:])   # from "    private fun initEngine() {" (0-idx 176)

head = '''package com.mean2vc.voice

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Bundle
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import java.io.File
import java.io.DataOutputStream
import java.io.BufferedOutputStream
import kotlin.concurrent.thread

class MainActivity : Activity() {

    private lateinit var inSpin: Spinner
    private lateinit var outSpin: Spinner
    private lateinit var spkSpin: Spinner
    private lateinit var bowtie: BowtieView
    private lateinit var spkAdapter: ArrayAdapter<String>
    private val handler = Handler(Looper.getMainLooper())
    private var ready = false
    private var running = false
    private var busy = false          // cloning in flight
    private var toggling = false      // start/stop transition in flight
    private var suppressSpk = false   // swallow the selection event from setSelection
    private var inDevs: List<AudioDeviceInfo> = emptyList()
    private var outDevs: List<AudioDeviceInfo> = emptyList()
    private var cards = mutableListOf<String>()   // saved voice cards (display names)
    private var activeCard: String? = null        // null = built-in card
    private var headStamp = 1_000L                // mtime pin for head-inserts
    private var leftMode = 0                      // left wheel slot (see onLeftMode)
    private lateinit var modelsDir: File
    private lateinit var spkDir: File
    private lateinit var cardsDir: File

    /** user-facing chatter goes to logcat only — the bowtie owns the screen */
    private fun say(msg: String) = android.util.Log.i("meanvc", msg)

    private val poll = object : Runnable {
        private var n = 0
        override fun run() {
            syncBowtie()
            val s = nativeStatus()
            // runtime metrics live in logcat only — the bowtie owns the screen
            if (n++ % 8 == 0) android.util.Log.i("meanvc", s)
            // AAudio disconnect (routing change etc.) kills the callbacks and
            // freezes the counters — stop cleanly instead of hanging forever
            if (running) {
                val aerr = Regex("aerr=(-?\\\\d+)").find(s)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                if (aerr != 0) {
                    toggle()   // stop path (async)
                    say("音频流被系统断开(路由变化,aerr=$aerr),已自动停止——请重新 Start")
                    return
                }
            }
            handler.postDelayed(this, 250)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // immersive: the bowtie owns the whole screen
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (android.view.View.SYSTEM_UI_FLAG_LOW_PROFILE
            or android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
            or android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)
        val pad = (12 * resources.displayMetrics.density).toInt()
        fun label(t: String) = TextView(this).apply { text = t }
        fun spinner() = Spinner(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT)
        }
        inSpin = spinner()
        outSpin = spinner()
        spkSpin = spinner()
        val legacy = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
            addView(label("输入设备")); addView(inSpin)
            addView(label("输出设备")); addView(outSpin)
            addView(label("音色(合成用)")); addView(spkSpin)
        }
        legacy.visibility = View.GONE          // state holders; the bowtie drives them

        // the bowtie: left wheel = function, right wheel = context, knot = action
        bowtie = BowtieView(this).apply {
            onKnotTap = { onKnot() }
            onLeftMode = { idx -> onLeftMode(idx) }
            onRightSelect = { idx -> onRightSelect(idx) }
            onRenameTap = { renameAt(bowtie.curSlot) }
            onDeleteTap = { deleteAt(bowtie.curSlot) }
            rightListProvider = { rightList() }
        }
        val info = TextView(this).apply {
            text = "ⓘ"
            textSize = 15f
            setTextColor(0xFF9AA3B5.toInt())
            setPadding(18, 12, 24, 24)
            setOnClickListener { showAbout() }
        }
        val frame = FrameLayout(this)
        frame.addView(legacy, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        frame.addView(bowtie, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        setContentView(frame)

        spkSpin.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                if (suppressSpk) { suppressSpk = false; return }
                if (!ready || running || busy) return
                val path = when {
                    pos == 0 -> File(spkDir, "reference_1.spk.npy").absolutePath
                    pos - 1 < cards.size -> cardPath(cards[pos - 1]).absolutePath
                    else -> return
                }
                activeCard = if (pos == 0) null else cards[pos - 1]
                syncBowtie()
                thread {
                    val ok = nativeSetCard(path)
                    handler.post {
                        if (ok) say("音色已切换: ${activeCard ?: "默认"}")
                        else say("切卡失败(见logcat)")
                    }
                }
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }

        modelsDir = File(filesDir, "models")
        spkDir = File(filesDir, "spk")
        cardsDir = File(filesDir, "cards")

        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1)
            return
        }
        initEngine()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == 1 &&
            grantResults.isNotEmpty() &&
            grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            initEngine()
        } else {
            say("mic permission denied")
        }
    }

'''
io.open(p, "w", encoding="utf-8").write(head + body)
print("head rebuilt; total lines:", len((head + body).split(chr(10))))
