package dev.capturekit.capture

/**
 * Process-wide set of bundle dirs owned by a live SessionManager (pure JVM). A dir is added at the start of
 * SessionManager.start (before its DB row says "recording") and removed only after finish/abandon is done, so
 * recovery never touches a bundle that is still being written or finalized.
 */
object LiveSessions {
    private val dirs = HashSet<String>()

    @Synchronized fun add(dir: String) { dirs.add(dir) }
    @Synchronized fun remove(dir: String) { dirs.remove(dir) }
    @Synchronized fun contains(dir: String) = dir in dirs

    /** The interrupted dirs that recovery may touch: everything not owned by a live session. */
    fun recoverable(interrupted: List<String>): List<String> = interrupted.filterNot(::contains)
}
