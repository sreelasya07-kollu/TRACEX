package dev.capturekit.capture

/**
 * COCO class indices of the EfficientDet-Lite0 detector (0-based, person = 0, "???" gaps kept; ADR 0017) mapped to the
 * core's object classes (understanding.hpp ObjectClass ids). Only furniture/obstacle classes the core understands are
 * kept; everything else maps to [UNKNOWN] and is dropped by the worker. Offset 0 was checked by running the model on
 * COCO val2017 000000039769 (cats on a couch: classes 16 cat, 62 couch, 74 remote).
 *
 * ADR 0030: core ids are append-only (they are recorded in bundles). 8..12 are further COCO indoor classes the shipped
 * model emits; 13..16 (desk, shelf, cabinet, lamp) exist in the core for a future detector trained on them and have no
 * COCO index, so nothing maps to them yet. COCO 2017 has no door / window / mirror / desk labels at all (their "???"
 * slots 65, 67, 68, 70 are never predicted): doors and windows come from the core's geometric opening detector.
 */
object LabelMap {
    const val UNKNOWN = 0
    const val CHAIR = 1
    const val COUCH = 2
    const val BED = 3
    const val DINING_TABLE = 4
    const val TV = 5
    const val POTTED_PLANT = 6
    const val LAPTOP = 7
    const val BENCH = 8
    const val TOILET = 9
    const val SINK = 10
    const val REFRIGERATOR = 11
    const val OVEN = 12
    const val DESK = 13
    const val SHELF = 14
    const val CABINET = 15
    const val LAMP = 16

    /** Same names as the core's kObjectClassNames, indexed by core class id. */
    val coreNames = listOf(
        "unknown", "chair", "couch", "bed", "dining_table", "tv", "potted_plant", "laptop",
        "bench", "toilet", "sink", "refrigerator", "oven",
        "desk", "shelf", "cabinet", "lamp",
    )

    private val cocoToCore = mapOf(
        14 to BENCH, 61 to CHAIR, 62 to COUCH, 63 to POTTED_PLANT, 64 to BED, 66 to DINING_TABLE, 69 to TOILET,
        71 to TV, 72 to LAPTOP, 78 to OVEN, 80 to SINK, 81 to REFRIGERATOR,
    )

    /**
     * What people call each class (singular, plural), keyed by core name. Same words as the core's spoken landmark
     * phrases (landmark.cpp class_phrase) so the screen and the voice agree. "door" / "window" are the core's openings.
     */
    private val spoken = mapOf(
        "chair" to ("chair" to "chairs"), "couch" to ("sofa" to "sofas"), "bed" to ("bed" to "beds"),
        "dining_table" to ("table" to "tables"), "tv" to ("TV" to "TVs"), "potted_plant" to ("plant" to "plants"),
        "laptop" to ("laptop" to "laptops"), "bench" to ("bench" to "benches"), "toilet" to ("toilet" to "toilets"),
        "sink" to ("sink" to "sinks"), "refrigerator" to ("fridge" to "fridges"), "oven" to ("oven" to "ovens"),
        "desk" to ("desk" to "desks"), "shelf" to ("shelf" to "shelves"), "cabinet" to ("cabinet" to "cabinets"),
        "lamp" to ("lamp" to "lamps"), "door" to ("door" to "doors"), "window" to ("window" to "windows"),
    )

    /** Core class id for a detector class index (a float from the model output), [UNKNOWN] when not mapped. */
    fun coreClass(cocoIndex: Int): Int = cocoToCore[cocoIndex] ?: UNKNOWN

    fun coreName(coreClass: Int): String = coreNames.getOrElse(coreClass) { "unknown" }

    /**
     * Display word for a core class NAME (as the overlay sends it) or an opening kind; [plural] for counts other than 1.
     * An unknown name (newer core) is shown as itself with underscores as spaces, never dropped.
     */
    fun displayName(coreName: String, plural: Boolean = false): String {
        val p = spoken[coreName] ?: return coreName.replace('_', ' ').let { if (plural) "${it}s" else it }
        return if (plural) p.second else p.first
    }

    /** Display word for a core class id (camera-feed box labels). */
    fun displayName(coreClass: Int): String = displayName(coreName(coreClass))

    /** "chair 82%": the camera-feed box label; the score is clamped to 0..100 % and rounded. */
    fun boxLabel(coreClass: Int, score: Float): String {
        val pct = if (score.isFinite()) Math.round(score.coerceIn(0f, 1f) * 100f) else 0
        return "${displayName(coreClass)} $pct%"
    }
}
