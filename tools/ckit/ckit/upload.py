"""tus 1.0 client for Scan Bundles (reference for the Kotlin/Swift uploaders; also `ckit upload`).

One tus upload per file (metadata: bundle_id, path, sha256), manifest.json LAST. Upload URLs are
persisted in `<bundle>/.ckit_upload.json` so a killed process or rebooted device resumes via HEAD
(Upload-Offset). Transient errors retry with exponential backoff. After the manifest is up, the
server verdict is polled; files the server reports CORRUPT are re-uploaded once from scratch.
"""

from __future__ import annotations

import base64
import hashlib
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

TUS = {"Tus-Resumable": "1.0.0"}
STATE_FILE = ".ckit_upload.json"
FINAL = {"COMPLETE", "CORRUPT"}


class UploadError(Exception):
    pass


@dataclass
class Faults:
    """Test-only fault injection (the failure matrix); never set in production."""

    corrupt_path: str | None = None  # flip one byte in the first chunk sent for this path
    stop_after_bytes: int | None = None  # simulate a cut-off: stop the whole upload mid-file
    on_chunk: Callable[[str, int], None] | None = None  # called after each PATCH (path, offset)


@dataclass
class UploadResult:
    status: str = "?"
    reason: str = ""
    repaired: list[str] = field(default_factory=list)
    retries: int = 0
    server: dict = field(default_factory=dict)


def _meta(d: dict) -> str:
    return ",".join(f"{k} {base64.b64encode(v.encode()).decode()}" for k, v in d.items())


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


