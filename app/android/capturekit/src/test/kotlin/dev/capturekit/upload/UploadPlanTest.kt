package dev.capturekit.upload

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class UploadPlanTest {
    @Test
    fun manifestIsLastAndTempFilesSkipped() {
        val d = Files.createTempDirectory("b").toFile()
        File(d, "keyframes").mkdirs()
        File(d, "manifest.json").writeText("""{"bundle_id": "abc-1"}""")
        File(d, "streams.mcap").writeText("x")
        File(d, "camera.json").writeText("{}")
        File(d, "keyframes/000000.jpg").writeText("j")
        File(d, "keyframes/000001.jpg.tmp").writeText("half")
        File(d, ".ckit_upload.json").writeText("{}")
        assertEquals(listOf("camera.json", "keyframes/000000.jpg", "streams.mcap", "manifest.json"), UploadPlan.order(d))
        assertEquals("abc-1", UploadPlan.bundleId(File(d, "manifest.json").readText()))
        d.deleteRecursively()
    }

    @Test
    fun sha256Known() {
        val f = Files.createTempFile("s", ".bin").toFile().apply { writeText("abc") }
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", UploadPlan.sha256(f))
    }

    @Test
    fun parsesVerifierStatus() {
        val json = """{"bundle_id":"b","corrupt":[{"path":"streams.mcap","reason":"sha256 differs from the manifest"}],
            "missing":["camera.json","keyframes/000001.jpg"],"reason":"x","received":["a"],"status":"CORRUPT","verdict":null}"""
        val s = ServerStatus.parse(json)
        assertEquals("CORRUPT", s.status); assertTrue(s.isFinal)
        assertEquals(listOf("streams.mcap"), s.corruptPaths)
        assertEquals(listOf("camera.json", "keyframes/000001.jpg"), s.missing)
        assertEquals(ServerStatus("COMPLETE", "ok", emptyList(), emptyList()).status, ServerStatus.parse("""{"corrupt":[],"missing":[],"reason":"ok","status":"COMPLETE"}""").status)
        assertTrue(ServerStatus.parse("""{"corrupt":[],"missing":[],"reason":"","status":"PENDING"}""").let { !it.isFinal })
    }
}
