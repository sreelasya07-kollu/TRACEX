// Phase 6 iOS capture session over the core C API. Compiled only when the core is linked
// (Package.swift sets up CCaptureKit when CAPTUREKIT_CORE_LIB_DIR is defined) and on iOS.
// UNVERIFIED on this dev machine (no macOS): the first Mac/CI run is its first compile.
#if os(iOS) && canImport(ARKit) && canImport(CCaptureKit)
import ARKit
import CCaptureKit
import CoreImage
import CoreMotion
import Foundation
import GRDB
import UIKit

public protocol CaptureSessionDelegate: AnyObject {
    func captureSession(_ s: CaptureSession, didEmit event: [String: Any])
    func captureSession(_ s: CaptureSession, didUpdatePolicy p: ck_policy)
}

/// Owns one ck_session. Callbacks only copy into the core's bounded queues (never block ARKit's
/// delegate queue); keyframe JPEGs are encoded on a serial queue; ARKit frames are not retained.
public final class CaptureSession: NSObject, ARSessionDelegate, @unchecked Sendable {
    public let dir: URL
    public weak var delegate: CaptureSessionDelegate?
    public private(set) var droppedAtEdge = 0

    private var handle: OpaquePointer?
    private let arSession = ARSession()
    private let motion = CMMotionManager()
    private let motionQueue = OperationQueue()
    private let encodeQueue = DispatchQueue(label: "dev.capturekit.encode")
    private let pollQueue = DispatchQueue(label: "dev.capturekit.poll")
    private var pollTimer: DispatchSourceTimer?
    private let ci = CIContext()
    private var frameCounter = 0
    private var nextFrameId: UInt32 = 0
    private let frameEveryN: Int
    private let thermal: ThermalAdapter
    private let store: SessionStore?

    // Ring of copied camera images awaiting a possible keyframe request (never retains ARFrames).
    private var ring: [UInt32: CVPixelBuffer] = [:]
    private var ringOrder: [UInt32] = []
    private let ringLock = NSLock()
    private static let ringSize = 6

    public init(dir: URL, frameEveryN: Int = 6 /* ARKit 60 fps -> ~10 fps candidates */) {
        self.dir = dir
        self.frameEveryN = frameEveryN
        self.store = try? SessionStore()
        self.thermal = ThermalAdapter { _ in }
        super.init()
        arSession.delegate = self
        motionQueue.name = "dev.capturekit.motion"
    }

    private static func tNs(_ seconds: TimeInterval) -> Int64 { Int64((seconds * 1e9).rounded()) }
    private var nowNs: Int64 { Self.tNs(ProcessInfo.processInfo.systemUptime) } // same base as ARFrame/CMLogItem

    public func start(config: [String: Any] = [:]) throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var cfg: [String: Any] = [
            "platform": "ios", "model": UIDevice.current.model, "os": UIDevice.current.systemVersion,
            "clock_base": "systemUptime", "clock_offset_ns": 0,
        ]
        config.forEach { cfg[$0.key] = $0.value }
        let json = String(data: try JSONSerialization.data(withJSONObject: cfg, options: [.sortedKeys]), encoding: .utf8)!
        var err = CK_OK
        guard let h = ck_session_create(dir.path, json, &err) else { throw NSError(domain: "CaptureKit", code: Int(err.rawValue)) }
        handle = h
        try store?.upsert(dir: dir.path, status: "recording")
        guard ck_session_start(h, nowNs) == CK_OK else { throw NSError(domain: "CaptureKit", code: 1) }

