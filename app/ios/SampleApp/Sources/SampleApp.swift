// CaptureKit SampleApp (iOS). Generate the Xcode project with XcodeGen: `cd ios/SampleApp && xcodegen`.
import SwiftUI
#if os(iOS)
import ARKit
import CaptureKit

@main
struct SampleApp: App {
    var body: some Scene { WindowGroup { RootView() } }
}

struct RootView: View {
    var body: some View {
        NavigationStack {
            List {
                NavigationLink("Raw recorder") { RecorderView(clockCheck: false) }
                NavigationLink("Clock check") { RecorderView(clockCheck: true) }
            }.navigationTitle("CaptureKit")
        }
    }
}

/// Phase 0 raw recorder / clock-check screen. Sessions land in Documents/sessions (Files app visible).
struct RecorderView: View {
    let clockCheck: Bool
    @State private var session = ARSession()
    @State private var recorder: RawRecorder?
    @State private var status = "idle"

    var body: some View {
        VStack(spacing: 16) {
            Text(clockCheck ? "Clock check" : "Raw recorder").font(.headline)
            Text(status).font(.caption.monospaced())
            Button(recorder == nil ? "Start" : "Stop") { recorder == nil ? start() : stop() }
                .buttonStyle(.borderedProminent)
        }
        .onDisappear { stop() }
    }

    private func start() {
        let fmt = DateFormatter(); fmt.dateFormat = "yyyyMMdd_HHmmss"
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let dir = docs.appendingPathComponent("sessions/\(clockCheck ? "clock" : "raw")_\(fmt.string(from: Date()))")
        let r = RawRecorder(dir: dir)
        r.clockCheck = clockCheck
        session.delegate = r
        let cfg = ARWorldTrackingConfiguration()
        cfg.worldAlignment = .gravity
        cfg.planeDetection = [.horizontal, .vertical]
        session.run(cfg, options: [.resetTracking, .removeExistingAnchors])
        r.start()
        recorder = r
        status = "recording \(dir.lastPathComponent)"
    }

    private func stop() {
        guard let r = recorder else { return }
        session.pause()
        r.stop()
        status = "saved \(r.dir.lastPathComponent), dropped=\(r.droppedCount)"
        recorder = nil
    }
}
#endif
