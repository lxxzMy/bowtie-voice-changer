package com.mean2vc.voice

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * The bowtie device, hand-drawn in Canvas after the soft-3D reference.
 *
 * LEFT wheel — the FUNCTION wheel (8 slots, free spin, snap on release):
 *   合成 / 克隆 / WAV克隆 / 重命名 / 删除 / 输入设备 / 输出设备 / 外放
 * The label under its arrow (outer side) is the active mode; fires
 * onLeftMode(slot).
 *
 * RIGHT wheel — the CONTEXT wheel; its content depends on the left mode
 * (voice cards / input devices / output devices — provided by
 * rightListProvider). Scrolling-window belt over the full list: entry v sits
 * at 180°+(v-winW)·45°; the slot facing the module is HIDDEN and the belt
 * slips one entry per detent through it, so lists of any length scroll.
 * The fixed arrow (outer side) marks the selection; fires onRightSelect(index).
 * In clone mode (+ok) one empty slot past each end is reachable: the head
 * slot is engraved 「从文件导入」, the tail slot is a big "+" — what the
 * knot writes into.
 *
 * Center module button = the action for the current left mode (start/stop,
 * record, pick wav, rename, delete, toggle speakerphone). While converting
 * or cloning both wheels are LOCKED; the knot and the two card buttons
 * stay active.
 */
class BowtieView(ctx: Context) : View(ctx) {

    var onLeftMode: ((Int) -> Unit)? = null
    var onRightSelect: ((Int) -> Unit)? = null
    var onKnotTap: (() -> Unit)? = null
    var onRenameTap: (() -> Unit)? = null
    var onDeleteTap: (() -> Unit)? = null
    var rightListProvider: (() -> List<String>)? = null

    var leftIndex: Int = 0                     // 0 = 变声, 1 = 克隆
        set(v) { field = v; rotL = -v * 45f; invalidate() }

    /** sync the right wheel to the selected voice (voice modes) */
    var voiceIndex: Int = 0
        set(v) {
            field = v; winW = (v - 4).toFloat(); lastSel = v; curSlot = v
            android.util.Log.i("meanvc", "voiceIndex=$v winW=$winW")
            invalidate()
        }

    /** sync the right wheel to the selected device (device modes) */
    var devIndex: Int = 0
        set(v) { field = v; winW = (v - 4).toFloat(); lastSel = v; curSlot = v; invalidate() }

    /** clone mode: the 从文件导入 / + slots past the list ends become
     *  reachable; leaving clone mode re-clamps so the arrow never rests
     *  outside the real list */
    var plusOk: Boolean = false
        set(v) {
            field = v
            val n = (rightListProvider?.invoke() ?: emptyList()).size
            val minW = if (v) -5f else -4f
            val maxW = maxOf(if (v) n - 4f else n - 5f, minW)
            winW = winW.coerceIn(minW, maxW)
            curSlot = Math.round(winW).toInt() + 4
            android.util.Log.i("meanvc",
                "plusOk=$v n=$n winW=$winW curSlot=$curSlot")
            invalidate()
        }

    var cloneBusy: Boolean = false
        set(v) { field = v; invalidate() }

    /** mic capture in progress (2nd knot press stops it and clones) */
    var recording: Boolean = false
        set(v) { field = v; invalidate() }
    var running: Boolean = false
        set(v) { field = v; invalidate() }

    /** the slot (index into the current right list) the arrow points at;
     *  -1 / n are the empty + slots (clone target) */
    var curSlot: Int = 0
        private set

    private var w = 0f; private var h = 0f
    private var cx = 0f; private var cy = 0f
    private var knotRect = RectF()
    private var btnR = 0f
    private var btnCy = 0f                     // dome center y (hit-test)
    private var btnRenameRect = RectF()
    private var btnDeleteRect = RectF()
    private var ledCy = 0f
    private var pendingBtn = 0                 // 0 none, 1 rename, 2 delete
    private var dialL = PointF(); private var dialR = PointF()
    private var dialR_ = 0f

    private val leftLabels = listOf("变声", "克隆")

    // rotations (degrees, follow the finger, snap on release)
    private var rotL = 0f
    private var winW = -4f                  // right belt window start (continuous)
    private var lastSel = 0                 // last fired right-list index
    private var dragDial = 0
    private var knotDown = false
    private var dragLastAng = 0f
    private var downTime = 0L
    private var downX = 0f; private var downY = 0f

