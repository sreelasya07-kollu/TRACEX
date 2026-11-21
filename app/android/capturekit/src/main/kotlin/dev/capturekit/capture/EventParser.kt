package dev.capturekit.capture

/** Events the core emits through ck_poll_events (flat JSON lines). Parsed without org.json so it is JVM-testable. */
sealed class CoreEvent {
    data class KeyframeRequest(val frameId: Int, val file: String) : CoreEvent()
    data class KeyframeExpired(val frameId: Int, val file: String) : CoreEvent()
    data class StateChange(val from: String, val to: String, val reason: String) : CoreEvent()
    data class FrameQuality(val frameId: Int, val quality: Double, val hardReject: Boolean, val reasons: List<String>) : CoreEvent()
    data class Governor(val from: String, val to: String, val reason: String) : CoreEvent()
    /** A guidance instruction (Phase 8). Voice/overlay shells render it; params are in the raw line. */
    data class Instruction(
        val id: Int, val type: String, val speakText: String, val longText: String,
        val priority: Int, val haptic: String, val overlayHint: String,
    ) : CoreEvent()
    /** Glanceable "what the app understands" snapshot (~2 Hz, non-critical). Wording (whyText) comes from the core. */
    data class Overlay(
        val completeness: Double, val seen: Double, val tracking: Int, val mode: String,
        val wallFound: Boolean, val floorFound: Boolean, val ceilingFound: Boolean, val walls: Int,
        val userX: Double, val userZ: Double, val userYawDeg: Double,
        val hasTarget: Boolean, val targetX: Double, val targetZ: Double,
        val whyId: String, val whyText: String, val surfaces: List<WallSeg>,
        /** Overlay v2 (ADR 0022): bit i set = a keyframe looked along heading [30i, 30i+30) deg. 0 when absent (old core). */
        val yawMask: Int = 0,
        /** Overlay v3 (ADR 0026): `surfaces` are layout walls, `walls` the layout wall count; false for an older core. */
        val layoutV3: Boolean = false,
        /** v3: wall spans (observed / blocked by furniture / missing). Empty when absent. */
        val spans: List<SpanSeg> = emptyList(),
        /** v3: the live room outline corners in order (<= 12), empty while there is none. */
        val shell: List<PlanPt> = emptyList(),
        val shellClosed: Boolean = false,
        /** v3: a ceiling plane was found OR a keyframe looked up. The ceiling is optional for completeness. */
        val ceilingSeen: Boolean = false,
        /** Phase 9 (only with guide.understanding on; empty otherwise): detected furniture footprints. */
        val objects: List<ObjectBox> = emptyList(),
        val openings: List<Opening> = emptyList(),
        val gaps: List<GapMark> = emptyList(),
        val gapWhyText: String = "",
    ) : CoreEvent()
    data class IngestDrop(val counts: Map<String, Long>) : CoreEvent()
    /**
     * Issue #1 / ADR 0031: tracking has been degraded (LIMITED = ARCore PAUSED, or NOT_AVAILABLE) for 2 s. The core has
     * paused keyframe capture and speaks RECALIBRATE until [TrackingReset]. [trackingState] / [reason] are core codes.
     */
    data class TrackingLost(val tNs: Long, val trackingState: Int, val reason: Int, val degradedS: Double) : CoreEvent()
    /** Tracking is back and steady: the session was re-anchored. [driftM] / [driftDeg] = pose jump across the loss. */
    data class TrackingReset(val tNs: Long, val lostS: Double, val driftM: Double, val driftDeg: Double) : CoreEvent()
    data class Other(val type: String, val raw: String) : CoreEvent()
}

/** One wall segment in world metres (x,z plan view), the percentage of its cells seen, and the stable 1-based wall id (0 = unknown, v1 core). */
data class WallSeg(val x0: Double, val z0: Double, val x1: Double, val z1: Double, val pct: Int, val id: Int = 0)

/** A point in the plan view (world metres). */
data class PlanPt(val x: Double, val z: Double)

/** One stretch of a layout wall: [state] 'o' observed, 'b' blocked (hidden by furniture, never demanded), 'm' missing. [id] = the wall's surfaces id. */
data class SpanSeg(val id: Int, val x0: Double, val z0: Double, val x1: Double, val z1: Double, val state: Char)

/** A detected object footprint: centre (x, z), w x d metres, [yawDeg] 0..89, height [h]. */
data class ObjectBox(val id: Int, val cls: String, val x: Double, val z: Double, val w: Double, val d: Double, val yawDeg: Int, val h: Double)

/** A door or window on a wall: footprint segment (x0,z0)-(x1,z1), heights y0..y1 above the floor. [kind] = "door" | "window". */
data class Opening(val kind: String, val wall: String, val x0: Double, val z0: Double, val x1: Double, val z1: Double, val y0: Double, val y1: Double)

