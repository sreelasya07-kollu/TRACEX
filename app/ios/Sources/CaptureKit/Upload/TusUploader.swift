// Phase 7 iOS upload over TUSKit 3.7: one tus upload per file (metadata bundle_id/path/sha256),
// manifest.json LAST, resumable after kill/reboot (TUSKit persists uploads; call start() on launch),
// background-capable URLSession. Mirrors tools/ckit/upload.py and the Android TusUploader.
// UNVERIFIED on this dev machine (no macOS): first compile happens on a Mac / CI macOS job.
import Foundation
import CryptoKit
#if canImport(TUSKit)
import TUSKit

public final class BundleUploader: NSObject, TUSClientDelegate, @unchecked Sendable {
    public enum Outcome { case complete, retry(String), failed(String) }

    private let client: TUSClient
    private let verifier: URL?
    private let bundle: URL
    private let bundleId: String
    private let consent: Bool
    private let deleteAfterVerify: Bool
    private var pending = Set<UUID>()
    private var finished: ((Outcome) -> Void)?
    private let lock = NSLock()
    private var manifestSent = false

    public init(bundle: URL, tusServer: URL, verifier: URL?, consent: Bool, deleteAfterVerify: Bool = false) throws {
        self.bundle = bundle
        self.verifier = verifier
        self.consent = consent
        self.deleteAfterVerify = deleteAfterVerify
        let manifest = try Data(contentsOf: bundle.appendingPathComponent("manifest.json"))
        guard let obj = try JSONSerialization.jsonObject(with: manifest) as? [String: Any], let id = obj["bundle_id"] as? String
        else { throw NSError(domain: "CaptureKit.Upload", code: 1, userInfo: [NSLocalizedDescriptionKey: "manifest has no bundle_id"]) }
        bundleId = id
        client = try TUSClient(server: tusServer, sessionIdentifier: "dev.capturekit.upload",
                               sessionConfiguration: .background(withIdentifier: "dev.capturekit.upload"),
                               storageDirectory: bundle.deletingLastPathComponent().appendingPathComponent(".tus"),
                               chunkSize: 256 * 1024)
        super.init()
        client.delegate = self
    }

    static func sha256(_ url: URL) throws -> String {
        var h = SHA256()
        let f = try FileHandle(forReadingFrom: url)
        defer { try? f.close() }
        while let d = try f.read(upToCount: 1 << 20), !d.isEmpty { h.update(data: d) }
        return h.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// Files of the bundle with manifest.json last.
    static func order(_ bundle: URL) -> [String] {
        let e = FileManager.default.enumerator(at: bundle, includingPropertiesForKeys: [.isRegularFileKey])
        var all: [String] = []
        while let u = e?.nextObject() as? URL {
            guard (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true, !u.lastPathComponent.hasSuffix(".tmp") else { continue }
            all.append(u.path.replacingOccurrences(of: bundle.path + "/", with: ""))
        }
        all.sort()
        return all.filter { $0 != "manifest.json" } + all.filter { $0 == "manifest.json" }
    }

    /// Resumes uploads TUSKit persisted from a previous launch, then (re)queues the bundle.
    public func run(completion: @escaping (Outcome) -> Void) {
        guard consent else { return completion(.failed("no upload consent recorded")) }
        finished = completion
        _ = client.start()  // resume unfinished uploads from before a kill/reboot
        queue(Self.order(bundle).filter { $0 != "manifest.json" })
    }

    private func queue(_ rels: [String]) {
        lock.lock(); defer { lock.unlock() }
        for rel in rels {
            let url = bundle.appendingPathComponent(rel)
            guard let sha = try? Self.sha256(url) else { continue }
            if let id = try? client.uploadFileAt(filePath: url, context: ["bundle_id": bundleId, "path": rel, "sha256": sha, "filename": url.lastPathComponent]) {
                pending.insert(id)
            }
        }
        if rels.contains("manifest.json") { manifestSent = true }
    }

    // MARK: TUSClientDelegate
    public func didFinishUpload(id: UUID, url: URL, context: [String: String]?, client: TUSClient) {
        lock.lock(); pending.remove(id); let done = pending.isEmpty; let sent = manifestSent; lock.unlock()
        guard done else { return }
        if !sent { queue(["manifest.json"]) } else { waitForVerdict(repairedOnce: false) }
    }
    public func uploadFailed(id: UUID, error: Error, context: [String: String]?, client: TUSClient) { finished?(.retry(error.localizedDescription)) }
    public func fileError(error: TUSClientError, client: TUSClient) { finished?(.retry(String(describing: error))) }
    public func didStartUpload(id: UUID, context: [String: String]?, client: TUSClient) {}
    public func totalProgress(bytesUploaded: Int, totalBytes: Int, client: TUSClient) {}
    public func progressFor(id: UUID, context: [String: String]?, bytesUploaded: Int, totalBytes: Int, client: TUSClient) {}

    // MARK: verdict
    private func waitForVerdict(repairedOnce: Bool, attempt: Int = 0) {
        guard let verifier else { return finished?(.complete) ?? () }
        var req = URLRequest(url: verifier.appendingPathComponent("bundles/\(bundleId)"))
        req.timeoutInterval = 20
        URLSession.shared.dataTask(with: req) { [self] data, _, err in
            guard let data, err == nil, let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let st = o["status"] as? String
            else { return finished?(.retry(err?.localizedDescription ?? "verifier unreachable")) ?? () }
            switch st {
            case "COMPLETE":
                if deleteAfterVerify && consent { try? FileManager.default.removeItem(at: bundle) }  // only after server verification
                finished?(.complete)
            case "CORRUPT" where !repairedOnce:
                let bad = ((o["corrupt"] as? [[String: Any]]) ?? []).compactMap { $0["path"] as? String }
                queue(bad.filter { $0 != "manifest.json" } + ["manifest.json"])  // resend damaged files, manifest last again
                // The next didFinishUpload with an empty queue re-enters waitForVerdict (repairedOnce handled below).
                repaired = true
            case "CORRUPT": finished?(.failed((o["reason"] as? String) ?? "corrupt"))
            default:
                if attempt > 60 { return finished?(.retry("server status \(st)")) ?? () }
                DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) { self.waitForVerdict(repairedOnce: self.repaired, attempt: attempt + 1) }
            }
        }.resume()
    }
    private var repaired = false
}
#endif
