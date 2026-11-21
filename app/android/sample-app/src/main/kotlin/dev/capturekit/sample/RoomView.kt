package dev.capturekit.sample

import android.content.Context
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.view.View
import dev.capturekit.capture.CoreEvent
import dev.capturekit.capture.GapMark
import dev.capturekit.capture.LabelMap
import dev.capturekit.capture.ObjectBox
import dev.capturekit.capture.Opening
import dev.capturekit.capture.PlanPt
import dev.capturekit.capture.SpanSeg
import dev.capturekit.capture.WallSeg
import dev.capturekit.guidance.MiniMapMath
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * Live mini-map (real-room-fix T3): a top-down plan, HEADING-UP (the user is fixed at the centre, their forward
 * direction is screen up; [MiniMapMath] owns the sign conventions, yaw positive = left). Draws the closed room outline
 * with corner dots, the layout walls (inked by the seen percentage, green from [DONE_PCT]), hatched spans for stretches
 * hidden by furniture (never demanded), dashed spans for missing stretches, doors / windows, detected objects (footprint
 * in a per-class colour with its name, e.g. "sofa", issue #2 B) and unseen-area marks when the core sends them, the
 * 12-sector heading ring, the target dot and a ceiling icon.
 * Elements the core did not send are simply not drawn (older core, or `guide.understanding` off).
 * [setData] copies the snapshot into preallocated arrays (call at <= 4 Hz, UI thread); onDraw never allocates.
 */
class RoomView(ctx: Context) : View(ctx) {
    private val d = ctx.resources.displayMetrics.density

    // ---- snapshot (preallocated) ----
    private val wx0 = FloatArray(MAX_WALLS); private val wz0 = FloatArray(MAX_WALLS)
    private val wx1 = FloatArray(MAX_WALLS); private val wz1 = FloatArray(MAX_WALLS)
    private val wpct = IntArray(MAX_WALLS); private val wid = IntArray(MAX_WALLS)
    private var nWalls = 0
    private val sa = FloatArray(MAX_SPANS * 4); private val sId = IntArray(MAX_SPANS); private val sSt = CharArray(MAX_SPANS)
    private var nSpans = 0
    private val shx = FloatArray(MAX_SHELL); private val shz = FloatArray(MAX_SHELL)
    private var nShell = 0
    private var shellClosed = false
    private val ob = FloatArray(MAX_OBJ * 5)   // x, z, w, d, yaw(rad)
    private val obName = arrayOfNulls<String>(MAX_OBJ)   // display word ("sofa"), built in copyObjects, never in onDraw
    private val obColor = IntArray(MAX_OBJ)
    private var nObj = 0
    private val op = FloatArray(MAX_OPEN * 4); private val opDoor = BooleanArray(MAX_OPEN)
    private var nOpen = 0
    private val gp = FloatArray(MAX_GAPS * 2)
    private var nGaps = 0
    private var userX = 0f; private var userZ = 0f; private var yawDeg = 0.0
    private var hasTarget = false; private var targetX = 0f; private var targetZ = 0f
    private var floorFound = false; private var ceilingFound = false; private var tracking = true
    private var hasData = false
    private var showUser = true
    private var yawMask = 0                  // overlay v2: 12 x 30 deg heading sectors with a keyframe (bit i = [30i, 30i+30) deg)
    private var refYawDeg = 0.0              // frame of the plan: the user's heading (live) or 180 = north-up, -z up (review snapshot)
    private var refX = 0f; private var refZ = 0f

    // ---- view transform: screen = (cx, cy) + s * MiniMapMath(world), smoothed ----
    private var s = 0f; private var radius = MIN_R_M
    private var snap = true
    private val tmp = FloatArray(2)

    // ---- drawing resources ----
    private val floorFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f * d; color = 0x99FFFFFF.toInt(); strokeJoin = Paint.Join.ROUND }
    private val wallInk = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 4.5f * d; strokeCap = Paint.Cap.ROUND }
    private val blockedLine = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.8f * d; color = 0xFFFFB300.toInt() }
    private val missingLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2.5f * d; color = 0x80FFFFFF.toInt()
        pathEffect = DashPathEffect(floatArrayOf(4f * d, 4f * d), 0f)
    }
    private val cornerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt() }
    private val cornerHaloPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1f * d; color = 0x6600E676.toInt() }
    private val doorPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 5f * d; color = 0xFFFFB74D.toInt(); strokeCap = Paint.Cap.ROUND }
    private val windowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 5f * d; color = 0xFF80DEEA.toInt(); strokeCap = Paint.Cap.ROUND }
    private val objFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x66B0BEC5 }
    private val objEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.2f * d; color = 0xCCCFD8DC.toInt() }
    private val objLabel = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 8.5f * d; color = 0xFFFFFFFF.toInt(); textAlign = Paint.Align.CENTER }
    private val objLabelHalo = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 8.5f * d; color = 0xCC000000.toInt(); textAlign = Paint.Align.CENTER
        style = Paint.Style.STROKE; strokeWidth = 2.5f * d; strokeJoin = Paint.Join.ROUND
    }
    private val gapRing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 1.5f * d; color = 0xCCFFB74D.toInt()
        pathEffect = DashPathEffect(floatArrayOf(3f * d, 3f * d), 0f)
    }
    private val userPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val userEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f * d; color = 0xFF000000.toInt() }
    private val fovBeamPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0x2600E5FF }
    private val fovBeamStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1f * d; color = 0x4D00E5FF }
    private val beamPath = Path()
    private val targetPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFD54F.toInt() }
    private val targetRing = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f * d; color = 0xFFFFD54F.toInt() }
    private val targetPulseRing = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.2f * d; color = 0x80FFD54F.toInt() }
    private val ceilFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = INK_DONE }
    private val ceilEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f * d; color = 0xCCFFFFFF.toInt() }
    private val path = Path()
    private val ringGhost = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * d; color = 0x33FFFFFF; strokeCap = Paint.Cap.ROUND }
    private val ringDone = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * d; color = INK_DONE; strokeCap = Paint.Cap.ROUND }

    init { importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO }

    /** Forget the session (fit and all elements). */
    fun reset() {
        yawMask = 0; hasData = false; nWalls = 0; nSpans = 0; nShell = 0; nObj = 0; nOpen = 0; nGaps = 0
        shellClosed = false; radius = MIN_R_M; snap = true; invalidate()
    }

    /**
     * Static snapshot for the Review screen: outline, corners, walls, spans, objects and openings, north-up (-z up), no user
     * arrow or target. Call on the UI thread; the view must be (or become) laid out, [onSizeChanged] refits.
     */
    fun setSnapshot(
        segs: List<WallSeg>, floor: Boolean, ceiling: Boolean,
        shell: List<PlanPt> = emptyList(), closed: Boolean = false, spans: List<SpanSeg> = emptyList(),
        objects: List<ObjectBox> = emptyList(), openings: List<Opening> = emptyList(),
    ) {
        copyWalls(segs); copyShell(shell, closed); copySpans(spans); copyObjects(objects); copyOpenings(openings); nGaps = 0
        // Reference = centroid of everything drawn, so the fit is symmetric around it.
        var sx = 0f; var sz = 0f; var n = 0
        for (i in 0 until nWalls) { sx += wx0[i] + wx1[i]; sz += wz0[i] + wz1[i]; n += 2 }
        for (i in 0 until nShell) { sx += shx[i]; sz += shz[i]; n++ }
        refX = if (n > 0) sx / n else 0f; refZ = if (n > 0) sz / n else 0f; refYawDeg = 180.0
        userX = refX; userZ = refZ
        showUser = false; hasTarget = false; tracking = true; yawMask = 0
        floorFound = floor; ceilingFound = ceiling
        hasData = true; snap = true
        fit(); invalidate()
    }

    fun setData(o: CoreEvent.Overlay) {
        showUser = true; yawMask = o.yawMask
        copyWalls(o.surfaces); copyShell(o.shell, o.shellClosed); copySpans(o.spans)
        copyObjects(o.objects); copyOpenings(o.openings); copyGaps(o.gaps)
        userX = o.userX.toFloat(); userZ = o.userZ.toFloat(); yawDeg = o.userYawDeg
        refX = userX; refZ = userZ; refYawDeg = yawDeg
        hasTarget = o.hasTarget; targetX = o.targetX.toFloat(); targetZ = o.targetZ.toFloat()
        floorFound = o.floorFound; ceilingFound = o.ceilingFound || o.ceilingSeen; tracking = o.tracking == 1
        hasData = true
        fit()
        invalidate()
    }

    private fun copyWalls(segs: List<WallSeg>) {
        nWalls = min(segs.size, MAX_WALLS)
        for (i in 0 until nWalls) {
            val w = segs[i]
            wx0[i] = w.x0.toFloat(); wz0[i] = w.z0.toFloat(); wx1[i] = w.x1.toFloat(); wz1[i] = w.z1.toFloat()
            wpct[i] = w.pct.coerceIn(0, 100); wid[i] = w.id
        }
    }

    private fun copyShell(shell: List<PlanPt>, closed: Boolean) {
        nShell = min(shell.size, MAX_SHELL); shellClosed = closed && nShell >= 3
        for (i in 0 until nShell) { shx[i] = shell[i].x.toFloat(); shz[i] = shell[i].z.toFloat() }
    }

    private fun copySpans(spans: List<SpanSeg>) {
        nSpans = min(spans.size, MAX_SPANS)
        for (i in 0 until nSpans) {
            val p = spans[i]
            sa[4 * i] = p.x0.toFloat(); sa[4 * i + 1] = p.z0.toFloat(); sa[4 * i + 2] = p.x1.toFloat(); sa[4 * i + 3] = p.z1.toFloat()
            sId[i] = p.id; sSt[i] = p.state
        }
    }

    private fun copyObjects(objs: List<ObjectBox>) {
        nObj = min(objs.size, MAX_OBJ)
        for (i in 0 until nObj) {
            val o = objs[i]
            ob[5 * i] = o.x.toFloat(); ob[5 * i + 1] = o.z.toFloat(); ob[5 * i + 2] = o.w.toFloat(); ob[5 * i + 3] = o.d.toFloat()
            ob[5 * i + 4] = Math.toRadians(o.yawDeg.toDouble()).toFloat()
            obName[i] = LabelMap.displayName(o.cls); obColor[i] = objectColor(o.cls)
        }
    }

    private fun copyOpenings(ops: List<Opening>) {
        nOpen = min(ops.size, MAX_OPEN)
        for (i in 0 until nOpen) {
            val o = ops[i]
            op[4 * i] = o.x0.toFloat(); op[4 * i + 1] = o.z0.toFloat(); op[4 * i + 2] = o.x1.toFloat(); op[4 * i + 3] = o.z1.toFloat()
            opDoor[i] = o.kind == "door"
        }
    }

    private fun copyGaps(gaps: List<GapMark>) {
        nGaps = min(gaps.size, MAX_GAPS)
        for (i in 0 until nGaps) { gp[2 * i] = gaps[i].x.toFloat(); gp[2 * i + 1] = gaps[i].z.toFloat() }
    }

    // ---- fit: the user is the centre, the scale shows everything within [radius] metres (stable while turning) ----

    private var far = 0f
    private fun reach(x: Float, z: Float) { far = max(far, hypot(x - refX, z - refZ)) }

    private fun fit() {
        val w = width.toFloat(); val h = height.toFloat()
        if (w <= 0f || h <= 0f || !hasData) return
        far = 0f
        for (i in 0 until nWalls) { reach(wx0[i], wz0[i]); reach(wx1[i], wz1[i]) }
        for (i in 0 until nShell) reach(shx[i], shz[i])
        for (i in 0 until nObj) reach(ob[5 * i], ob[5 * i + 1])
        if (hasTarget) reach(targetX, targetZ)
        val tr = max(far, MIN_R_M)
        val ts = min((min(w, h) / 2f - 12f * d) / tr, MAX_PX_PER_M * d)
        if (snap) { s = ts; radius = tr; snap = false } else { s += SMOOTH * (ts - s); radius += SMOOTH * (tr - radius) }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) { snap = true; fit() }

    // world -> screen (into tmp)
    private fun pt(x: Float, z: Float, into: FloatArray = tmp) {
        MiniMapMath.toScreen(x.toDouble(), z.toDouble(), refX.toDouble(), refZ.toDouble(), refYawDeg, 0, true, into)
        into[0] = width / 2f + s * into[0]; into[1] = height / 2f + s * into[1]
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat(); val h = height.toFloat()
        if (!hasData || s <= 0f) return
        c.save()
        c.clipRect(0f, 0f, w, h)

        // Floor + outline (the closed shell polygon; open while a side is still hypothesised)
        if (nShell >= 3) {
            path.reset()
            for (i in 0 until nShell) { pt(shx[i], shz[i]); if (i == 0) path.moveTo(tmp[0], tmp[1]) else path.lineTo(tmp[0], tmp[1]) }
            if (shellClosed) path.close()
            if (shellClosed) { floorFill.color = if (floorFound) 0x5566BB6A else 0x22FFFFFF; c.drawPath(path, floorFill) }
            outline.color = if (shellClosed) 0xDDFFFFFF.toInt() else 0x66FFFFFF
            c.drawPath(path, outline)
        }

        // Wall spans when the core sends them (v3); else the layout walls themselves, inked by pct.
        if (nSpans > 0) {
            for (i in 0 until nSpans) {
                pt(sa[4 * i], sa[4 * i + 1]); val x0 = tmp[0]; val y0 = tmp[1]
                pt(sa[4 * i + 2], sa[4 * i + 3]); val x1 = tmp[0]; val y1 = tmp[1]
                when (sSt[i]) {
                    'b' -> blockedSpan(c, x0, y0, x1, y1)
                    'm' -> c.drawLine(x0, y0, x1, y1, missingLine)
                    else -> { wallInk.color = inkFor(pctOfWall(sId[i])); c.drawLine(x0, y0, x1, y1, wallInk) }
                }
            }
        } else {
            for (i in 0 until nWalls) {
                pt(wx0[i], wz0[i]); val x0 = tmp[0]; val y0 = tmp[1]
                pt(wx1[i], wz1[i])
                wallInk.color = inkFor(wpct[i])
                c.drawLine(x0, y0, tmp[0], tmp[1], wallInk)
            }
        }

        // Corners
        if (nShell > 0) {
            val r = (if (shellClosed) 3.5f else 2.5f) * d
            for (i in 0 until nShell) {
                pt(shx[i], shz[i])
                c.drawCircle(tmp[0], tmp[1], r + 1.5f * d, cornerHaloPaint)
                c.drawCircle(tmp[0], tmp[1], r, cornerPaint)
            }
        }

        // Openings
        for (i in 0 until nOpen) {
            pt(op[4 * i], op[4 * i + 1]); val x0 = tmp[0]; val y0 = tmp[1]
            pt(op[4 * i + 2], op[4 * i + 3])
            c.drawLine(x0, y0, tmp[0], tmp[1], if (opDoor[i]) doorPaint else windowPaint)
        }

        // Objects: oriented footprints (local x axis (cos y, -sin y), local z axis (sin y, cos y) in world x/z)
        for (i in 0 until nObj) {
            val cx = ob[5 * i]; val cz = ob[5 * i + 1]; val hw = ob[5 * i + 2] / 2f; val hd = ob[5 * i + 3] / 2f
            val ca = cos(ob[5 * i + 4]); val sn = sin(ob[5 * i + 4])
            path.reset()
            for (k in 0 until 4) {
                val lx = if (k == 0 || k == 3) -hw else hw; val lz = if (k < 2) -hd else hd
                pt(cx + lx * ca + lz * sn, cz - lx * sn + lz * ca)
                if (k == 0) path.moveTo(tmp[0], tmp[1]) else path.lineTo(tmp[0], tmp[1])
            }
            path.close()
            objFill.color = (obColor[i] and 0x00FFFFFF) or 0x66000000
            objEdge.color = (obColor[i] and 0x00FFFFFF) or 0xDD000000.toInt()
            c.drawPath(path, objFill); c.drawPath(path, objEdge)
        }
        // Object names on top of every footprint (after all footprints, so a neighbour never covers a name).
        for (i in 0 until nObj) {
            val name = obName[i] ?: continue
            pt(ob[5 * i], ob[5 * i + 1])
            val y = tmp[1] - (objLabel.ascent() + objLabel.descent()) / 2f   // vertically centred on the footprint
            c.drawText(name, tmp[0], y, objLabelHalo); c.drawText(name, tmp[0], y, objLabel)
        }

        // Unseen-area marks
        for (i in 0 until nGaps) { pt(gp[2 * i], gp[2 * i + 1]); c.drawCircle(tmp[0], tmp[1], 6f * d, gapRing) }

        // Target dot
        if (hasTarget) {
            pt(targetX, targetZ)
            c.drawCircle(tmp[0], tmp[1], 11f * d, targetPulseRing)
            c.drawCircle(tmp[0], tmp[1], 7f * d, targetRing)
            c.drawCircle(tmp[0], tmp[1], 3.5f * d, targetPaint)
        }

        // User arrow: fixed at the centre, always pointing up. Heading ring: world sector [30i, 30i+30) deg drawn at its
        // bearing relative to the user (positive = left = screen left).
        if (showUser) {
            val ux = w / 2f; val uy = h / 2f

            // Flashlight / FOV beam: projects forward from the user (screen UP) like Polycam / RoomPlan
            if (tracking) {
                val beamLen = 32f * d
                val halfFovRad = Math.toRadians(25.0) // 50 deg total cone
                beamPath.reset()
                beamPath.moveTo(ux, uy)
                val bxL = ux - beamLen * sin(halfFovRad).toFloat()
                val byL = uy - beamLen * cos(halfFovRad).toFloat()
                val bxR = ux + beamLen * sin(halfFovRad).toFloat()
                val byR = uy - beamLen * cos(halfFovRad).toFloat()
                beamPath.lineTo(bxL, byL)
                beamPath.quadTo(ux, uy - beamLen * 1.08f, bxR, byR)
                beamPath.close()
                c.drawPath(beamPath, fovBeamPaint)
                c.drawPath(beamPath, fovBeamStroke)
            }

            val rr = 15f * d
            for (sec in 0 until 12) {
                path.reset()
                for (j in 0..3) {
                    val rel = Math.toRadians(sec * 30 + 1 + j * 9.33 - yawDeg)
                    val px = ux - rr * sin(rel).toFloat(); val py = uy - rr * cos(rel).toFloat()
                    if (j == 0) path.moveTo(px, py) else path.lineTo(px, py)
                }
                c.drawPath(path, if ((yawMask shr sec) and 1 != 0) ringDone else ringGhost)
            }
            val a = 9f * d; val b = 5.5f * d
            path.reset()
            path.moveTo(ux, uy - a)
            path.lineTo(ux - b, uy + b * 0.6f)
            path.lineTo(ux, uy + b * 0.1f)
            path.lineTo(ux + b, uy + b * 0.6f)
            path.close()
            userPaint.color = if (tracking) 0xFF00E5FF.toInt() else UiKit.AMBER
            c.drawPath(path, userPaint)
            c.drawPath(path, userEdge)
        }
        c.restore()

        // Ceiling icon (top-right): a flat slab, filled once the ceiling is seen (optional for completeness)
        val iw = 22f * d; val ih = 8f * d; val m = 8f * d
        val right = w - m; val top = m
        path.reset()
        path.moveTo(right - iw, top + ih); path.lineTo(right - iw * 0.75f, top)
        path.lineTo(right, top); path.lineTo(right - iw * 0.25f, top + ih); path.close()
        if (ceilingFound) c.drawPath(path, ceilFill)
        c.drawPath(path, ceilEdge)
    }

    /** Percentage of the wall with this id; -1 when no wall has it (the span is then drawn in the scanning colour). */
    private fun pctOfWall(id: Int): Int {
        for (i in 0 until nWalls) if (wid[i] == id) return wpct[i]
        return -1
    }

    private fun inkFor(pct: Int) = if (pct >= DONE_PCT) INK_DONE else INK_SCANNING

    /** Hatched span: an amber centre line with diagonal ticks (hidden behind furniture; never demanded). */
    private fun blockedSpan(c: Canvas, x0: Float, y0: Float, x1: Float, y1: Float) {
        c.drawLine(x0, y0, x1, y1, blockedLine)
        val dx = x1 - x0; val dy = y1 - y0
        val len = hypot(dx, dy)
        if (len < 1f) return
        val ux = dx / len; val uy = dy / len
        val nx = -uy; val ny = ux
        val step = 5f * d; val half = 3.5f * d
        var t = 0f
        while (t <= len) {
            val px = x0 + ux * t; val py = y0 + uy * t
            // tick leans 45 degrees along the span
            c.drawLine(px - (nx + ux) * half * 0.7f, py - (ny + uy) * half * 0.7f, px + (nx + ux) * half * 0.7f, py + (ny + uy) * half * 0.7f, blockedLine)
            t += step
        }
    }

    companion object {
        /**
         * Colour of an object class (core name) on the maps and the camera-feed boxes, by kind: seating blue, tables and
         * desks amber, bed purple, screens cyan, plants green, kitchen and bathroom teal, storage brown, lamp yellow;
         * anything else (newer core) grey.
         */
        fun objectColor(coreName: String): Int = when (coreName) {
            "chair", "couch", "bench" -> 0xFF64B5F6.toInt()
            "dining_table", "desk" -> 0xFFFFB74D.toInt()
            "bed" -> 0xFFBA68C8.toInt()
            "tv", "laptop" -> 0xFF4DD0E1.toInt()
            "potted_plant" -> 0xFF81C784.toInt()
            "toilet", "sink", "refrigerator", "oven" -> 0xFF4DB6AC.toInt()
            "shelf", "cabinet" -> 0xFFA1887F.toInt()
            "lamp" -> 0xFFFFF176.toInt()
            else -> 0xFFB0BEC5.toInt()
        }

        /** Review hand-over encoders (overlay string formats; parse with EventParser). */
        fun encodeSurfaces(segs: List<WallSeg>): String = segs.joinToString(";") {
            String.format(java.util.Locale.ROOT, "%.3f,%.3f,%.3f,%.3f,%d,%d", it.x0, it.z0, it.x1, it.z1, it.pct, it.id)
        }

        private const val MAX_WALLS = 12             // core overlay caps surfaces at 12
        private const val MAX_SPANS = 24             // core overlay caps spans at 24
        private const val MAX_SHELL = 12
        private const val MAX_OBJ = 12
        private const val MAX_OPEN = 6
        private const val MAX_GAPS = 4
        const val MIN_R_M = 2.0f              // never zoom in tighter than 2 m around the user
        const val MAX_PX_PER_M = 40f          // dp per metre cap
        const val SMOOTH = 0.35f              // fit easing per update (4 Hz)
        const val DONE_PCT = 80               // guess until Gate 0 data
        const val INK_DONE = 0xFF00E676.toInt()
        const val INK_SCANNING = 0xFF00B0FF.toInt()
    }
}