        let cfgAR = ARWorldTrackingConfiguration()
        cfgAR.worldAlignment = .gravity                       // Y up, gravity aligned
        cfgAR.planeDetection = [.horizontal, .vertical]
        if let hi = ARWorldTrackingConfiguration.supportedVideoFormats.max(by: { $0.imageResolution.width < $1.imageResolution.width }) {
            _ = hi // Phase 6 device task: evaluate the recommended high-res format for keyframes vs thermal cost.
        }
        arSession.run(cfgAR, options: [.resetTracking, .removeExistingAnchors])
        startMotion()
        thermal.stop()
        thermal.start()
        startPolling()
    }

    private func startMotion() {
        guard motion.isGyroAvailable, motion.isAccelerometerAvailable else { return }
        motion.gyroUpdateInterval = 1.0 / 200
        motion.accelerometerUpdateInterval = 1.0 / 200
        motion.startGyroUpdates(to: motionQueue) { [weak self] d, _ in
            guard let self, let h = self.handle, let d else { return }
            var m = ck_imu(t_ns: Self.tNs(d.timestamp), v: (d.rotationRate.x, d.rotationRate.y, d.rotationRate.z))
            self.note(ck_ingest_gyro(h, &m))
        }
        motion.startAccelerometerUpdates(to: motionQueue) { [weak self] d, _ in
            guard let self, let h = self.handle, let d else { return }
            // CoreMotion reports acceleration in g; the bundle stores m/s^2 specific force (negated gravity sign kept as delivered).
            var m = ck_imu(t_ns: Self.tNs(d.timestamp), v: (d.acceleration.x * 9.80665, d.acceleration.y * 9.80665, d.acceleration.z * 9.80665))
            self.note(ck_ingest_accel(h, &m))
        }
    }

    private func note(_ st: ck_status) { if st == CK_ERR_FULL { droppedAtEdge += 1 } }

    // MARK: ARSessionDelegate (ARKit camera.transform is world_from_camera with GL axes)
    public func session(_ session: ARSession, didUpdate frame: ARFrame) {
        guard let h = handle else { return }
        let t = Self.tNs(frame.timestamp)
        let cam = frame.camera
        let tf = cam.transform
        let q = simd_quatf(tf)
        var pose = ck_pose(t_ns: t, t: (Double(tf.columns.3.x), Double(tf.columns.3.y), Double(tf.columns.3.z)),
                           q_wxyz: (Double(q.real), Double(q.imag.x), Double(q.imag.y), Double(q.imag.z)),
                           tracking_state: Self.trackingState(cam.trackingState).0, tracking_reason: Self.trackingState(cam.trackingState).1)
        note(ck_ingest_pose(h, &pose, Int32(CK_POSE_ARKIT_GL.rawValue)))

        let K = cam.intrinsics, res = cam.imageResolution
        var intr = ck_intrinsics(t_ns: t, width: UInt32(res.width), height: UInt32(res.height),
                                 fx: Double(K[0][0]), fy: Double(K[1][1]), cx: Double(K[2][0]), cy: Double(K[2][1]))
        _ = ck_ingest_intrinsics(h, &intr) // the core stores changes only

        if let pts = frame.rawFeaturePoints?.points, !pts.isEmpty, frameCounter % 12 == 0 {
            var flat = [Float](); flat.reserveCapacity(pts.count * 3)
            for p in pts { flat += [p.x, p.y, p.z] }
            note(ck_ingest_points(h, t, flat, pts.count))
        }

        frameCounter += 1
        if frameCounter % frameEveryN == 0 { ingestFrame(h, frame, t) }
    }

    private func ingestFrame(_ h: OpaquePointer, _ frame: ARFrame, _ t: Int64) {
        let pb = frame.capturedImage
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(pb, 0) else { return }
        let id = nextFrameId; nextFrameId &+= 1
        var f = ck_frame(t_ns: t, frame_id: id, exposure_ns: Int64(frame.camera.exposureDuration * 1e9),
                         luma: base.assumingMemoryBound(to: UInt8.self),
                         width: Int32(CVPixelBufferGetWidthOfPlane(pb, 0)), height: Int32(CVPixelBufferGetHeightOfPlane(pb, 0)),
                         stride: Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 0)))
        let st = ck_ingest_frame(h, &f)
        note(st)
        guard st == CK_OK, let copy = Self.copy(pb) else { return }
        ringLock.lock(); defer { ringLock.unlock() }
        ring[id] = copy; ringOrder.append(id)
        while ringOrder.count > Self.ringSize { ring.removeValue(forKey: ringOrder.removeFirst()) }
    }

    /// Deep copy so ARKit's frame pool is never starved by retained buffers.
    private static func copy(_ src: CVPixelBuffer) -> CVPixelBuffer? {
        var dst: CVPixelBuffer?
        CVPixelBufferCreate(nil, CVPixelBufferGetWidth(src), CVPixelBufferGetHeight(src), CVPixelBufferGetPixelFormatType(src), nil, &dst)
        guard let d = dst else { return nil }
        CVPixelBufferLockBaseAddress(src, .readOnly); CVPixelBufferLockBaseAddress(d, [])
        defer { CVPixelBufferUnlockBaseAddress(src, .readOnly); CVPixelBufferUnlockBaseAddress(d, []) }
        for p in 0..<CVPixelBufferGetPlaneCount(src) {
            guard let s = CVPixelBufferGetBaseAddressOfPlane(src, p), let t = CVPixelBufferGetBaseAddressOfPlane(d, p) else { return nil }
            let rows = CVPixelBufferGetHeightOfPlane(src, p)
            let sb = CVPixelBufferGetBytesPerRowOfPlane(src, p), db = CVPixelBufferGetBytesPerRowOfPlane(d, p)
            for r in 0..<rows { memcpy(t + r * db, s + r * sb, min(sb, db)) }
        }
        return d
    }

    public func session(_ session: ARSession, didAdd anchors: [ARAnchor]) { planes(anchors) }
    public func session(_ session: ARSession, didUpdate anchors: [ARAnchor]) { planes(anchors) }
    private func planes(_ anchors: [ARAnchor]) {
        guard let h = handle else { return }
        let t = nowNs
        for case let a as ARPlaneAnchor in anchors {
            let m = a.transform
            let c = m * SIMD4<Float>(a.center, 1)
            var info = (id: a.identifier.uuidString, kind: Self.kind(a))
            info.id.withCString { idp in info.kind.withCString { kp in
                var p = ck_plane(t_ns: t, id: idp, kind: kp,
                                 center: (Double(c.x), Double(c.y), Double(c.z)),
                                 normal: (Double(m.columns.1.x), Double(m.columns.1.y), Double(m.columns.1.z)),
                                 axis_u: (Double(m.columns.0.x), Double(m.columns.0.y), Double(m.columns.0.z)),
                                 extent_u: Double(a.planeExtent.width), extent_v: Double(a.planeExtent.height))
                note(ck_ingest_plane(h, &p))
            } }
        }
    }

    private static func kind(_ a: ARPlaneAnchor) -> String {
        switch a.classification {
        case .floor: return "floor"
        case .ceiling: return "ceiling"
        case .wall: return "wall"
        case .table, .seat: return "table"
        default: return a.alignment == .vertical ? "wall" : "other"
        }
    }

    // MARK: interruptions (calls, Siri, backgrounding): pause the core, resume when ARKit recovers
    public func sessionWasInterrupted(_ session: ARSession) { if let h = handle { _ = ck_session_pause(h, nowNs) } }
    public func sessionInterruptionEnded(_ session: ARSession) {
        guard let h = handle else { return }
        arSession.run(arSession.configuration ?? ARWorldTrackingConfiguration(), options: [.resetTracking])
        _ = ck_session_resume(h, nowNs)
    }

    // MARK: events, keyframes
    private func startPolling() {
        let t = DispatchSource.makeTimerSource(queue: pollQueue)
        t.schedule(deadline: .now(), repeating: .milliseconds(50))
        t.setEventHandler { [weak self] in self?.drain() }
        t.resume(); pollTimer = t
    }

    private func drain() {
        guard let h = handle else { return }
        var buf = [CChar](repeating: 0, count: 1 << 16)
        let n = ck_poll_events(h, &buf, buf.count)
        if n > 0, let text = String(bytes: buf[0..<n].map { UInt8(bitPattern: $0) }, encoding: .utf8) {
            for line in text.split(separator: "\n") {
                guard let d = line.data(using: .utf8), let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { continue }
                if o["type"] as? String == "keyframe_request", let id = o["frame_id"] as? Int, let file = o["file"] as? String {
                    encodeQueue.async { [weak self] in self?.encode(UInt32(id), file) }
                }
                delegate?.captureSession(self, didEmit: o)
            }
        }
        var p = ck_policy(); if ck_policy_get(h, &p) == CK_OK { delegate?.captureSession(self, didUpdatePolicy: p) }
    }

    private func encode(_ id: UInt32, _ rel: String) {
        guard let h = handle else { return }
        ringLock.lock(); let pb = ring[id]; ringLock.unlock()
        guard let pb else { _ = ck_keyframe_written(h, id, 0); return }
        let target = dir.appendingPathComponent(rel), tmp = dir.appendingPathComponent(rel + ".tmp")
        try? FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        let image = CIImage(cvPixelBuffer: pb)
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let data = ci.jpegRepresentation(of: image, colorSpace: space, options: [kCGImageDestinationLossyCompressionQuality as CIImageRepresentationOption: 0.92]),
              (try? data.write(to: tmp, options: .atomic)) != nil,
              (try? FileManager.default.replaceItemAt(target, withItemAt: tmp)) != nil else { _ = ck_keyframe_written(h, id, 0); return }
        _ = ck_keyframe_written(h, id, 1)
    }

    // MARK: lifecycle
    public func pause() { if let h = handle { _ = ck_session_pause(h, nowNs) } }
    public func resume() { if let h = handle { _ = ck_session_resume(h, nowNs) } }

    /// Stops capture, lets pending keyframes encode, finalizes the bundle.
    public func finish() -> ck_status {
        guard let h = handle else { return CK_ERR_STATE }
        arSession.pause(); motion.stopGyroUpdates(); motion.stopAccelerometerUpdates(); thermal.stop()
        _ = ck_session_stop(h, nowNs)
        Thread.sleep(forTimeInterval: 0.3); pollQueue.sync { drain() }
        encodeQueue.sync {}
        pollTimer?.cancel()
        let st = ck_session_finalize(h)
        try? store?.setStatus(dir: dir.path, status: st == CK_OK ? "complete" : "unrecoverable")
        ck_session_destroy(h); handle = nil
        return st
    }

    /// App start: sessions still marked "recording" were interrupted: recover them in the core.
    public static func recoverInterrupted() -> [(String, String)] {
        guard let store = try? SessionStore(), let rows = try? store.interrupted() else { return [] }
        return rows.map { dir in
            var buf = [CChar](repeating: 0, count: 1024)
            let ok = ck_recover(dir, &buf, buf.count) == CK_OK
            try? store.setStatus(dir: dir, status: ok ? "recovered" : "unrecoverable")
            return (dir, String(cString: buf))
        }
    }

    // conventions.md: tracking_state 1 NORMAL, 2 LIMITED, 3 NOT_AVAILABLE; reasons 0 NONE..8 OTHER
    static func trackingState(_ s: ARCamera.TrackingState) -> (Int32, Int32) {
        switch s {
        case .normal: return (1, 0)
        case .notAvailable: return (3, 8)
        case .limited(let r):
            switch r {
            case .initializing: return (2, 1)
            case .excessiveMotion: return (2, 2)
            case .insufficientFeatures: return (2, 3)
            case .relocalizing: return (2, 5)
            @unknown default: return (2, 8)
            }
        }
    }
}

/// GRDB table of capture sessions (same role as Android's Room SessionStore).
final class SessionStore {
    private let db: DatabaseQueue
    init() throws {
        let url = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("capturekit.sqlite")
        db = try DatabaseQueue(path: url.path)
        try db.write { try $0.execute(sql: "CREATE TABLE IF NOT EXISTS sessions (dir TEXT PRIMARY KEY, status TEXT NOT NULL, startedAt REAL NOT NULL)") }
    }
    func upsert(dir: String, status: String) throws {
        try db.write { try $0.execute(sql: "INSERT OR REPLACE INTO sessions (dir, status, startedAt) VALUES (?, ?, ?)", arguments: [dir, status, Date().timeIntervalSince1970]) }
    }
    func setStatus(dir: String, status: String) throws {
        try db.write { try $0.execute(sql: "UPDATE sessions SET status = ? WHERE dir = ?", arguments: [status, dir]) }
    }
    func interrupted() throws -> [String] {
        try db.read { try String.fetchAll($0, sql: "SELECT dir FROM sessions WHERE status = 'recording'") }
    }
}
#endif
