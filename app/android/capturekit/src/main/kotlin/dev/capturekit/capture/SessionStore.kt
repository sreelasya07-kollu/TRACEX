package dev.capturekit.capture

import android.content.Context
import androidx.room.Dao
import androidx.room.Database
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Room
import androidx.room.RoomDatabase

/** One row per capture session directory. Lets the app find sessions that were killed mid-scan. */
@Entity(tableName = "sessions")
data class SessionRow(
    @PrimaryKey val dir: String,
    val startedAtMs: Long,
    val status: String, // recording | complete | recovered | unrecoverable
)

@Dao
interface SessionDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    fun upsert(row: SessionRow)

    @Query("UPDATE sessions SET status = :status WHERE dir = :dir")
    fun setStatus(dir: String, status: String)

    @Query("SELECT * FROM sessions WHERE status = 'recording'")
    fun interrupted(): List<SessionRow>

    @Query("SELECT * FROM sessions ORDER BY startedAtMs DESC")
    fun all(): List<SessionRow>
}

@Database(entities = [SessionRow::class], version = 1, exportSchema = false)
abstract class CaptureDb : RoomDatabase() {
    abstract fun sessions(): SessionDao

    companion object {
        @Volatile private var inst: CaptureDb? = null
        fun get(ctx: Context): CaptureDb = inst ?: synchronized(this) {
            inst ?: Room.databaseBuilder(ctx.applicationContext, CaptureDb::class.java, "capturekit.db").build().also { inst = it }
        }
    }
}