    private val bgPaint = Paint()
    private val shadowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val wingPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val wingHiPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val foldPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 8f; color = 0x2E8E1016
        strokeCap = Paint.Cap.ROUND
    }
    private val modulePaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val moduleHiPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val btnPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val btnSeamPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 6f; color = 0x66000000
    }
    private val dialFace = Paint(Paint.ANTI_ALIAS_FLAG)
    private val dentDark = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 3f; color = 0x2E000000
        strokeCap = Paint.Cap.ROUND
    }
    private val dentLight = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2f; color = 0x1FFFFFFF
        strokeCap = Paint.Cap.ROUND
    }
    private val arrowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ledPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val smallBtnPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val plusPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF9AA3B5.toInt(); strokeCap = Paint.Cap.ROUND
    }
    private val wheelTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textAlign = Paint.Align.CENTER
    }
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textAlign = Paint.Align.CENTER
    }

    /** overall bowtie scale (user: a bit too small) — applied to every
     *  layout constant; drawWing authors in unscaled design units */
    private val K = 1.15f

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        this.w = w.toFloat(); this.h = h.toFloat()
        cx = this.w * 0.5f; cy = this.h * 0.46f
        knotRect = RectF(cx - this.w * 0.058f * K, cy - this.h * 0.195f * K,
                         cx + this.w * 0.058f * K, cy + this.h * 0.195f * K)
        btnR = knotRect.width() * 0.30f
        dialR_ = this.h * 0.165f * K
        dialL = PointF(this.w * 0.5f - this.w * 0.175f * K, cy)
        dialR = PointF(this.w * 0.5f + this.w * 0.175f * K, cy)
        bgPaint.shader = LinearGradient(0f, 0f, 0f, this.h,
            0xFFFCFCFD.toInt(), 0xFFEEF0F4.toInt(), Shader.TileMode.CLAMP)
    }

    override fun onDraw(c: Canvas) {
        if (w == 0f) return
        c.drawRect(0f, 0f, w, h, bgPaint)

        // ground shadow
        c.save()
        c.translate(cx, cy + h * 0.27f * K)
        c.scale(1f, h * 0.10f / (w * 0.28f))
        shadowPaint.shader = RadialGradient(0f, 0f, w * 0.28f * K,
            0x44000000.toInt(), 0x00000000.toInt(), Shader.TileMode.CLAMP)
        c.drawCircle(0f, 0f, w * 0.28f * K, shadowPaint)
        c.restore()

        // running glow behind the module (cloning signals via the LED only)
        if (running) {
            glowPaint.shader = RadialGradient(cx, cy, w * 0.075f * K,
                0x59FF6B60.toInt(), 0x00FF6B60.toInt(), Shader.TileMode.CLAMP)
            c.drawCircle(cx, cy, w * 0.075f * K, glowPaint)
        }

        drawWing(c, mirror = false)
        drawWing(c, mirror = true)

        drawModule(c)

        drawWheel(c, dialL, rotL, leftLabels, outside = 180f)
        drawBelt(c, dialR, rightListProvider?.invoke() ?: emptyList(),
            winW, outside = 0f)
    }

    private fun drawWing(c: Canvas, mirror: Boolean) {
        val s = if (mirror) -1f else 1f
        fun X(o: Float) = cx + s * o * K          // design units → screen
        fun Y(o: Float) = cy + o * K
        val kx = w * 0.048f          // top inner corner (at the module edge)
        val ox = w * 0.27f
        val p = Path()
        p.moveTo(X(kx), Y(-h * 0.165f))
        p.cubicTo(X(w * 0.09f), Y(-h * 0.25f),
                  X(ox - w * 0.05f), Y(-h * 0.29f),
                  X(ox - w * 0.016f), Y(-h * 0.28f))
        p.quadTo(X(ox + w * 0.010f), Y(-h * 0.26f),
                 X(ox + w * 0.005f), Y(-h * 0.11f))
        p.cubicTo(X(ox + w * 0.009f), Y(h * 0.04f),
                  X(ox + w * 0.009f), Y(h * 0.07f),
                  X(ox), Y(h * 0.20f))
        // bottom edge sweeps in and TUCKS BEHIND the module — the wing must
        // read as passing behind the center block (no tails, no spikes)
        p.cubicTo(X(ox - w * 0.05f), Y(h * 0.285f),
                  X(w * 0.11f), Y(h * 0.19f),
                  X(w * 0.048f), Y(h * 0.16f))
        p.close()
        wingPaint.shader = LinearGradient(0f, cy - h * 0.27f * K, 0f, cy + h * 0.27f * K,
            0xFFF04048.toInt(), 0xFFB11218.toInt(), Shader.TileMode.CLAMP)
        c.drawPath(p, wingPaint)
        val hi = Path()
        hi.moveTo(X(kx), Y(-h * 0.165f))
        hi.cubicTo(X(w * 0.09f), Y(-h * 0.25f),
                  X(ox - w * 0.05f), Y(-h * 0.29f),
                  X(ox - w * 0.016f), Y(-h * 0.28f))
        hi.quadTo(X(ox + w * 0.010f), Y(-h * 0.26f),
                 X(ox + w * 0.005f), Y(-h * 0.11f))
        hi.cubicTo(X(ox + w * 0.004f), Y(-h * 0.06f),
                   X(w * 0.13f), Y(-h * 0.11f),
                   X(kx), Y(-h * 0.04f))
        hi.close()
        wingHiPaint.shader = LinearGradient(0f, cy - h * 0.27f * K, 0f, cy - h * 0.02f * K,
            0x52FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP)
        c.drawPath(hi, wingHiPaint)
        c.drawLine(X(w * 0.06f), Y(-h * 0.10f), X(w * 0.10f), Y(-h * 0.17f), foldPaint)
        c.drawLine(X(w * 0.06f), Y(h * 0.08f), X(w * 0.10f), Y(h * 0.15f), foldPaint)
    }

    private fun drawModule(c: Canvas) {
        val kr = knotRect
        // the shell stays red in every state — the LED below carries the
        // color code (green 变声中 / amber 克隆中)
        modulePaint.shader = LinearGradient(0f, kr.top, 0f, kr.bottom,
            if (running) 0xFFF0555C.toInt() else 0xFFE42A32.toInt(),
            if (running) 0xFFB11218.toInt() else 0xFFA50F16.toInt(),
            Shader.TileMode.CLAMP)
        c.drawRoundRect(kr, kr.width() * 0.34f, kr.width() * 0.34f, modulePaint)
        moduleHiPaint.shader = LinearGradient(0f, kr.top, 0f, kr.centerY(),
            0x3CFFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP)
        c.drawRoundRect(RectF(kr.left + 12, kr.top + 10, kr.right - 12, kr.centerY()),
            kr.width() * 0.30f, kr.width() * 0.30f, moduleHiPaint)
        // dome button — glossy BLACK on the red shell (same family as the
        // 重命名/删除 buttons below the LED)
        val domeCy = kr.top + kr.height() * 0.30f
        btnCy = domeCy
        btnR = kr.width() * 0.24f
        c.drawCircle(cx, domeCy, btnR + 10f, btnSeamPaint)
        btnPaint.shader = RadialGradient(cx - btnR * 0.3f, domeCy - btnR * 0.45f,
            btnR * 1.7f, 0xFF5C5C68.toInt(), 0xFF0E0E14.toInt(),
            Shader.TileMode.CLAMP)
        c.drawCircle(cx, domeCy, btnR, btnPaint)
        // work indicator LED: green = converting, amber = cloning, dark = idle
        // (lowered — it must not touch the dome button)
        ledCy = kr.top + kr.height() * 0.60f
        val ledR = kr.width() * 0.065f
        ledPaint.shader = RadialGradient(cx - ledR * 0.3f, ledCy - ledR * 0.3f,
            ledR * 1.6f,
            when {
                cloneBusy -> 0xFFFFD98A.toInt()
                running -> 0xFF7CE87C.toInt()
                else -> 0xFF6E1015.toInt()
            },
            when {
                cloneBusy -> 0xFFB07820.toInt()
                running -> 0xFF1D7A22.toInt()
                else -> 0xFF420407.toInt()
            },
            Shader.TileMode.CLAMP)
        c.drawCircle(cx, ledCy, ledR, ledPaint)
        // 重命名 / 删除: BLACK physical buttons (dimmed while locked)
        val dim = (running || cloneBusy)
        val dimA = if (dim) 110 else 255
        val bw = kr.width() * 0.40f
        val bh = kr.height() * 0.115f
        val by = kr.top + kr.height() * 0.775f
        btnRenameRect = RectF(cx - bw - 6, by - bh / 2, cx - 6, by + bh / 2)
        btnDeleteRect = RectF(cx + 6, by - bh / 2, cx + bw + 6, by + bh / 2)
        for (br in listOf(btnRenameRect, btnDeleteRect)) {
            smallBtnPaint.shader = LinearGradient(0f, br.top, 0f, br.bottom,
                0xFF4A4A56.toInt(), 0xFF12121A.toInt(), Shader.TileMode.CLAMP)
            smallBtnPaint.alpha = dimA
            c.drawRoundRect(br, bh * 0.35f, bh * 0.35f, smallBtnPaint)
            val bez = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                style = Paint.Style.STROKE; strokeWidth = 4f; color = 0x55000000
            }
            c.drawRoundRect(br, bh * 0.35f, bh * 0.35f, bez)
            val gl = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                shader = LinearGradient(0f, br.top, 0f, br.centerY(),
                    0x30FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP)
            }
            c.drawRoundRect(RectF(br.left + 6, br.top + 4, br.right - 6, br.centerY()),
                bh * 0.3f, bh * 0.3f, gl)
        }
        textPaint.textSize = w * 0.011f
        textPaint.color = 0xFFE8E8EE.toInt()
        textPaint.alpha = dimA
        textPaint.setShadowLayer(4f, 0f, 1f, 0x88000000)
        c.drawText("重命名", btnRenameRect.centerX(),
            by - (textPaint.ascent() + textPaint.descent()) / 2, textPaint)
        c.drawText("删除", btnDeleteRect.centerX(),
            by - (textPaint.ascent() + textPaint.descent()) / 2, textPaint)
        textPaint.setShadowLayer(0f, 0f, 0f, 0)
        textPaint.alpha = 255
    }

    /** generic black wheel: labels engraved, rotating; the fixed arrow outside
     *  (tip inward) marks the selection. Used for the left function wheel. */
    private fun drawWheel(c: Canvas, pv: PointF, rotation: Float,
                          labels: List<String>, outside: Float) {
        val r = dialR_
        drawDisc(c, pv, r)
        // always 8 divisions (45°); labels occupy their own slots at 45°
        // spacing (left wheel: 变声 slot0 + 克隆 slot1, adjacent)
        for (k in 0 until 8) {
            drawDent(c, pv, (outside + rotation + (k + 0.5) * 45.0).toFloat(), r)
        }
        for (k in labels.indices) {
            if (labels[k].isEmpty()) continue
            drawBeltText(c, pv, labels[k],
                (outside + rotation + k * 45.0).toFloat(), r, outside,
                big = false, maxPx = r * 0.84f)
        }
        drawArrow(c, pv, outside, r)
    }

    /** right wheel — the scrolling window belt (see class doc) */
    private fun drawBelt(c: Canvas, pv: PointF, list: List<String>,
                         win: Float, outside: Float) {
        val r = dialR_
        drawDisc(c, pv, r)
        for (k in 0 until 8) {
            drawDent(c, pv, 180f + (k + 0.5f - win) * 45f, r)
        }
        wheelTextPaint.alpha = 255
        val n = list.size
        for (v in -1..n) {
            val rel = v - win
            if (rel < 0.5 || rel > 7.5) continue          // hidden slot / off belt
            val ang = 180f + rel * 45f
            if (v in 0 until n) {
                drawBeltText(c, pv, list[v], ang, r, outside, big = false,
                    maxPx = r * 0.84f)
            } else if (plusOk) {                       // empty slot past an end
                if (v == -1) {                         // head: import a wav
                    drawBeltText(c, pv, "从文件导入", ang, r, outside,
                        big = false, maxPx = r * 0.84f, scale = 0.7f)
                } else {                               // tail: the "+" slot
                    val a = Math.toRadians(ang.toDouble())
                    val dx = pv.x + (r * 0.58 * cos(a)).toFloat()
                    val dy = pv.y + (r * 0.58 * sin(a)).toFloat()
                    c.save()
                    c.rotate(ang - outside, dx, dy)
                    val s = w * 0.0144f
                    plusPaint.strokeWidth = s * 0.22f
                    c.drawLine(dx - s / 2, dy, dx + s / 2, dy, plusPaint)
                    c.drawLine(dx, dy - s / 2, dx, dy + s / 2, plusPaint)
                    c.restore()
                }
            }
        }
        drawArrow(c, pv, outside, r)
    }

    private fun drawDisc(c: Canvas, pv: PointF, r: Float) {
        val bezel = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = 6f; color = 0x33000000
        }
        c.drawCircle(pv.x, pv.y, r + 4f, bezel)
        dialFace.shader = RadialGradient(pv.x - r * 0.35f, pv.y - r * 0.35f, r * 1.5f,
            0xFF54545E.toInt(), 0xFF12121A.toInt(), Shader.TileMode.CLAMP)
        c.drawCircle(pv.x, pv.y, r, dialFace)
    }

    private fun drawDent(c: Canvas, pv: PointF, ang: Float, r: Float) {
        c.save()
        c.rotate(ang, pv.x, pv.y)
        val y0 = pv.y + r * 0.34f
        val y1 = pv.y + r * 0.84f
        c.drawLine(pv.x, y0, pv.x, y1, dentDark)
        c.drawLine(pv.x + 2f, y0 + 1.5f, pv.x + 2f, y1 + 1.5f, dentLight)
        c.restore()
    }

    /** engraved belt text: rotates with the disc, tilt relative to the arrow
     *  (label under the arrow reads upright); long names shrink then "…".
     *  scale: pre-shrink size factor (action-slot labels render smaller so
     *  5-character labels fit without truncation) */
    private fun drawBeltText(c: Canvas, pv: PointF, raw: String, ang: Float,
                             r: Float, outside: Float, big: Boolean,
                             maxPx: Float = Float.MAX_VALUE,
                             scale: Float = 1f) {
        val a = Math.toRadians(ang.toDouble())
        val dx = pv.x + (r * 0.58 * cos(a)).toFloat()
        val dy = pv.y + (r * 0.58 * sin(a)).toFloat()
        c.save()
        c.rotate(ang - outside, dx, dy)
        var ts = (if (big) w * 0.021f else w * 0.016f) * scale
        wheelTextPaint.textSize = ts
        var label = raw
        val full = wheelTextPaint.measureText(label)
        if (full > maxPx) {
            ts = (ts * maxPx / full).coerceAtLeast(w * 0.0104f)   // 65% floor
            wheelTextPaint.textSize = ts
            if (wheelTextPaint.measureText(label) > maxPx) {
                while (label.isNotEmpty() &&
                    wheelTextPaint.measureText(label + "…") > maxPx)
                    label = label.dropLast(1)
                label += "…"
            }
        }
        wheelTextPaint.setShadowLayer(6f, 0f, 2f, 0x99000000)
        c.drawText(label, dx, dy + ts * 0.35f, wheelTextPaint)
        wheelTextPaint.setShadowLayer(0f, 0f, 0f, 0)
        c.restore()
    }

    private fun drawArrow(c: Canvas, pv: PointF, outside: Float, r: Float) {
        val rr = Math.toRadians(outside.toDouble())
        val ux = cos(rr).toFloat(); val uy = sin(rr).toFloat()   // outward unit
        val px = -uy; val py = ux                                // perpendicular
        val dist = r * 1.30f
        val ax = pv.x + ux * dist; val ay = pv.y + uy * dist
        val tip = r * 0.22f; val back = r * 0.10f; val half = r * 0.11f
        val arrow = Path().apply {
            moveTo(ax - ux * tip, ay - uy * tip)                 // tip AT the wheel
            lineTo(ax + ux * back + px * half, ay + uy * back + py * half)
            lineTo(ax + ux * back - px * half, ay + uy * back - py * half)
            close()
        }
        arrowPaint.color = 0xFF2B2B33.toInt()
        c.drawPath(arrow, arrowPaint)
        arrowPaint.color = 0xFFFFFFFF.toInt()
        arrowPaint.style = Paint.Style.STROKE
        arrowPaint.strokeWidth = 3f
        c.drawPath(arrow, arrowPaint)
        arrowPaint.style = Paint.Style.FILL
    }

    // ---------------------------------------------------------------- touch
    private fun inCircle(x: Float, y: Float, pv: PointF, r: Float) =
        (x - pv.x) * (x - pv.x) + (y - pv.y) * (y - pv.y) < r * r

    private fun angAt(pv: PointF, x: Float, y: Float) =
        Math.toDegrees(atan2((y - pv.y).toDouble(), (x - pv.x).toDouble())).toFloat()

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downTime = System.currentTimeMillis()
                downX = e.x; downY = e.y
                // converting / cloning: rename+delete are locked too
                pendingBtn = if (running || cloneBusy) 0 else when {
                    btnRenameRect.contains(e.x, e.y) -> 1
                    btnDeleteRect.contains(e.x, e.y) -> 2
                    else -> 0
                }
                if (pendingBtn != 0) return true
                knotDown = run {
                    // ONLY the black dome button toggles — not the whole
                    // module square (LED / buttons area must stay inert)
                    val dx = e.x - cx
                    val dy = e.y - btnCy
                    btnR > 0f && dx * dx + dy * dy < (btnR + 8f) * (btnR + 8f)
                }
                if (knotDown) return true
                // converting / cloning: the wheels are locked (the knot and
                // the rename/delete buttons stay alive)
                dragDial = if (running || cloneBusy) 0 else when {
                    inCircle(e.x, e.y, dialL, dialR_ * 1.25f) -> 1
                    inCircle(e.x, e.y, dialR, dialR_ * 1.25f) -> 2
                    else -> 0
                }
                if (dragDial != 0) {
                    val pv = if (dragDial == 1) dialL else dialR
                    dragLastAng = angAt(pv, e.x, e.y)
                }
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (dragDial == 0) return true
                if (running || cloneBusy) { dragDial = 0; return true }   // locked mid-drag
                val pv = if (dragDial == 1) dialL else dialR
                val a = angAt(pv, e.x, e.y)
                var d = a - dragLastAng
                if (d > 180) d -= 360; if (d < -180) d += 360
                dragLastAng = a
                if (dragDial == 1) rotL = (rotL + d).coerceIn(-45f, 0f)
                else {
                    val n = (rightListProvider?.invoke() ?: emptyList()).size
                    // 变声: arrow stays on real entries [0, n-1];
                    // 克隆: one empty slot past each end [-1, n]
                    val minW = if (plusOk) -5f else -4f
                    val maxW = maxOf(if (plusOk) n - 4f else n - 5f, minW)
                    winW = (winW - d / 45f).coerceIn(minW, maxW)
                }
                invalidate()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                // generous tap window: laggy input pipelines (emulators, slow
                // touch panels) routinely deliver DOWN/UP slower than 250 ms
                val quick = System.currentTimeMillis() - downTime < 500 &&
                        abs(e.x - downX) < 48 && abs(e.y - downY) < 48
                if (dragDial == 1 && !quick) {
                    rotL = Math.round(rotL / 45f).coerceIn(-1, 0) * 45f
                    val idx = -Math.round(rotL / 45f).toInt()   // 0 变声 / 1 克隆
                    haptic()
                    onLeftMode?.invoke(idx)
                }
                if (dragDial == 2 && !quick) {
                    val n = (rightListProvider?.invoke() ?: emptyList()).size
                    val s = Math.round(winW).toInt() + 4
                    winW = (s - 4).toFloat()
                    curSlot = s
                    android.util.Log.i("meanvc", "wheel snap s=$s winW=$winW")
                    haptic()
                    if (s in 0 until n && s != lastSel) {
                        onRightSelect?.invoke(s)
                        lastSel = s
                    }
                }
                if (pendingBtn != 0 && quick) {
                    if (pendingBtn == 1) onRenameTap?.invoke()
                    else onDeleteTap?.invoke()
                }
                pendingBtn = 0
                if (knotDown && quick) onKnotTap?.invoke()
                else if (knotDown) android.util.Log.i("meanvc",
                    "knot press rejected as drag (${System.currentTimeMillis() - downTime} ms)")
                knotDown = false
                dragDial = 0
                invalidate()
                return true
            }
        }
        return true
    }

    private fun haptic() = performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
}
