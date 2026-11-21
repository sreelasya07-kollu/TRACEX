import Foundation
#if canImport(UIKit)
import UIKit
#endif

/// OS part of the governor Signals (the session adds frame time and drop rate). See ADR 0007.
public struct PlatformSignals: Equatable {
    public var thermalLevel: Int
    public var lowPowerMode: Bool
    public var batteryPct: Double
    public var charging: Bool
}

public enum ThermalMapping {
    /// ProcessInfo.ThermalState -> governor 0...3 (nominal, fair, serious, critical).
    public static func level(_ s: ProcessInfo.ThermalState) -> Int {
        switch s {
        case .nominal: return 0
        case .fair: return 1
        case .serious: return 2
        case .critical: return 3
        @unknown default: return 2
        }
    }
}

/// Observes thermal state, Low Power Mode and battery; reports PlatformSignals on change.
/// Handlers only copy values and call `onSignals`; they never block the AR delegate queue.
public final class ThermalAdapter {
    private let onSignals: (PlatformSignals) -> Void
    private var observers: [NSObjectProtocol] = []

    public init(onSignals: @escaping (PlatformSignals) -> Void) {
        self.onSignals = onSignals
    }

    public func start() {
        #if canImport(UIKit)
        UIDevice.current.isBatteryMonitoringEnabled = true
        #endif
        let nc = NotificationCenter.default
        var names: [Notification.Name] = [
            ProcessInfo.thermalStateDidChangeNotification,
            Notification.Name.NSProcessInfoPowerStateDidChange,
        ]
        #if canImport(UIKit)
        names += [UIDevice.batteryLevelDidChangeNotification, UIDevice.batteryStateDidChangeNotification]
        #endif
        for n in names {
            observers.append(nc.addObserver(forName: n, object: nil, queue: nil) { [weak self] _ in self?.emit() })
        }
        emit()
    }

    public func stop() {
        observers.forEach { NotificationCenter.default.removeObserver($0) }
        observers.removeAll()
    }

    public func current() -> PlatformSignals {
        var pct = 100.0
        var charging = false
        #if canImport(UIKit)
        let level = UIDevice.current.batteryLevel  // -1 when unknown (simulator)
        if level >= 0 { pct = Double(level) * 100 }
        let st = UIDevice.current.batteryState
        charging = st == .charging || st == .full
        #endif
        return PlatformSignals(
            thermalLevel: ThermalMapping.level(ProcessInfo.processInfo.thermalState),
            lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled,
            batteryPct: pct,
            charging: charging
        )
    }

    private func emit() { onSignals(current()) }
}