/** An expected-but-unseen area (centre x, z; priority 0..1). */
data class GapMark(val type: String, val x: Double, val z: Double, val pri: Double)

object EventParser {
    /** Splits "a,b,...;..." rows; rows with another field count are skipped; [build] returns null to skip a malformed row. */
    private inline fun <T> rows(s: String, fields: Int, build: (List<String>) -> T?): List<T> =
        if (s.isEmpty()) emptyList() else s.split(';').mapNotNull { part ->
            val f = part.split(',').map { it.trim() }
            if (f.size != fields) null else build(f)
        }

    /** v3 "id,x0,z0,x1,z1,state;..." (state o|b|m). */
    fun parseSpans(s: String): List<SpanSeg> = rows(s, 6) { f ->
        val v = f.subList(0, 5).map { it.toDoubleOrNull() ?: return@rows null }
        val st = f[5].firstOrNull()?.takeIf { it == 'o' || it == 'b' || it == 'm' } ?: return@rows null
        SpanSeg(v[0].toInt(), v[1], v[2], v[3], v[4], st)
    }

    /** v3 "x,z;..." outline corners. */
    fun parseShell(s: String): List<PlanPt> = rows(s, 2) { f ->
        PlanPt(f[0].toDoubleOrNull() ?: return@rows null, f[1].toDoubleOrNull() ?: return@rows null)
    }

    private fun f3(v: Double) = String.format(java.util.Locale.ROOT, "%.3f", v)

    /** Inverse of the parsers above, for the Review hand-over (Intent extras). */
    fun encodeShell(l: List<PlanPt>): String = l.joinToString(";") { "${f3(it.x)},${f3(it.z)}" }
    fun encodeSpans(l: List<SpanSeg>): String = l.joinToString(";") { "${it.id},${f3(it.x0)},${f3(it.z0)},${f3(it.x1)},${f3(it.z1)},${it.state}" }
    fun encodeObjects(l: List<ObjectBox>): String =
        l.joinToString(";") { "${it.id},${it.cls},${f3(it.x)},${f3(it.z)},${f3(it.w)},${f3(it.d)},${it.yawDeg},${f3(it.h)}" }
    fun encodeOpenings(l: List<Opening>): String =
        l.joinToString(";") { "${it.kind},${it.wall},${f3(it.x0)},${f3(it.z0)},${f3(it.x1)},${f3(it.z1)},${f3(it.y0)},${f3(it.y1)}" }

    /** "id,cls,x,z,w,d,yaw_deg,h;...". */
    fun parseObjects(s: String): List<ObjectBox> = rows(s, 8) { f ->
        val n = (listOf(0, 2, 3, 4, 5, 6, 7)).map { f[it].toDoubleOrNull() ?: return@rows null }
        ObjectBox(n[0].toInt(), f[1], n[1], n[2], n[3], n[4], n[5].toInt(), n[6])
    }

    /** "kind,wall,x0,z0,x1,z1,y0,y1;...". */
    fun parseOpenings(s: String): List<Opening> = rows(s, 8) { f ->
        val n = (2..7).map { f[it].toDoubleOrNull() ?: return@rows null }
        Opening(f[0], f[1], n[0], n[1], n[2], n[3], n[4], n[5])
    }

    /** "type,x,z,pri;...". */
    fun parseGaps(s: String): List<GapMark> = rows(s, 4) { f ->
        GapMark(f[0], f[1].toDoubleOrNull() ?: return@rows null, f[2].toDoubleOrNull() ?: return@rows null, f[3].toDoubleOrNull() ?: return@rows null)
    }

    /** "x0,z0,x1,z1,pct[,id];..." (5 fields v1, 6 fields v2) -> segments; malformed entries are skipped. */
    fun parseSurfaces(s: String): List<WallSeg> = s.split(';').mapNotNull { part ->
        val f = part.split(',')
        if (f.size != 5 && f.size != 6) return@mapNotNull null
        val v = f.map { it.trim().toDoubleOrNull() ?: return@mapNotNull null }
        WallSeg(v[0], v[1], v[2], v[3], v[4].toInt(), if (f.size == 6) v[5].toInt().coerceAtLeast(0) else 0)
    }

    private fun str(line: String, key: String): String? =
        Regex("\"$key\":\"((?:[^\"\\\\]|\\\\.)*)\"").find(line)?.groupValues?.get(1)

    private fun num(line: String, key: String): Double? =
        Regex("\"$key\":(-?[0-9.eE+-]+)").find(line)?.groupValues?.get(1)?.toDoubleOrNull()

    /** Integer field without a round trip through Double (t_ns needs all 64 bits). */
    private fun long(line: String, key: String): Long? =
        Regex("\"$key\":(-?[0-9]+)(?![0-9.eE])").find(line)?.groupValues?.get(1)?.toLongOrNull()

