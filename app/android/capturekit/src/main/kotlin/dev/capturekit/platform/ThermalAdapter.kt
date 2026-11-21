package dev.capturekit.platform

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager

/** OS part of the governor Signals (the session adds frame time and drop rate). See ADR 0007. */
data class PlatformSignals(
    val thermalLevel: Int,
    val lowPowerMode: Boolean,
    val batteryPct: Double,
    val charging: Boolean,
)

/** Pure mapping rules (unit-tested on the JVM). */
object ThermalMapping {
    /** PowerManager.THERMAL_STATUS_* (0 NONE .. 6 SHUTDOWN) -> governor 0..3. */
    fun fromThermalStatus(status: Int): Int = when {
        status <= 0 -> 0
        status <= 2 -> 1 // LIGHT, MODERATE
        status == 3 -> 2 // SEVERE
        else -> 3 // CRITICAL, EMERGENCY, SHUTDOWN
    }

    /** getThermalHeadroom(10 s forecast): 1.0 = throttling expected. NaN = unsupported. */
    fun fromHeadroom(headroom: Float): Int = when {
        headroom.isNaN() -> 0
        headroom >= 1.0f -> 2
        headroom >= 0.85f -> 1
        else -> 0
    }

    fun combine(status: Int, headroom: Float): Int = maxOf(fromThermalStatus(status), fromHeadroom(headroom))
}

/**
 * Listens to PowerManager (thermal status API 29+, headroom API 30+) and battery broadcasts and
 * reports PlatformSignals. Callbacks are cheap and never block the AR thread.
 */
class ThermalAdapter(private val context: Context, private val onSignals: (PlatformSignals) -> Unit) {
    private val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
    private var thermalStatus = 0
    private var batteryPct = 100.0
    private var charging = false
    private var thermalListener: Any? = null

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            if (level >= 0 && scale > 0) batteryPct = 100.0 * level / scale
            val st = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
            charging = st == BatteryManager.BATTERY_STATUS_CHARGING || st == BatteryManager.BATTERY_STATUS_FULL
            emit()
        }
    }

    fun start() {
        context.registerReceiver(batteryReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val l = PowerManager.OnThermalStatusChangedListener { s -> thermalStatus = s; emit() }
            pm.addThermalStatusListener(l)
            thermalListener = l
            thermalStatus = pm.currentThermalStatus
        }
        emit()
    }

    fun stop() {
        runCatching { context.unregisterReceiver(batteryReceiver) }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            (thermalListener as? PowerManager.OnThermalStatusChangedListener)?.let { pm.removeThermalStatusListener(it) }
        }
    }

    /** Poll once per second from the session: headroom has no listener. */
    fun poll() = emit()

    private fun headroom(): Float =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) pm.getThermalHeadroom(10) else Float.NaN

    private fun emit() {
        onSignals(
            PlatformSignals(
                thermalLevel = ThermalMapping.combine(thermalStatus, headroom()),
                lowPowerMode = pm.isPowerSaveMode,
                batteryPct = batteryPct,
                charging = charging,
            ),
        )
    }
}
