package dev.capturekit.segment

import android.content.Context
import android.util.Log
import com.google.ai.edge.litert.Accelerator
import com.google.ai.edge.litert.CompiledModel
import com.google.ai.edge.litert.TensorBuffer

/**
 * SAM 2.1 Hiera-Tiny on LiteRT (issue #8 part A: SKELETON, verified on a phone in 8b). Two litert-community models,
 * fetched by scripts/fetch_models.sh --with-segmentation (gitignored assets, never committed):
 *  - encoder `sam2_tiny_image_encoder_v2_fp16.tflite`: [1,3,1024,1024] NCHW float32 ImageNet-normalised -> decoder-ready
 *    image_embeddings [1,256,64,64], feat_s1 [1,64,128,128], feat_s0 [1,32,256,256]
 *  - decoder `sam2_tiny_mask_decoder_v2_fp16.tflite`: inputs 0 image_embeddings, 1 sparse_prompt [1,2,256], 2 feat_s1,
 *    3 feat_s0 -> pred_masks [1,3,256,256] logits + iou_scores [1,3]; the point is encoded on the host ([SamPromptEncoder])
 * The image is squashed to 1024 x 1024 (as the model card does), so a 256 x 256 mask covers the whole camera image.
 * One encoder run per job, one decoder run per prompt (positive point = box centre; box prompts are an 8b spike).
 * UNVERIFIED until 8b: the encoder's output ORDER for the two equal-size maps (image_embeddings vs feat_s1; card order is
 * assumed), GPU availability in the pinned runtime (falls back to CPU, ~5-10 s per encoder run), nearest-neighbour
 * resampling instead of bilinear, and running on the sensor-oriented (not upright) image.
 * Single-threaded: [segment] runs on the segmentation thread only. Needs the device runtime: not JVM-testable.
 */