    fun parseLine(line: String): CoreEvent? {
        val type = str(line, "type") ?: return null
        return when (type) {
            "keyframe_request" -> CoreEvent.KeyframeRequest((num(line, "frame_id") ?: return null).toInt(), str(line, "file") ?: return null)
            "keyframe_expired" -> CoreEvent.KeyframeExpired((num(line, "frame_id") ?: return null).toInt(), str(line, "file") ?: "")
            "state" -> CoreEvent.StateChange(str(line, "from") ?: "", str(line, "to") ?: "", str(line, "reason") ?: "")
            "governor" -> CoreEvent.Governor(str(line, "from") ?: "", str(line, "to") ?: "", str(line, "reason") ?: "")
            "frame_quality" -> CoreEvent.FrameQuality(
                (num(line, "frame_id") ?: return null).toInt(),
                num(line, "quality") ?: 0.0,
                line.contains("\"hard_reject\":true"),
                Regex("\"reasons\":\\[([^\\]]*)]").find(line)?.groupValues?.get(1)
                    ?.split(',')?.map { it.trim().trim('"') }?.filter { it.isNotEmpty() } ?: emptyList(),
            )
            "instruction" -> CoreEvent.Instruction(
                (num(line, "id") ?: return null).toInt(), str(line, "instruction") ?: "", str(line, "speak_text") ?: "",
                str(line, "long_text") ?: "", (num(line, "priority") ?: 2.0).toInt(), str(line, "haptic") ?: "none",
                str(line, "overlay_hint") ?: "",
            )
            "overlay" -> CoreEvent.Overlay(
                num(line, "completeness") ?: 0.0, num(line, "seen") ?: 0.0, (num(line, "tracking") ?: 0.0).toInt(),
                str(line, "mode") ?: "",
                line.contains("\"wall_found\":true"), line.contains("\"floor_found\":true"), line.contains("\"ceiling_found\":true"),
                (num(line, "walls") ?: 0.0).toInt(),
                num(line, "user_x") ?: 0.0, num(line, "user_z") ?: 0.0, num(line, "user_yaw_deg") ?: 0.0,
                line.contains("\"has_target\":true"), num(line, "target_x") ?: 0.0, num(line, "target_z") ?: 0.0,
                str(line, "why_id") ?: "", str(line, "why_text") ?: "", parseSurfaces(str(line, "surfaces") ?: ""),
                ((num(line, "yaw_mask") ?: 0.0).toInt() and 0xFFF),
                layoutV3 = line.contains("\"shell_closed\":"),
                spans = parseSpans(str(line, "spans") ?: ""),
                shell = parseShell(str(line, "shell") ?: ""),
                shellClosed = line.contains("\"shell_closed\":true"),
                ceilingSeen = line.contains("\"ceiling_seen\":true"),
                objects = parseObjects(str(line, "objects") ?: ""),
                openings = parseOpenings(str(line, "openings") ?: ""),
                gaps = parseGaps(str(line, "gaps") ?: ""),
                gapWhyText = str(line, "gap_why_text") ?: "",
            )
            "tracking_lost" -> CoreEvent.TrackingLost(
                long(line, "t_ns") ?: return null, (num(line, "tracking_state") ?: 0.0).toInt(),
                (num(line, "reason") ?: 0.0).toInt(), num(line, "degraded_s") ?: 0.0,
            )
            "tracking_reset" -> CoreEvent.TrackingReset(
                long(line, "t_ns") ?: return null, num(line, "lost_s") ?: 0.0,
                num(line, "drift_m") ?: 0.0, num(line, "drift_deg") ?: 0.0,
            )
            "ingest_drop" -> CoreEvent.IngestDrop(
                listOf("pose", "gyro", "accel", "intrinsics", "frame", "points", "plane", "platform")
                    .mapNotNull { k -> num(line, k)?.let { k to it.toLong() } }.toMap(),
            )
            else -> CoreEvent.Other(type, line)
        }
    }

    fun parse(text: String): List<CoreEvent> = text.lineSequence().filter { it.isNotBlank() }.mapNotNull(::parseLine).toList()
}

/** Decoded ck_policy (CaptureKitNative.policy). */
data class Policy(
    val level: Int, val keyframeRateScale: Double, val mlRateScale: Double,
    val resolutionTier: Int, val overlay: Boolean, val upload: Boolean, val mustPause: Boolean,
) {
    companion object {
        val LEVELS = listOf("NOMINAL", "REDUCE_KEYFRAMES", "REDUCE_ML", "LOWER_RESOLUTION", "WARN", "SAFE_PAUSE")
        fun from(a: DoubleArray) = Policy(a[0].toInt(), a[1], a[2], a[3].toInt(), a[4] != 0.0, a[5] != 0.0, a[6] != 0.0)
    }
    val levelName: String get() = LEVELS.getOrElse(level) { "?" }
}
