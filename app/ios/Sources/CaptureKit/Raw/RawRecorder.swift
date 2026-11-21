// Phase 0 throwaway raw recorder (docs/program-spec/recording-protocol.md).
// Callbacks only enqueue; one serial queue does I/O and JPEG encoding. Bounded backlog, drops logged.
import Foundation

#if os(iOS) && canImport(ARKit)
import ARKit
import CoreImage
import CoreMotion

public final class RawRecorder: NSObject, ARSessionDelegate, @unchecked Sendable {
    public let dir: URL
    public private(set) var droppedCount = 0
    private let io = DispatchQueue(label: "dev.capturekit.raw.io")
    private let maxBacklog = 256
    private var backlog = 0
    private let lock = NSLock()
    private var handles: [String: FileHandle] = [:]
    private var lastKeyframe: TimeInterval = -1
    private var frameIndex = 0
    private var lastIntrinsics = ""
    private let ci = CIContext()
    private let motion = CMMotionManager()
    private let motionQueue = OperationQueue()
    public var clockCheck = false
    public var keyframeInterval: TimeInterval = 0.5

    public init(dir: URL) {
        self.dir = dir
        super.init()
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("frames"), withIntermediateDirectories: true)
    }

    // MARK: lifecycle
    public func start() {
        writeDevice()
        motion.gyroUpdateInterval = 1.0 / 200
        motion.accelerometerUpdateInterval = 1.0 / 200
        motion.startGyroUpdates(to: motionQueue) { [weak self] d, _ in
            guard let self, let d else { return }
            self.imu("gyro", d.timestamp, d.rotationRate.x, d.rotationRate.y, d.rotationRate.z)
        }
        motion.startAccelerometerUpdates(to: motionQueue) { [weak self] d, _ in
            guard let self, let d else { return }
            // CoreMotion reports g units with gravity; convert to m/s^2 specific force.
            let g = 9.80665
            self.imu("accel", d.timestamp, d.acceleration.x * g, d.acceleration.y * g, d.acceleration.z * g)
        }
    }

    public func stop() {
        motion.stopGyroUpdates()
        motion.stopAccelerometerUpdates()
        io.sync { handles.values.forEach { try? $0.close() }; handles.removeAll() }
        if droppedCount > 0 { NSLog("CKRawRecorder: %d dropped writes", droppedCount) }
    }

    // MARK: ARSessionDelegate
    public func session(_ session: ARSession, didUpdate frame: ARFrame) {
        let t = Self.ns(frame.timestamp)
        if clockCheck { clock("ar", t); return }
        let cam = frame.camera
        let T = cam.transform  // world_from_camera, ARKit camera axes (conventions.md)
        let q = simd_quatf(simd_float3x3(columns: (simd_make_float3(T.columns.0), simd_make_float3(T.columns.1), simd_make_float3(T.columns.2))))
        let (state, reason) = Self.mapTracking(cam.trackingState)
        line("poses.jsonl", String(format: "{\"t_ns\":%lld,\"p\":[%.6f,%.6f,%.6f],\"q\":[%.8f,%.8f,%.8f,%.8f],\"tracking\":\"%@\",\"reason\":\"%@\"}",
                                   t, T.columns.3.x, T.columns.3.y, T.columns.3.z, q.real, q.imag.x, q.imag.y, q.imag.z, state, reason))
        let K = cam.intrinsics  // column-major
        let intr = String(format: "\"w\":%d,\"h\":%d,\"fx\":%.4f,\"fy\":%.4f,\"cx\":%.4f,\"cy\":%.4f",
                          Int(cam.imageResolution.width), Int(cam.imageResolution.height), K[0][0], K[1][1], K[2][0], K[2][1])
        if intr != lastIntrinsics { lastIntrinsics = intr; line("intrinsics.jsonl", "{\"t_ns\":\(t),\(intr)}") }
        if let pts = frame.rawFeaturePoints?.points, !pts.isEmpty {
            let body = pts.map { String(format: "[%.4f,%.4f,%.4f]", $0.x, $0.y, $0.z) }.joined(separator: ",")
            line("points.jsonl", "{\"t_ns\":\(t),\"pts\":[\(body)]}")
        }
        if frame.timestamp - lastKeyframe >= keyframeInterval {
            lastKeyframe = frame.timestamp
            let i = frameIndex; frameIndex += 1
            let image = CIImage(cvPixelBuffer: frame.capturedImage)  // sensor orientation, no EXIF
            enqueue {
                let name = String(format: "frames/%06d.jpg", i)
                let opts = [kCGImageDestinationLossyCompressionQuality as CIImageRepresentationOption: 0.92]
                if let data = self.ci.jpegRepresentation(of: image, colorSpace: CGColorSpaceCreateDeviceRGB(), options: opts) {
                    try? data.write(to: self.dir.appendingPathComponent(name))
                    self.write("frames.jsonl", "{\"i\":\(i),\"t_ns\":\(t),\"file\":\"\(name)\",\"exposure_ns\":\(Self.ns(cam.exposureDuration))}\n")
                }
            }
        }
    }

    public func session(_ session: ARSession, didUpdate anchors: [ARAnchor]) {
        guard !clockCheck, let t = session.currentFrame.map({ Self.ns($0.timestamp) }) else { return }
        for case let p as ARPlaneAnchor in anchors {
            let T = p.transform
            let c = T * simd_float4(p.center, 1)
            let n = simd_make_float3(T.columns.1)
            let u = simd_make_float3(T.columns.0)
            let kind: String
            switch p.classification {
            case .floor: kind = "floor"
            case .ceiling: kind = "ceiling"
            case .wall: kind = "wall"
            case .table: kind = "table"
            case .seat: kind = "seat"
            case .door: kind = "door"
            case .window: kind = "window"
            default: kind = p.alignment == .horizontal ? "horizontal_up" : "vertical"
            }
            line("planes.jsonl", String(format: "{\"t_ns\":%lld,\"id\":%d,\"kind\":\"%@\",\"center\":[%.4f,%.4f,%.4f],\"normal\":[%.6f,%.6f,%.6f],\"axis_u\":[%.6f,%.6f,%.6f],\"extent\":[%.4f,%.4f]}",
                                        t, abs(p.identifier.hashValue % 1_000_000), kind, c.x, c.y, c.z, n.x, n.y, n.z, u.x, u.y, u.z,
                                        p.planeExtent.width, p.planeExtent.height))
        }
    }

    // MARK: writing
    private func imu(_ type: String, _ ts: TimeInterval, _ x: Double, _ y: Double, _ z: Double) {
        if clockCheck { clock(type, Self.ns(ts)); return }
        line("imu.jsonl", String(format: "{\"t_ns\":%lld,\"type\":\"%@\",\"v\":[%.6f,%.6f,%.6f]}", Self.ns(ts), type, x, y, z))
    }

    private func clock(_ src: String, _ raw: Int64) {
        let mono = Self.ns(ProcessInfo.processInfo.systemUptime)
        line("clockcheck.jsonl", "{\"src\":\"\(src)\",\"raw_ns\":\(raw),\"mono_ns\":\(mono)}")
    }

    private func line(_ file: String, _ json: String) { enqueue { self.write(file, json + "\n") } }

    private func enqueue(_ job: @escaping () -> Void) {
        lock.lock()
        if backlog >= maxBacklog { droppedCount += 1; lock.unlock(); if droppedCount % 100 == 1 { NSLog("CKRawRecorder: dropped=%d", droppedCount) }; return }
        backlog += 1
        lock.unlock()
        io.async { job(); self.lock.lock(); self.backlog -= 1; self.lock.unlock() }
    }

    private func write(_ file: String, _ s: String) {
        let h: FileHandle
        if let existing = handles[file] { h = existing } else {
            let url = dir.appendingPathComponent(file)
            FileManager.default.createFile(atPath: url.path, contents: nil)
            guard let nh = try? FileHandle(forWritingTo: url) else { return }
            handles[file] = nh; h = nh
        }
        h.write(Data(s.utf8))
    }

    private func writeDevice() {
        var sys = utsname(); uname(&sys)
        let model = withUnsafePointer(to: &sys.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) } }
        let json = """
        {"platform": "ios", "model": "\(model)", "os": "iOS \(UIDevice.current.systemVersion)", "app_version": "0.0.1",
         "pose_convention": "arkit", "imu_frame": "ios_device", "clock_base": "systemUptime", "clock_offset_ns": 0}
        """
        try? json.write(to: dir.appendingPathComponent("device.json"), atomically: true, encoding: .utf8)
    }

    static func ns(_ s: TimeInterval) -> Int64 { Int64((s * 1e9).rounded()) }

    static func mapTracking(_ s: ARCamera.TrackingState) -> (String, String) {
        switch s {
        case .normal: return ("NORMAL", "NONE")
        case .notAvailable: return ("NOT_AVAILABLE", "NONE")
        case .limited(let r):
            switch r {
            case .initializing: return ("LIMITED", "INITIALIZING")
            case .excessiveMotion: return ("LIMITED", "EXCESSIVE_MOTION")
            case .insufficientFeatures: return ("LIMITED", "INSUFFICIENT_FEATURES")
            case .relocalizing: return ("LIMITED", "RELOCALIZING")
            @unknown default: return ("LIMITED", "OTHER")
            }
        }
    }
}
#endif