class TusUploader:
    def __init__(
        self,
        server: str,
        verifier: str | None = None,
        chunk_size: int = 256 * 1024,
        max_attempts: int = 8,
        backoff_s: float = 0.5,
        sleep: Callable[[float], None] = time.sleep,
        faults: Faults | None = None,
        timeout: float = 15.0,
    ) -> None:
        self.server = server.rstrip("/") + "/"
        self.verifier = verifier.rstrip("/") if verifier else None
        self.chunk = chunk_size
        self.max_attempts = max_attempts
        self.backoff = backoff_s
        self.sleep = sleep
        self.faults = faults or Faults()
        self.timeout = timeout
        self.retries = 0
        self._corrupted = False

    # --- HTTP with retry/backoff -------------------------------------------------------------
    def _req(self, method: str, url: str, headers: dict, data: bytes | None = None):
        last: Exception | None = None
        for attempt in range(self.max_attempts):
            try:
                req = urllib.request.Request(
                    url, data=data, method=method, headers={**TUS, **headers}
                )
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    return r.status, dict(r.headers), r.read()
            except urllib.error.HTTPError as e:
                if e.code in (404, 410):  # unknown upload URL: caller starts a new one
                    return e.code, dict(e.headers), b""
                if e.code < 500 and e.code != 423:
                    raise UploadError(f"{method} {url}: HTTP {e.code}") from e
                last = e
            except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as e:
                last = e  # airplane mode, server down/restarting, reset connection
            self.retries += 1
            self.sleep(min(self.backoff * 2**attempt, 8.0))
        raise UploadError(f"{method} {url}: gave up after {self.max_attempts} attempts: {last}")

    # --- one file ------------------------------------------------------------------------------
    def _upload_file(
        self, bundle: Path, rel: str, bundle_id: str, state: dict, fresh: bool
    ) -> None:
        p = bundle / rel
        size = p.stat().st_size
        digest = sha256_file(p)
        entry = state["uploads"].get(rel)
        url = None if fresh or not entry or entry.get("sha256") != digest else entry["url"]
        offset = 0
        if url:
            code, hdr, _ = self._req("HEAD", url, {})
            if code in (404, 410):
                url = None
            else:
                offset = int(hdr.get("Upload-Offset", hdr.get("upload-offset", 0)))
        if not url:
            code, hdr, _ = self._req(
                "POST", self.server,
                {"Upload-Length": str(size),
                 "Upload-Metadata": _meta({"bundle_id": bundle_id, "path": rel, "sha256": digest,
                                           "filename": Path(rel).name})},
            )  # fmt: skip
            url = hdr.get("Location") or hdr.get("location")
            if not url:
                raise UploadError("server returned no Location")
            state["uploads"][rel] = {"url": url, "sha256": digest, "size": size}
            self._save(bundle, state)
            offset = 0
        with open(p, "rb") as f:
            while offset < size or (size == 0 and offset == 0 and not entry):
                f.seek(offset)
                data = f.read(self.chunk)
                if self.faults.corrupt_path == rel and not self._corrupted and data:
                    data = bytes([data[0] ^ 0xFF]) + data[1:]
                    self._corrupted = True
                _, hdr, _ = self._req(
                    "PATCH", url,
                    {
                        "Upload-Offset": str(offset),
                        "Content-Type": "application/offset+octet-stream",
                    },
                    data,
                )  # fmt: skip
                offset = int(hdr.get("Upload-Offset", hdr.get("upload-offset", offset + len(data))))
                if self.faults.on_chunk:
                    self.faults.on_chunk(rel, offset)
                if (
                    self.faults.stop_after_bytes is not None
                    and offset >= self.faults.stop_after_bytes
                ):
                    raise UploadError(f"simulated cut-off in {rel} at {offset}/{size}")
                if size == 0:
                    break

    # --- bundle ------------------------------------------------------------------------------
    @staticmethod
    def _load(bundle: Path) -> dict:
        try:
            return json.loads((bundle / STATE_FILE).read_text())
        except (OSError, ValueError):
            return {"uploads": {}}

    @staticmethod
    def _save(bundle: Path, state: dict) -> None:
        tmp = bundle / (STATE_FILE + ".tmp")
        tmp.write_text(json.dumps(state))
        tmp.replace(bundle / STATE_FILE)  # atomic: a kill never leaves half a state file

    @staticmethod
    def files(bundle: Path) -> list[str]:
        names = sorted(
            p.relative_to(bundle).as_posix() for p in bundle.rglob("*")
            if p.is_file() and not p.name.startswith(".ckit_upload") and not p.name.endswith(".tmp")
        )  # fmt: skip
        return [n for n in names if n != "manifest.json"] + ["manifest.json"]  # manifest LAST

    def status(self, bundle_id: str) -> dict:
        assert self.verifier
        _, _, body = self._req("GET", f"{self.verifier}/bundles/{bundle_id}", {})
        return json.loads(body)

    def upload_bundle(self, bundle: Path, wait_s: float = 30.0) -> UploadResult:
        manifest = json.loads((bundle / "manifest.json").read_text())
        bid = manifest["bundle_id"]
        state = self._load(bundle)
        res = UploadResult()
        for rel in self.files(bundle):
            self._upload_file(bundle, rel, bid, state, fresh=False)
        if not self.verifier:
            res.status, res.retries = "UPLOADED", self.retries
            return res
        deadline = time.monotonic() + wait_s
        repaired = False
        while True:
            srv = self.status(bid)
            if srv["status"] in FINAL or time.monotonic() > deadline:
                if srv["status"] == "CORRUPT" and not repaired:
                    repaired = True  # re-send damaged files from scratch, manifest again last
                    bad = [c["path"] for c in srv["corrupt"]]
                    for rel in [b for b in bad if b != "manifest.json"] + ["manifest.json"]:
                        self._upload_file(bundle, rel, bid, state, fresh=True)
                    res.repaired = bad
                    deadline = time.monotonic() + wait_s
                    continue
                res.status, res.reason, res.server = srv["status"], srv["reason"], srv
                break
            if srv["status"] == "INCOMPLETE":  # files lost server-side: resend what is missing
                for rel in srv["missing"]:
                    self._upload_file(bundle, rel, bid, state, fresh=True)
            self.sleep(0.3)
        res.retries = self.retries
        return res


def delete_after_verify(bundle: Path, result: UploadResult, consent: bool) -> bool:
    """Local retention (consent stub): delete only after server verification AND consent."""
    import shutil

    if not consent or result.status != "COMPLETE":
        return False
    shutil.rmtree(bundle)
    return True
