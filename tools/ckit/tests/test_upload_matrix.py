"""Phase 7 failure matrix against the real dev stack (tusd + MinIO + verifier in docker compose).
Run via dev-server/failure_test.sh (or CKIT_DOCKER_TESTS=1 with the stack up).
Every case must end in a COMPLETE verified bundle or a clearly reported failure."""

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import uuid
from pathlib import Path

import pytest

from ckit.bundle.import_raw import import_raw
from ckit.simulate import simulate
from ckit.upload import STATE_FILE, Faults, TusUploader, UploadError

pytestmark = [
    pytest.mark.docker,
    pytest.mark.skipif(not os.environ.get("CKIT_DOCKER_TESTS"), reason="set CKIT_DOCKER_TESTS=1"),
]

TUS = os.environ.get("CKIT_TUS", "http://localhost:1080/files/")
VER = os.environ.get("CKIT_VERIFIER", "http://localhost:8081")
COMPOSE_DIR = Path(__file__).parents[2] / "dev-server"
RAW = Path(__file__).parent / "fixtures" / "raw_small"


def compose(*args: str) -> None:
    subprocess.run(["docker", "compose", *args], cwd=COMPOSE_DIR, check=True, capture_output=True)


@pytest.fixture(scope="module")
def template(tmp_path_factory) -> Path:
    root = tmp_path_factory.mktemp("matrix")
    src = root / "src"
    import_raw(RAW, src)
    out = root / "template"
    simulate(src, out)
    return out


@pytest.fixture
def bundle(template, tmp_path) -> Path:
    b = tmp_path / "bundle"
    shutil.copytree(template, b)
    m = json.loads((b / "manifest.json").read_text())
    m["bundle_id"] = "t-" + uuid.uuid4().hex[:12]  # fresh id per case
    (b / "manifest.json").write_text(json.dumps(m))
    return b


def bid(b: Path) -> str:
    return json.loads((b / "manifest.json").read_text())["bundle_id"]


def up(**kw) -> TusUploader:
    kw.setdefault("chunk_size", 16 * 1024)
    kw.setdefault("backoff_s", 0.3)
    return TusUploader(TUS, VER, **kw)


def test_happy_path_manifest_last_complete(bundle):
    u = up()
    order = u.files(bundle)
    assert order[-1] == "manifest.json" and order.count("manifest.json") == 1
    r = u.upload_bundle(bundle)
    assert r.status == "COMPLETE", r.reason
    assert r.server["verdict"]["verdict"] in ("PASS", "WARN")
    assert set(r.server["received"]) >= set(order)


def test_airplane_mode_server_unreachable_then_back(bundle):
    state = {"stopped": False}

    def go_offline(path, offset):
        if not state["stopped"] and path == "streams.mcap" and offset >= 32 * 1024:
            state["stopped"] = True
            compose("stop", "tusd")
            threading.Timer(3.0, lambda: compose("start", "tusd")).start()

    r = up(faults=Faults(on_chunk=go_offline), max_attempts=12).upload_bundle(bundle)
    assert state["stopped"]
    assert r.status == "COMPLETE", r.reason
    assert r.retries > 0  # backoff retries happened while offline


def test_app_kill_mid_file_then_resume_after_reboot(bundle, tmp_path):
    code = (
        "import os, signal\nfrom pathlib import Path\n"
        "from ckit.upload import TusUploader, Faults\n"
        "def kill(path, off):\n"
        "    if path == 'streams.mcap' and off >= 48 * 1024: os.kill(os.getpid(), signal.SIGKILL)\n"
        f"TusUploader({TUS!r}, {VER!r}, chunk_size=16*1024, faults=Faults(on_chunk=kill))"
        f".upload_bundle(Path({str(bundle)!r}))\n"
    )
    p = subprocess.run([sys.executable, "-c", code], capture_output=True)
    assert p.returncode == -signal.SIGKILL, p.stderr.decode()[-300:]
    state = json.loads((bundle / STATE_FILE).read_text())  # survived the kill (atomic write)
    entry = state["uploads"]["streams.mcap"]
    assert 0 < (bundle / "streams.mcap").stat().st_size
    from urllib.request import Request, urlopen

    h = urlopen(Request(entry["url"], method="HEAD", headers={"Tus-Resumable": "1.0.0"}))
    offset = int(h.headers["Upload-Offset"])
    assert 0 < offset < entry["size"]  # server kept the partial upload
    sent = []
    r = up(faults=Faults(on_chunk=lambda path, off: sent.append((path, off)))).upload_bundle(bundle)
    assert r.status == "COMPLETE", r.reason
    first = next(o for p_, o in sent if p_ == "streams.mcap")
    assert first > offset  # resumed from the server offset, did not restart at 0


def test_server_restart_mid_upload(bundle):
    state = {"done": False}

    def restart(path, offset):
        if not state["done"] and path == "streams.mcap" and offset >= 32 * 1024:
            state["done"] = True
            compose("restart", "tusd", "verifier")

    r = up(faults=Faults(on_chunk=restart), max_attempts=12).upload_bundle(bundle)
    assert state["done"] and r.status == "COMPLETE", r.reason


def test_truncated_upload_is_reported_incomplete_then_completes(bundle):
    with pytest.raises(UploadError, match="cut-off"):
        up(faults=Faults(stop_after_bytes=40 * 1024)).upload_bundle(bundle)
    import json as j
    from urllib.request import urlopen

    st = j.loads(urlopen(f"{VER}/bundles/{bid(bundle)}").read())
    assert st["status"] == "PENDING" and "manifest.json has not arrived" in st["reason"]
    assert "streams.mcap" in st["reason"]  # names the unfinished upload
    r = up().upload_bundle(bundle)  # resume
    assert r.status == "COMPLETE", r.reason


def test_corrupted_chunk_detected_then_repaired(bundle):
    r = up(faults=Faults(corrupt_path="streams.mcap")).upload_bundle(bundle)
    assert r.repaired == ["streams.mcap"], r
    assert r.status == "COMPLETE", r.reason


def test_tampered_file_ends_corrupt_with_clear_reason(bundle):
    cam = bundle / "camera.json"
    data = bytearray(cam.read_bytes())
    i = next(k for k, c in enumerate(data) if chr(c).isdigit())
    data[i] = ord("0") if data[i] != ord("0") else ord("1")  # one digit changed after hashing
    cam.write_bytes(bytes(data))
    r = up().upload_bundle(bundle, wait_s=5)
    assert r.status == "CORRUPT"
    assert "camera.json" in r.reason and "sha256" in r.reason  # clearly reported, never COMPLETE


def test_cli_requires_consent_and_deletes_only_after_complete(bundle):
    from typer.testing import CliRunner

    from ckit.cli import app

    r = CliRunner().invoke(app, ["upload", str(bundle), "--server", TUS, "--verifier", VER])
    assert r.exit_code == 2 and bundle.exists()
    r = CliRunner().invoke(
        app,
        [
            "upload",
            str(bundle),
            "--server",
            TUS,
            "--verifier",
            VER,
            "--consent",
            "--delete-after-verify",
        ],
    )
    assert r.exit_code == 0, r.output
    assert not bundle.exists()
