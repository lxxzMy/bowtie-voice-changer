package com.mean2vc.voice

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaRecorder
import android.os.Bundle
import android.provider.OpenableColumns
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
import android.widget.Toast
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
    private var busy = false          // cloning in flight (incl. mic capture)
    private var recording = false     // mic capture live — 2nd knot press stops
    private var toggling = false      // start/stop transition in flight
    private var rec: AudioRecord? = null
    @Volatile private var recStop = false
    private var recThread: Thread? = null
    private val recBuf = java.io.ByteArrayOutputStream()
    private var suppressSpk = false   // swallow the selection event from setSelection
    private var spkExpected = 0       // spinner position the engine was switched to
    // ALL engine card swaps go through one FIFO thread — the last REQUESTED
    // switch always lands last in the engine, whatever the spinner callbacks do
    private val cardExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()
    private var inDevs: List<AudioDeviceInfo> = emptyList()
    private var outDevs: List<AudioDeviceInfo> = emptyList()
    private var cards = mutableListOf<String>()   // saved voice cards (display names)
    private var activeCard: String? = null        // null = built-in card
    private var leftMode = 0                      // left wheel slot (see onLeftMode)
    private lateinit var modelsDir: File
    private lateinit var spkDir: File
    private lateinit var cardsDir: File

    /** user-facing chatter goes to logcat only — the bowtie owns the screen */
    private fun say(msg: String) = android.util.Log.i("meanvc", msg)

    /** load a voice card into the engine. FIFO single thread: when several
     *  switches queue up (spinner echoes etc.), the last REQUESTED one is
     *  what the engine ends up with — never order-dependent. */
    private fun switchEngineCard(path: String, label: String) {
        cardExecutor.execute {
            val ok = nativeSetCard(path)
            handler.post {
                if (ok) say("音色已切换: $label")
                else say("切卡失败(见logcat)")
            }
        }
    }

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
                val aerr = Regex("aerr=(-?\\d+)").find(s)?.groupValues?.get(1)?.toIntOrNull() ?: 0
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
                if (pos == spkExpected) return     // programmatic echo, engine already switched
                val card = when {
                    pos == 0 -> null
                    pos - 1 < cards.size -> cards[pos - 1]
                    else -> return
                }
                if (card == activeCard) return     // engine already on this card
                spkExpected = pos
                val path = if (card == null) File(spkDir, "reference_1.spk.npy").absolutePath
                           else cardPath(card).absolutePath
                activeCard = card
                syncBowtie()
                switchEngineCard(path, card ?: "默认")
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

    /** today's earlier builds kept the single clone at spk/custom.spk.npy —
     *  fold it into the named card library once. */
    private fun migrateLegacyCard() {
        val legacy = File(spkDir, "custom.spk.npy")
        if (!legacy.exists()) return
        val dest = File(cardsDir, "克隆音色.spk.npy")
        cardsDir.mkdirs()
        if (dest.exists()) legacy.delete() else legacy.renameTo(dest)
    }

    private fun initEngine() {
        Thread {
            // cards/ must exist BEFORE the first nativeCloneCard — C fopen
            // does not create directories (a fresh install without the legacy
            // card used to fail with 写入音色卡失败)
            cardsDir.mkdirs()
            migrateLegacyCard()
            val ok = copyAssets() && nativeInit(
                modelsDir.absolutePath,
                File(spkDir, "reference_1.spk.npy").absolutePath, 1)
            handler.post inner@{
                ready = ok
                if (!ok) say("init failed")
                if (ok) {
                    val prefs = getSharedPreferences("app", MODE_PRIVATE)
                    if (!prefs.getBoolean("consented", false)) {
                        showConsent()
                        return@inner
                    }
                    populateDevices()
                    val echoListener = object : AdapterView.OnItemSelectedListener {
                        override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                        }
                        override fun onNothingSelected(p: AdapterView<*>?) {}
                    }
                    inSpin.onItemSelectedListener = echoListener
                    outSpin.onItemSelectedListener = echoListener
                    spkAdapter = ArrayAdapter(this,
                        android.R.layout.simple_spinner_item, mutableListOf("默认音色")).apply {
                        setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
                    }
                    spkSpin.adapter = spkAdapter
                    suppressSpk = true   // adapter attach fires a selection event
                    spkSpin.post { suppressSpk = false }   // …but not always; clear it regardless
                    refreshCards()
                    syncBowtie()
                    onLeftMode(0)          // prime the right wheel for 变声
                }
            }
        }.start()
    }

    /** copy the graphs + speaker card from APK assets to filesDir (ORT needs
     *  real paths; ~510 MB on first launch with the clone encoder). */
    private fun copyAssets(): Boolean {
        val dstRoot = filesDir
        return try {
            for (name in assets.list("models") ?: emptyArray()) {
                val dst = File(dstRoot, "models/$name")
                if (dst.exists() && dst.length() > 0) continue
                dst.parentFile?.mkdirs()
                assets.open("models/$name").use { input ->
                    dst.outputStream().use { input.copyTo(it) }
                }
            }
            val spk = File(dstRoot, "spk/reference_1.spk.npy")
            if (!spk.exists()) {
                spk.parentFile?.mkdirs()
                assets.open("spk/reference_1.spk.npy").use { input ->
                    spk.outputStream().use { input.copyTo(it) }
                }
            }
            true
        } catch (e: Exception) {
            say("asset copy failed: ${e.message}")
            false
        }
    }

    // ------------------------------------------------------------------ cards
    private fun cardPath(name: String) = File(cardsDir, "$name.spk.npy")

    private fun sanitizeName(s: String): String {
        val cleaned = s.replace(Regex("[\\\\/:*?\"<>|\\p{Cntrl}]"), "").trim()
        return when {
            cleaned.isEmpty() -> "未命名"
            else -> cleaned.take(32)
        }
    }

    private fun uniqueName(base: String): String {
        var n = base
        var k = 2
        while (cardPath(n).exists()) { n = "$base($k)"; k++ }
        return n
    }

    /** sequential auto-name for a freshly created card: 音色1, 音色2, … */
    private fun defaultCardName(): String {
        var k = 1
        while (cardPath("音色$k").exists()) k++
        return "音色$k"
    }

    private fun refreshCards() {
        cards.clear()
        cardsDir.listFiles { f -> f.isFile && f.name.endsWith(".spk.npy") }
            ?.sortedBy { it.lastModified() }
            ?.forEach { cards.add(it.name.removeSuffix(".spk.npy")) }
        val items = mutableListOf("默认音色")
        items.addAll(cards)
        if (!::spkAdapter.isInitialized) return
        spkAdapter.clear()
        spkAdapter.addAll(items)
        spkAdapter.notifyDataSetChanged()
        // a data change can echo a pos-0 selection event — swallow it once
        suppressSpk = true
        spkSpin.post { suppressSpk = false }
    }

    /** built-in mic + built-in speaker (or system defaults, which usually are
     *  exactly that) acoustically couple: the speaker's converted output is
     *  re-captured and re-converted. Headset, earpiece output, or platform
     *  AEC breaks the loop. */
    private fun askCardName(default: String, onOk: (String) -> Unit) {
        val input = EditText(this).apply {
            setText(default)
            setSelection(default.length)
        }
        AlertDialog.Builder(this)
            .setTitle("音色卡名称")
            .setView(input)
            .setPositiveButton("确定") { _, _ -> onOk(sanitizeName(input.text.toString())) }
            .setNegativeButton("取消", null)
            .show()
    }

    // ------------------------------------------------------------------ devices
    private fun populateDevices() {
        val am = getSystemService(AUDIO_SERVICE) as AudioManager
        inDevs = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
            .filter { it.isSource }.toList()
        outDevs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter { it.isSink }.toList()
        fun names(devs: List<AudioDeviceInfo>) = mutableListOf("系统默认").apply {
            for (d in devs) add("${devName(d.type)} id=${d.id} ${d.productName}")
        }
        inSpin.adapter = ArrayAdapter(this,
            android.R.layout.simple_spinner_item, names(inDevs)).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }
        outSpin.adapter = ArrayAdapter(this,
            android.R.layout.simple_spinner_item, names(outDevs)).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }
    }

    private fun devName(t: Int) = when (t) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "机身麦克风"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "机身扬声器"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE -> "机身扬声器(安全)"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "听筒"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳机带麦"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "有线耳机"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "蓝牙SCO"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "蓝牙A2DP"
        AudioDeviceInfo.TYPE_BLE_HEADSET -> "蓝牙LE耳机"
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> "蓝牙LE音箱"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB音频"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB耳机"
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB配件"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        AudioDeviceInfo.TYPE_LINE_ANALOG -> "模拟线路"
        AudioDeviceInfo.TYPE_LINE_DIGITAL -> "数字线路"
        AudioDeviceInfo.TYPE_TELEPHONY -> "通话线路"
        33 -> "回声参考(虚拟)"          // TYPE_ECHO_REFERENCE (hidden const)
        AudioDeviceInfo.TYPE_REMOTE_SUBMIX -> "远程混音(虚拟)"
        AudioDeviceInfo.TYPE_FM_TUNER -> "FM调谐器(虚拟)"
        AudioDeviceInfo.TYPE_HEARING_AID -> "助听器"
        else -> "类型$t"
    }

    // ------------------------------------------------------------------ start
    private fun selDevId(spin: Spinner, devs: List<AudioDeviceInfo>): Int =
        if (spin.selectedItemPosition > 0 && spin.selectedItemPosition <= devs.size)
            devs[spin.selectedItemPosition - 1].id else 0

    private fun setControls(on: Boolean) {
        inSpin.isEnabled = on
        outSpin.isEnabled = on
        spkSpin.isEnabled = on
    }

    /** enter/leave the call audio path. MODE_IN_COMMUNICATION engages the
     *  platform AEC; speakerphone reroutes communication audio from the
     *  (quiet) earpiece to the loudspeaker — the exact scenario OEMs tune
     *  their strongest AEC for. */
    private fun applyCommRouting(on: Boolean) {
        val am = getSystemService(AUDIO_SERVICE) as AudioManager
        try {
            if (on) {
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                if (Build.VERSION.SDK_INT >= 31) {
                    val spk = am.availableCommunicationDevices.firstOrNull {
                        it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
                    }
                    if (spk != null) am.setCommunicationDevice(spk)
                }
                // loudspeaker ALWAYS on when converting (user-confirmed).
                // Keep these statements OUT of any bare { } block — a bare
                // block is an uninvoked lambda and silently never runs.
                @Suppress("DEPRECATION") am.isSpeakerphoneOn = true
            } else {
                if (Build.VERSION.SDK_INT >= 31) am.clearCommunicationDevice()
                @Suppress("DEPRECATION") am.isSpeakerphoneOn = false
                am.mode = AudioManager.MODE_NORMAL
            }
        } catch (_: Exception) {}
    }

    private fun toggle() {
        if (busy || !ready || toggling) return
        if (running) {
            // nativeStop joins AAudio callback threads — can take seconds on a
            // wedged/rerouting audio HAL; NEVER on the main thread (ANR)
            toggling = true
            thread {
                nativeStop()
                applyCommRouting(false)
                handler.post {
                    toggling = false
                    running = false
                    syncBowtie()               // bowtie.running must drop NOW
                    setControls(true)
                    handler.removeCallbacks(poll)
                }
            }
            return
        }
        val inId = selDevId(inSpin, inDevs)
        val outId = selDevId(outSpin, outDevs)
        toggling = true
        say("starting…")
        thread {
            // ALWAYS the call path: VOICE_COMMUNICATION preset engages the
            // platform AEC (anti-howling on the loudspeaker), and the routing
            // below sends the output to the speakerphone. Streams open AFTER
            // the route (mode+route then open — a post-open reroute would
            // tear the streams down).
            applyCommRouting(true)
            val ok = nativeStart(40.0, inId, outId, true)
            handler.post {
                toggling = false
                running = ok
                syncBowtie()
                if (!ok) {
                    say("启动失败(设备不可用?详见logcat)")
                    applyCommRouting(false)
                    setControls(true)
                } else {
                    handler.postDelayed(poll, 250)
                }
            }
        }
    }

    // ------------------------------------------------------------------ clone
    /** 克隆 writes to the slot the right wheel points at: an occupied slot is
     *  OVERWRITTEN (same card, new voice, same name+position); an empty slot
     *  (one blank scrollable past each end of the list) CREATES a card.
     *  Recording is press-to-start / press-to-stop: the encoder takes any
     *  length ≥1 s (native floor, no fixed duration), 5-10 s recommended. */
    private fun startMicRecording() {
        val sr = 16000
        val minBuf = AudioRecord.getMinBufferSize(
            sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) { say("录音初始化失败"); return }
        val r = AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, sr,
            AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, minBuf * 4)
        if (r.state != AudioRecord.STATE_INITIALIZED) {
            r.release(); say("录音初始化失败"); return
        }
        if (inSpin.selectedItemPosition > 0 &&
            inSpin.selectedItemPosition <= inDevs.size)
            try { r.preferredDevice = inDevs[inSpin.selectedItemPosition - 1] }
            catch (_: Exception) {}
        rec = r
        recStop = false
        recBuf.reset()
        busy = true                    // capture + encode share the busy gate
        recording = true
        bowtie.cloneBusy = true        // amber + locked dials
        bowtie.recording = true
        setControls(false)
        say("录音中…再按一次中央键停止(至少1秒,建议5-10秒)")
        recThread = thread {
            r.startRecording()
            val buf = ByteArray(3200)                      // 100 ms chunks
            while (!recStop) {
                val n = r.read(buf, 0, buf.size)
                if (n <= 0) break
                synchronized(recBuf) { recBuf.write(buf, 0, n) }
                if (recBuf.size() >= sr * 2 * 60) {        // hard cap 60 s
                    handler.post { if (recording) stopMicRecording() }
                    break
                }
            }
            try { r.stop() } catch (_: Exception) {}
            r.release()
        }
    }

    /** second knot press: stop reading, then encode + write the card.
     *  Captures longer than 10 s are auto-trimmed to the loudest (most
     *  voiced) 10-second window before cloning. */
    private fun stopMicRecording() {
        if (!recording) return
        recording = false
        bowtie.recording = false
        recStop = true
        thread {
            recThread?.join(2000)              // let the reader drain its tail
            val captured = synchronized(recBuf) { recBuf.toByteArray() }
            if (captured.size < 16000 * 2) {   // <1 s — encoder rejects it
                handler.post {
                    busy = false; bowtie.cloneBusy = false
                    setControls(true)
                    say("录音太短(至少1秒),已放弃")
                }
                return@thread
            }
            val pcm = bestSlice16(captured, 16000, 10)
            val allSecs = String.format(java.util.Locale.US,
                "%.1f", captured.size / 32000.0)
            val useSecs = String.format(java.util.Locale.US,
                "%.1f", pcm.size / 32000.0)
            handler.post {
                say(if (pcm !== captured) "录音$allSecs 秒,自动截取最响亮的 $useSecs 秒,克隆中(首次需加载编码器,约几十秒)…"
                    else "录音完成(${useSecs}秒),克隆中(首次需加载编码器,约几十秒)…")
            }
            val cardIdx = bowtie.curSlot - 1          // voices[0] = 默认音色
            val name = if (cardIdx in cards.indices) cards[cardIdx] else defaultCardName()
            val out = File(filesDir, "ref_custom.wav")
            if (!writeWav16(out, pcm, 16000)) {
                handler.post {
                    busy = false; bowtie.cloneBusy = false
                    setControls(true)
                    say("录音写入失败")
                }
                return@thread
            }
            val oldMtime = if (cardIdx in cards.indices) cardPath(name).lastModified() else -1L
            cardPath(name).parentFile?.mkdirs()
            val err = nativeCloneCard(out.absolutePath, cardPath(name).absolutePath)
            handler.post { cloneDoneAt(cardIdx, name, oldMtime, err) }
        }
    }

    // ------------------------------------------------------- voiced trimming
    /** keep at most maxSecs of 16-bit-mono PCM: slide a 50 ms-energy window
     *  and take the loudest stretch — silence wastes the embedding */
    private fun bestSlice16(pcm: ByteArray, sr: Int, maxSecs: Int): ByteArray {
        val maxLen = sr * 2 * maxSecs
        if (pcm.size <= maxLen) return pcm
        val frame = 1600                              // 50 ms of 16-bit mono
        val frames = pcm.size / frame
        val wf = maxLen / frame
        if (frames < wf || wf == 0) return pcm
        val energy = DoubleArray(frames)
        for (k in 0 until frames) energy[k] = winEnergy(k, frame, pcm)
        var best = 0; var bestE = -1.0; var cur = 0.0
        for (k in 0..frames - wf) {                   // window [k, k+wf-1]
            cur += energy[k + wf - 1]
            if (k > 0) cur -= energy[k - 1]
            if (cur > bestE) { bestE = cur; best = k }
        }
        val off = best * frame
        return pcm.copyOfRange(off, off + wf * frame)
    }

    private fun winEnergy(frameIdx: Int, frame: Int, pcm: ByteArray): Double {
        var e = 0.0
        val o = frameIdx * frame
        var i = o
        while (i < o + frame) {
            val s = ((pcm[i].toInt() and 0xFF) or
                     (pcm[i + 1].toInt() shl 8)).toShort().toInt()
            e += s.toDouble() * s
            i += 2
        }
        return e
    }

    private class WavInfo(val sr: Int, val channels: Int, val bits: Int,
                          val isFloat: Boolean, val data: ByteArray)

    /** minimal RIFF/WAVE reader for what the encoder accepts anyway:
     *  16-bit PCM or 32-bit float, mono/stereo */
    // ---------------------------------------------------- any-format decode
    /** decode any MediaCodec-supported audio (mp3/m4a/aac/flac/ogg/opus/wav…)
     *  to a 16-bit mono 16 kHz wav at dst. Hard cap 120 s of decoded audio —
     *  the clone only ever uses the loudest 10 s anyway. */
    private fun decodeToWav16(src: File, dst: File): Boolean {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        var monoBuf = ShortArray(1 shl 18)
        var monoN = 0
        try {
            extractor.setDataSource(src.absolutePath)
            var ti = -1
            for (i in 0 until extractor.trackCount) {
                if ((extractor.getTrackFormat(i)
                        .getString(MediaFormat.KEY_MIME) ?: "").startsWith("audio/")) {
                    ti = i; break
                }
            }
            if (ti < 0) return false
            extractor.selectTrack(ti)
            val mime = extractor.getTrackFormat(ti)
                .getString(MediaFormat.KEY_MIME) ?: return false
            codec = MediaCodec.createDecoderByType(mime).apply {
                configure(extractor.getTrackFormat(ti), null, null, 0)
                start()
            }
            var rate = 44100
            var channels = 1
            var floatPcm = false
            fun readFormat() {
                val of = codec!!.outputFormat
                rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                floatPcm = (of.getInteger(MediaFormat.KEY_PCM_ENCODING,
                    android.media.AudioFormat.ENCODING_PCM_16BIT)
                    == android.media.AudioFormat.ENCODING_PCM_FLOAT)
            }
            val info = MediaCodec.BufferInfo()
            var eosIn = false
            var eosOut = false
            while (!eosOut) {
                if (!eosIn) {
                    val inIdx = codec.dequeueInputBuffer(10_000)
                    if (inIdx >= 0) {
                        val ib = codec.getInputBuffer(inIdx)!!
                        val sz = extractor.readSampleData(ib, 0)
                        if (sz < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            eosIn = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, sz,
                                extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                var outIdx = codec.dequeueOutputBuffer(info, 10_000)
                if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    readFormat()
                    outIdx = codec.dequeueOutputBuffer(info, 0)
                }
                while (outIdx >= 0) {
                    val ob = codec.getOutputBuffer(outIdx)!!
                    ob.order(java.nio.ByteOrder.LITTLE_ENDIAN)
                    ob.position(info.offset)
                    ob.limit(info.offset + info.size)
                    if (channels > 0) {
                        val need = (info.size / channels / 2) + 1
                        if (monoN + need > monoBuf.size)
                            monoBuf = monoBuf.copyOf(maxOf(monoBuf.size * 2, monoN + need))
                        if (floatPcm) {
                            while (ob.remaining() >= 4 * channels) {
                                var acc = 0f
                                for (c in 0 until channels) acc += ob.float
                                monoBuf[monoN++] = (acc / channels)
                                    .coerceIn(-1f, 1f).times(32767f).toInt().toShort()
                            }
                        } else {
                            while (ob.remaining() >= 2 * channels) {
                                var acc = 0
                                for (c in 0 until channels) acc += ob.short
                                monoBuf[monoN++] = (acc / channels).toShort()
                            }
                        }
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0)
                        eosOut = true
                    outIdx = codec.dequeueOutputBuffer(info, 0)
                    if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        readFormat()
                        outIdx = codec.dequeueOutputBuffer(info, 0)
                    }
                }
                if (rate > 0 && monoN >= 120L * rate) {   // 120 s decode cap
                    eosOut = true
                }
            }
            codec.stop()
            if (monoN < rate) return false                // <1 s of audio
            // mono @rate → 16 kHz: each output sample averages its source
            // window (cheap anti-alias box filter for the downsample)
            val outN = (monoN.toLong() * 16000L / rate).toInt()
            val pcm = ByteArray(outN * 2)
            var pos = 0
            for (i in 0 until outN) {
                val s0 = (i.toLong() * rate / 16000L).toInt()
                val s1 = maxOf(s0 + 1, ((i + 1).toLong() * rate / 16000L).toInt())
                var acc = 0L
                var n = 0
                var j = s0
                while (j < s1 && j < monoN) { acc += monoBuf[j].toInt(); n++; j++ }
                val v = if (n > 0) (acc / n).toInt() else 0
                pcm[pos] = (v and 0xFF).toByte()
                pcm[pos + 1] = ((v shr 8) and 0xFF).toByte()
                pos += 2
            }
            return writeWav16(dst, pcm, 16000)
        } catch (e: Exception) {
            say("解码失败: ${e.message}")
            return false
        } finally {
            try { codec?.release() } catch (_: Exception) {}
            try { extractor.release() } catch (_: Exception) {}
        }
    }

    private fun parseWav(f: File): WavInfo? {
        val b = try { f.readBytes() } catch (e: Exception) { return null }
        if (b.size < 44 ||
            String(b, 0, 4, Charsets.US_ASCII) != "RIFF" ||
            String(b, 8, 4, Charsets.US_ASCII) != "WAVE") return null
        fun u16(o: Int) = (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)
        fun u32(o: Int) = (u16(o) or (u16(o + 2) shl 16))
        var o = 12
        var fmt = -1; var ch = 0; var sr = 0; var bits = 0
        var data: ByteArray? = null
        while (o + 8 <= b.size) {
            val id = String(b, o, 4, Charsets.US_ASCII)
            val sz = u32(o + 4)
            when (id) {
                "fmt " -> { fmt = u16(o + 8); ch = u16(o + 10)
                            sr = u32(o + 12); bits = u16(o + 22) }
                "data" -> data = b.copyOfRange(o + 8, minOf(o + 8 + sz, b.size))
            }
            o += 8 + sz + (sz and 1)                  // chunks are word-aligned
        }
        val d = data ?: return null
        if ((fmt != 1 && fmt != 3) || (bits != 16 && bits != 32) ||
            ch !in 1..2 || sr <= 0) return null
        return WavInfo(sr, ch, bits, fmt == 3, d)
    }

    /** imported wavs longer than maxSecs are re-written to their loudest
     *  maxSecs window (same sr/channels/format); null = leave untouched */
    private fun trimWavFile(f: File, maxSecs: Int): ByteArray? {
        val w = parseWav(f) ?: return null
        val frameBytes = w.channels * w.bits / 8      // one sample-frame
        val bytesPerSec = w.sr * frameBytes
        val maxLen = bytesPerSec * maxSecs
        if (maxLen <= 0 || w.data.size <= maxLen) return null
        val win = 50 * bytesPerSec / 100              // 50 ms per energy frame
        if (win <= 0) return null
        val frames = w.data.size / win
        val wf = maxLen / win
        if (frames < wf) return null
        val energy = DoubleArray(frames)
        val sb = w.bits / 8
        for (k in 0 until frames) {
            var e = 0.0
            var i = k * win
            val end = i + win
            while (i + sb <= end) {
                val s: Double = if (w.isFloat) {
                    val acc = (w.data[i].toInt() and 0xFF) or
                              ((w.data[i + 1].toInt() and 0xFF) shl 8) or
                              ((w.data[i + 2].toInt() and 0xFF) shl 16) or
                              (w.data[i + 3].toInt() shl 24)
                    java.lang.Float.intBitsToFloat(acc).toDouble()
                } else {
                    ((w.data[i].toInt() and 0xFF) or
                     (w.data[i + 1].toInt() shl 8)).toShort().toDouble()
                }
                e += s * s
                i += sb
            }
            energy[k] = e
        }
        var best = 0; var bestE = -1.0; var cur = 0.0
        for (k in 0..frames - wf) {                   // window [k, k+wf-1]
            cur += energy[k + wf - 1]
            if (k > 0) cur -= energy[k - 1]
            if (cur > bestE) { bestE = cur; best = k }
        }
        val off = best * win
        val slice = w.data.copyOfRange(off, off + wf * win)
        val out = java.io.ByteArrayOutputStream()
        fun le16(v: Int) { out.write(v); out.write(v shr 8) }
        fun le32(v: Int) { out.write(v); out.write(v shr 8); out.write(v shr 16); out.write(v shr 24) }
        out.write("RIFF".toByteArray()); le32(36 + slice.size)
        out.write("WAVE".toByteArray())
        out.write("fmt ".toByteArray()); le32(16)
        le16(if (w.isFloat) 3 else 1); le16(w.channels)
        le32(w.sr); le32(bytesPerSec); le16(frameBytes); le16(w.bits)
        out.write("data".toByteArray()); le32(slice.size)
        out.write(slice)
        return out.toByteArray()
    }

    override fun onActivityResult(req: Int, res: Int, data: Intent?) {
        super.onActivityResult(req, res, data)
        if (req != 2 || res != RESULT_OK || data?.data == null) return
        if (busy || running || !ready) return
        busy = true
        setControls(false)
        say("克隆中(首次需加载编码器,约几十秒)…")
        val cardIdx = bowtie.curSlot - 1          // voices[0] = 默认音色
        val name = if (cardIdx in cards.indices) cards[cardIdx] else defaultCardName()
        thread {
            try {
                val f = File(filesDir, "ref_import.tmp")
                contentResolver.openInputStream(data.data!!)!!.use { input ->
                    f.outputStream().use { input.copyTo(it) }
                }
                // picked display name (card files keep their name)
                var pickedName = "imported"
                contentResolver.query(data.data!!,
                    arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                    if (c.moveToFirst()) pickedName = c.getString(0) ?: pickedName
                }
                // voice-card file (.spk.npy / npy magic): straight into the library
                if (isNpyCard(f) || pickedName.endsWith(".spk.npy")
                    || pickedName.endsWith(".npy")) {
                    val base = sanitizeName(pickedName
                        .removeSuffix(".spk.npy").removeSuffix(".npy")
                        .ifEmpty { "导入音色" })
                    val name = uniqueName(base)
                    val dst = cardPath(name)
                    dst.parentFile?.mkdirs()
                    f.copyTo(dst, overwrite = true)
                    // nativeSetCard is the real validator: loads the 256-dim
                    // embedding and reports failure for junk files
                    cardExecutor.execute {
                        val ok = nativeSetCard(dst.absolutePath)
                        handler.post {
                            if (!ok) dst.delete()
                            cardImportDone(name, if (ok) null else "不是有效的音色卡文件")
                        }
                    }
                    return@thread
                }
                var working = f
                if (parseWav(f) == null) {            // mp3/m4a/flac/ogg/… → decode
                    val dec = File(filesDir, "ref_decoded.wav")
                    say("解码音频中(mp3/m4a/flac/ogg…)…")
                    if (!decodeToWav16(f, dec)) {
                        handler.post {
                            cloneDoneAt(cardIdx, name, -1L,
                                "无法解码该音频(支持wav/mp3/m4a/flac/ogg等)")
                        }
                        return@thread
                    }
                    working = dec
                    say("已解码转换为wav(16k单声道)")
                }
                val trimmed = trimWavFile(working, 10)   // >10 s → loudest 10 s
                if (trimmed != null) {
                    working.writeBytes(trimmed)
                    say("音频超过10秒,已自动截取最响亮的10秒")
                }
                val oldMtime = if (cardIdx in cards.indices) cardPath(name).lastModified() else -1L
                cardPath(name).parentFile?.mkdirs()
                val err = nativeCloneCard(working.absolutePath,
                    cardPath(name).absolutePath)
                handler.post { cloneDoneAt(cardIdx, name, oldMtime, err) }
            } catch (e: Exception) {
                handler.post { cloneDoneAt(cardIdx, name, -1L, e.message ?: "读取文件失败") }
            }
        }
    }

    /** npy magic check: a .spk.npy voice card is a 256-dim float32 npy */
    private fun isNpyCard(f: File): Boolean = try {
        val b = ByteArray(8)
        java.io.FileInputStream(f).use { it.read(b) == 8 } &&
            b[0] == 0x93.toByte() && b[1] == 'N'.code.toByte() &&
            b[2] == 'U'.code.toByte() && b[3] == 'M'.code.toByte() &&
            b[4] == 'P'.code.toByte() && b[5] == 'Y'.code.toByte()
    } catch (e: Exception) {
        false
    }

    /** runs on the UI thread — result of importing a .spk.npy card file */
    private fun cardImportDone(name: String, err: String?) {
        busy = false
        bowtie.cloneBusy = false
        setControls(true)
        if (err != null) {
            say("音色卡导入失败: $err")
            return
        }
        refreshCards()
        val idx = cards.indexOf(name)
        val selectTo = if (idx >= 0) idx + 1 else 1
        activeCard = name
        spkExpected = selectTo
        if (spkSpin.selectedItemPosition != selectTo)
            spkSpin.setSelection(selectTo)
        say("已导入音色卡: $name")
        bowtie.voiceIndex = spkSpin.selectedItemPosition
        syncBowtie()
    }

    /** runs on the UI thread */
    private fun cloneDoneAt(cardIdx: Int, name: String, oldMtime: Long, err: String?) {
        busy = false
        bowtie.cloneBusy = false
        setControls(true)
        if (err != null) {
            say("克隆失败: $err")
            return
        }
        if (oldMtime > 0) {
            // overwrite: restore the old mtime so the card keeps its slot
            cardPath(name).setLastModified(oldMtime)
        }
        refreshCards()
        val idx = cards.indexOf(name)
        val selectTo = if (idx >= 0) idx + 1 else 1
        activeCard = name
        // switch the engine DIRECTLY — never depend on the spinner callback
        // (setSelection right after a data change can silently not fire,
        // which left the default card loaded while the UI showed the new one)
        spkExpected = selectTo
        switchEngineCard(cardPath(name).absolutePath, name)
        if (spkSpin.selectedItemPosition != selectTo)
            spkSpin.setSelection(selectTo)                // UI sync only
        say(if (oldMtime > 0) "已覆盖音色卡: $name" else "已写入音色卡: $name")
        // stay in 克隆 mode — the wheel follows to the card just written
        bowtie.voiceIndex = spkSpin.selectedItemPosition
        syncBowtie()
    }

    /** Apache-2.0 attribution + consent gate. Decline = exit. */
    private fun showConsent() {
        AlertDialog.Builder(this)
            .setTitle("蝴蝶结变声器")
            .setMessage(
                "语音转换引擎基于 ASLP-lab 开源项目 MeanVC2" +
                "(Apache License 2.0) 构建\n" +
                "github.com/ASLP-lab/MeanVC2\n\n" +
                "模型致谢:F5-TTS · Vocos · WavLM · ECAPA-TDNN · " +
                "WeNet · s3prl · FunASR\n" +
                "推理运行时:ONNX Runtime (MIT)\n\n" +
                "使用声明:克隆他人音色前须获得本人明确同意;" +
                "严禁用于仿冒、欺诈或误导性内容。")
            .setPositiveButton("同意") { _, _ ->
                getSharedPreferences("app", MODE_PRIVATE)
                    .edit().putBoolean("consented", true).apply()
            }
            .setNegativeButton("拒绝并退出") { _, _ -> finish() }
            .show()
    }

    // ------------------------------------------------------------ bowtie glue
    private fun showAbout() {
        AlertDialog.Builder(this)
            .setTitle("蝴蝶结变声器")
            .setMessage(
                "语音转换引擎基于 ASLP-lab 开源项目 MeanVC2\n" +
                "(Apache License 2.0) 构建\n" +
                "github.com/ASLP-lab/MeanVC2\n\n" +
                "模型致谢:F5-TTS · Vocos · WavLM · ECAPA-TDNN\n" +
                "WeNet · s3prl · FunASR\n" +
                "推理运行时:ONNX Runtime (MIT)\n\n" +
                "使用声明:克隆他人音色前须获得本人明确同意;\n" +
                "严禁用于仿冒、欺诈或误导性内容。")
            .setPositiveButton("关闭", null)
            .show()
    }

    /** mirror app state onto the bowtie drawing */
    private fun syncBowtie() {
        bowtie.running = running
        bowtie.cloneBusy = busy
        bowtie.voiceIndex = spkSpin.selectedItemPosition
    }

    /** right wheel always shows the voice cards */
    private fun rightList(): List<String> = voiceCards()

    private fun voiceCards(): List<String> = listOf("默认音色") + cards

    /** left wheel picked a function slot: 0 变声 / 1 克隆 */
    private fun onLeftMode(idx: Int) {
        leftMode = idx
        bowtie.plusOk = (idx == 1)             // 克隆: 导入/新建 slots reachable
        say("left mode $idx: " + leftWheelName(idx))
    }

    private fun leftWheelName(idx: Int) = listOf("变声", "克隆")[idx]

    private fun setRightVoices(plusOk: Boolean) {
        bowtie.plusOk = plusOk
        bowtie.voiceIndex = spkSpin.selectedItemPosition
    }

    private fun setRightDevs(input: Boolean) {
        bowtie.plusOk = false
        bowtie.devIndex = (if (input) inSpin else outSpin).selectedItemPosition
    }

    /** right wheel settled on an entry */
    private fun onRightSelect(idx: Int) {
        say("right select $idx: " + (rightList().getOrNull(idx) ?: "?"))
        if (idx != spkSpin.selectedItemPosition)
            spkSpin.setSelection(idx)              // listener swaps the card
    }

    /** center button: 变声 mode = start/stop; 克隆 mode writes the arrow slot
     *  (head slot → import a wav, real card → record-overwrite, tail slot →
     *  record-create). Recording: 1st press starts the mic, 2nd press stops
     *  and clones whatever was captured (≥1 s). */
    private fun onKnot() {
        if (!ready || toggling) return
        if (recording) { stopMicRecording(); return }
        if (busy) return
        when (leftMode) {
            1 -> when {
                bowtie.curSlot == -1 -> importWav()
                bowtie.curSlot == 0 ->
                    say("默认音色不支持克隆写入,请指向一张克隆卡")
                else -> startMicRecording()
            }
            else -> toggle()
        }
    }

    /** head slot 「从文件导入」: pick a wav/audio file from storage */
    private fun importWav() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"       // audio + .spk.npy voice-card files
        }
        try {
            // transient on-screen hint over the picker (auto-dismisses)
            Toast.makeText(this, "支持 wav/mp3/m4a/flac/ogg/opus/ .spk.npy",
                Toast.LENGTH_LONG).show()
            startActivityForResult(
                Intent.createChooser(i, "选择参考音频或音色卡"), 2)
        } catch (e: Exception) {
            say("没有可用的文件选择器: ${e.message}")
        }
    }

    /** rename the card at right-wheel slot s (voices index; 0 = 默认音色) */
    private fun renameAt(s: Int) {
        if (running || busy) { say("变声/克隆中不可重命名"); return }
        if (s == 0) { say("默认音色不可重命名"); return }
        if (s - 1 !in cards.indices) { say("没有可重命名的音色卡"); return }
        val cur = cards[s - 1]
        askCardName(cur) { newName ->
            if (newName == cur || !cardPath(cur).exists()) return@askCardName
            val target = if (cardPath(newName).exists()) uniqueName(newName) else newName
            if (cardPath(cur).renameTo(cardPath(target))) {
                if (activeCard == cur) activeCard = target
                refreshCards()
                syncBowtie()
                say("已重命名: $cur -> $target")
            } else say("重命名失败")
        }
    }

    /** delete the card at right-wheel slot s (voices index; 0 = 默认音色) */
    private fun deleteAt(s: Int) {
        if (running || busy) { say("变声/克隆中不可删除"); return }
        if (s == 0) { say("默认音色不可删除"); return }
        if (s - 1 !in cards.indices) { say("没有可删除的音色卡"); return }
        val cur = cards[s - 1]
        AlertDialog.Builder(this)
            .setMessage("删除音色卡「$cur」?")
            .setPositiveButton("删除") { _, _ ->
                val wasActive = activeCard == cur
                if (!cardPath(cur).delete()) { say("删除失败"); return@setPositiveButton }
                refreshCards()
                if (wasActive) {
                    activeCard = null
                    spkExpected = 0
                    switchEngineCard(File(spkDir, "reference_1.spk.npy").absolutePath, "默认")
                    if (spkSpin.selectedItemPosition != 0) spkSpin.setSelection(0)
                    say("已删除: $cur(已切回默认音色)")
                } else syncBowtie()
                say("已删除: $cur")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun writeWav16(out: File, pcm: ByteArray, sr: Int): Boolean = try {
        DataOutputStream(BufferedOutputStream(out.outputStream())).use { d ->
            val dataLen = pcm.size
            fun leInt(v: Int) { d.write(v); d.write(v shr 8); d.write(v shr 16); d.write(v shr 24) }
            fun leShort(v: Int) { d.write(v); d.write(v shr 8) }
            d.write("RIFF".toByteArray()); leInt(36 + dataLen); d.write("WAVE".toByteArray())
            d.write("fmt ".toByteArray()); leInt(16)
            leShort(1); leShort(1)                     // PCM, mono
            leInt(sr); leInt(sr * 2); leShort(2); leShort(16)
            d.write("data".toByteArray()); leInt(dataLen)
            d.write(pcm)
        }
        true
    } catch (e: Exception) {
        false
    }

    override fun onDestroy() {
        if (running) thread { nativeStop() }
        recStop = true                   // an open capture thread exits on its own
        handler.removeCallbacks(poll)
        super.onDestroy()
    }

    companion object {
        init { System.loadLibrary("onnxruntime"); System.loadLibrary("meanvc") }
        @JvmStatic external fun nativeInit(modelsDir: String, spkPath: String, ortThreads: Int): Boolean
        @JvmStatic external fun nativeStart(preRollMs: Double, inDevId: Int, outDevId: Int, aec: Boolean): Boolean
        @JvmStatic external fun nativeStop()
        @JvmStatic external fun nativeStatus(): String
        /** returns null on success, else the error message */
        @JvmStatic external fun nativeCloneCard(wavPath: String, outNpy: String): String?
        @JvmStatic external fun nativeSetCard(path: String): Boolean
    }
}
