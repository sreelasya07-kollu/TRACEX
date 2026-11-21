"""Phase 7: verifier logic against a local tusd-layout store (no docker), plus tus client units."""

import hashlib
import json
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "dev-server" / "verifier"))
from bundle_verify import DirStore, Verifier  # noqa: E402

from ckit.upload import UploadResult, _meta, delete_after_verify  # noqa: E402


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class Fake:
    """Writes tusd-style objects: <key> and <key>.info (MetaData, Size)."""

    def __init__(self, root: Path, bundle_id="b1"):
        self.root, self.bid, self.n = root, bundle_id, 0

    def put(self, path: str, data: bytes, declared: bytes | None = None, complete=True, cut=None):
        self.n += 1
        key = f"u{self.n:04d}"
        meta = {
            "bundle_id": self.bid,
            "path": path,
            "sha256": sha(declared if declared is not None else data),
        }
        (self.root / f"{key}.info").write_text(json.dumps({"Size": len(data), "MetaData": meta}))
        if complete or cut:
            (self.root / key).write_bytes(data[:cut] if cut else data)
        time.sleep(0.002)  # distinct mtimes: latest upload of a path wins

    def manifest(self, files: dict[str, bytes], **kw):
        m = {"files": [{"path": p, "size": len(d), "sha256": sha(d)} for p, d in files.items()]}
        m["validation"] = {"verdict": "PASS"}
        self.put("manifest.json", json.dumps(m).encode(), **kw)


@pytest.fixture
def env(tmp_path):
    return Fake(tmp_path), Verifier(DirStore(tmp_path))


FILES = {"streams.mcap": b"a" * 1000, "camera.json": b"{}", "keyframes/000000.jpg": b"jpeg" * 50}


def test_pending_until_manifest(env):
    f, v = env
    f.put("streams.mcap", FILES["streams.mcap"])
    s = v.check("b1")
    assert s.status == "PENDING" and "manifest.json has not arrived" in s.reason


def test_complete(env):
    f, v = env
    for p, d in FILES.items():
        f.put(p, d)
    f.manifest(FILES)
    s = v.check("b1")
    assert s.status == "COMPLETE" and s.verdict == {"verdict": "PASS"}


def test_incomplete_missing_file_then_resolves_when_it_arrives(env):
    f, v = env
    f.put("streams.mcap", FILES["streams.mcap"])
    f.put("camera.json", FILES["camera.json"])
    f.manifest(FILES)
    s = v.check("b1")
    assert s.status == "INCOMPLETE" and s.missing == ["keyframes/000000.jpg"]
    f.put("keyframes/000000.jpg", FILES["keyframes/000000.jpg"])
    assert v.check("b1").status == "COMPLETE"


def test_truncated_upload_is_incomplete_not_complete(env):
    f, v = env
    f.put("streams.mcap", FILES["streams.mcap"])
    f.put("camera.json", FILES["camera.json"])
    f.put("keyframes/000000.jpg", FILES["keyframes/000000.jpg"], cut=60)  # object shorter than Size
    f.manifest(FILES)
    s = v.check("b1")
    assert s.status == "INCOMPLETE" and "keyframes/000000.jpg" in s.missing


def test_corrupted_chunk_detected_and_repaired_by_reupload(env):
    f, v = env
    bad = bytearray(FILES["streams.mcap"])
    bad[10] ^= 0xFF
    f.put("streams.mcap", bytes(bad), declared=FILES["streams.mcap"])
    f.put("camera.json", FILES["camera.json"])
    f.put("keyframes/000000.jpg", FILES["keyframes/000000.jpg"])
    f.manifest(FILES)
    s = v.check("b1")
    assert s.status == "CORRUPT" and s.corrupt[0]["path"] == "streams.mcap"
    f.put("streams.mcap", FILES["streams.mcap"])
    f.manifest(FILES)
    assert v.check("b1").status == "COMPLETE"


def test_tampered_file_after_manifest_is_corrupt(env):
    f, v = env
    for p, d in FILES.items():
        f.put(p, d if p != "camera.json" else b'{"x":1}')  # same size class, different bytes
    f.manifest(FILES)
    s = v.check("b1")
    assert s.status == "CORRUPT" and "camera.json" in s.reason


def test_size_mismatch_and_unreadable_manifest(env):
    f, v = env
    f.put("camera.json", b"{}" * 3)
    f.put("manifest.json", b"not json")
    assert v.check("b1").status == "CORRUPT"
    f2 = Fake(f.root, "b2")
    f2.put("camera.json", b"{}" * 3)
    f2.manifest({"camera.json": b"{}"})
    assert v.check("b2").corrupt[0]["reason"].startswith("size ")


def test_bundles_are_isolated(env):
    f, v = env
    for p, d in FILES.items():
        f.put(p, d)
    f.manifest(FILES)
    assert v.check("b1").status == "COMPLETE"
    assert v.check("other").status == "PENDING"


def test_status_recomputed_from_storage_after_restart(env):
    f, v = env
    for p, d in FILES.items():
        f.put(p, d)
    f.manifest(FILES)
    v2 = Verifier(DirStore(f.root))  # a fresh process: no memory of any hook event
    assert v2.check("b1").status == "COMPLETE"


def test_tus_metadata_encoding():
    assert _meta({"a": "x"}) == "a eA=="


def test_retention_requires_consent_and_complete(tmp_path):
    d = tmp_path / "b"
    d.mkdir()
    (d / "f").write_text("x")
    assert not delete_after_verify(d, UploadResult(status="COMPLETE"), consent=False) and d.exists()
    assert not delete_after_verify(d, UploadResult(status="CORRUPT"), consent=True) and d.exists()
    assert delete_after_verify(d, UploadResult(status="COMPLETE"), consent=True) and not d.exists()