class LiteRtSam2Segmenter private constructor(
    private val enc: CompiledModel,
    private val dec: CompiledModel,
    private val prompt: SamPromptEncoder,
    override val name: String,
) : Segmenter {
    private val encIn: List<TensorBuffer> = enc.createInputBuffers()
    private val encOut: List<TensorBuffer> = enc.createOutputBuffers()
    private val decIn: List<TensorBuffer> = dec.createInputBuffers()
    private val decOut: List<TensorBuffer> = dec.createOutputBuffers()
    private val chw = FloatArray(3 * SIDE * SIDE)

    override fun segment(image: SegImage, prompts: List<SegPrompt>): List<MaskBits?>? {
        if (encIn.size != 1 || encOut.size != 3 || decIn.size != 4 || decOut.size != 2) {
            Log.e(TAG, "unexpected model layout: encoder ${encIn.size}->${encOut.size}, decoder ${decIn.size}->${decOut.size}")
            return null
        }
        if (image.w <= 0 || image.h <= 0) return null
        fillChw(image)
        encIn[0].writeFloat(chw)
        enc.run(encIn, encOut)
        val maps = encOut.map { it.readFloat() }
        val s0 = maps.firstOrNull { it.size == FEAT_S0 } ?: return null
        val pair = maps.filter { it.size == EMBED }
        if (pair.size != 2) return null
        decIn[0].writeFloat(pair[0])   // image_embeddings (card order; UNVERIFIED)
        decIn[2].writeFloat(pair[1])   // feat_s1
        decIn[3].writeFloat(s0)
        val out = ArrayList<MaskBits?>(prompts.size)
        for (p in prompts) {
            decIn[1].writeFloat(prompt.encode(p.x * SIDE / image.w, p.y * SIDE / image.h, SIDE))
            dec.run(decIn, decOut)
            val a = decOut[0].readFloat(); val b = decOut[1].readFloat()
            val masks = if (a.size == 3 * MASK * MASK) a else b
            val iou = if (a.size == 3 * MASK * MASK) b else a
            if (masks.size != 3 * MASK * MASK || iou.size != 3) return null
            var best = 0
            for (k in 1 until 3) if (iou[k] > iou[best]) best = k
            val bits = ByteArray(MASK * MASK)
            val off = best * MASK * MASK
            var any = false
            for (i in bits.indices) if (masks[off + i] > 0f) { bits[i] = 1; any = true }
            out.add(if (any) MaskBits(MASK, MASK, bits, iou[best]) else null)
        }
        return out
    }

    /** Camera image -> [1,3,1024,1024] ImageNet-normalised RGB (nearest-neighbour squash). */
    private fun fillChw(img: SegImage) {
        val plane = SIDE * SIDE
        for (yy in 0 until SIDE) {
            val sy = yy * img.h / SIDE
            for (xx in 0 until SIDE) {
                val c = img.argb(xx * img.w / SIDE, sy)
                val i = yy * SIDE + xx
                chw[i] = (((c shr 16) and 0xFF) / 255f - MEAN_R) / STD_R
                chw[plane + i] = (((c shr 8) and 0xFF) / 255f - MEAN_G) / STD_G
                chw[2 * plane + i] = ((c and 0xFF) / 255f - MEAN_B) / STD_B
            }
        }
    }

    override fun close() {
        for (b in encIn + encOut + decIn + decOut) try { b.close() } catch (_: Throwable) {}
        try { enc.close() } catch (_: Throwable) {}
        try { dec.close() } catch (_: Throwable) {}
    }

    companion object {
        private const val TAG = "CK/Segment"
        const val ENCODER_ASSET = "sam2_tiny_image_encoder_v2_fp16.tflite"
        const val DECODER_ASSET = "sam2_tiny_mask_decoder_v2_fp16.tflite"
        const val PROMPT_CONST_ASSET = "sam2_tiny_prompt_encode_const.bin"
        const val SIDE = 1024
        const val MASK = 256
        private const val EMBED = 256 * 64 * 64      // = 64 * 128 * 128 (feat_s1): same size
        private const val FEAT_S0 = 32 * 256 * 256
        private const val MEAN_R = 0.485f; private const val MEAN_G = 0.456f; private const val MEAN_B = 0.406f
        private const val STD_R = 0.229f; private const val STD_G = 0.224f; private const val STD_B = 0.225f

        /** True when all three files are packaged (cheap: opens and closes the asset streams). Never throws. */
        fun available(context: Context): Boolean = listOf(ENCODER_ASSET, DECODER_ASSET, PROMPT_CONST_ASSET).all {
            try { context.assets.open(it).close(); true } catch (_: Throwable) { false }
        }

        /** Loads both models (GPU first, then CPU), or null when a file is missing or neither accelerator compiles it. Slow: worker thread only. */
        fun open(context: Context): LiteRtSam2Segmenter? {
            if (!available(context)) return null
            val pe = try { context.assets.open(PROMPT_CONST_ASSET).use { SamPromptEncoder.fromLittleEndian(it.readBytes()) } } catch (_: Throwable) { null }
                ?: run { Log.e(TAG, "bad $PROMPT_CONST_ASSET"); return null }
            val (enc, encAcc) = compile(context, ENCODER_ASSET) ?: return null
            val (dec, decAcc) = compile(context, DECODER_ASSET) ?: run { enc.close(); return null }
            return try {
                LiteRtSam2Segmenter(enc, dec, pe, "sam2.1-tiny enc=$encAcc dec=$decAcc").also { Log.i(TAG, "segmenter ready: ${it.name}") }
            } catch (t: Throwable) {
                Log.e(TAG, "segmenter buffers failed", t); enc.close(); dec.close(); null
            }
        }

        private fun compile(context: Context, asset: String): Pair<CompiledModel, Accelerator>? {
            for (acc in listOf(Accelerator.GPU, Accelerator.CPU)) {
                try { return CompiledModel.create(context.assets, asset, CompiledModel.Options(acc)) to acc } catch (t: Throwable) {
                    Log.w(TAG, "$asset: ${acc.name} unavailable (${t.javaClass.simpleName}: ${t.message})")
                }
            }
            return null
        }
    }
}
