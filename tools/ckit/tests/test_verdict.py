"""Phase 7: verdict table completeness, generated docs in sync, C++ on-device validator parity."""

import json
import re
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.bundle.import_raw import import_raw
from ckit.bundle.validate import validate
from ckit.cli import app
from ckit.core import CAPTUREKIT_ROOT
from ckit.simulate import simulate
from ckit.verdict import MESSAGES, for_report, to_json, to_markdown

RAW = Path(__file__).parent / "fixtures" / "raw_small"
SPEC = (CAPTUREKIT_ROOT / "spec" / "scanbundle" / "SPEC.md").read_text()
VALIDATOR_SRC = (Path(__file__).parents[1] / "ckit" / "bundle" / "validate.py").read_text()


def test_every_spec_and_validator_code_has_a_message():
    spec_codes = set(re.findall(r"^\| ([A-Z][A-Z0-9_]+) \| (?:ERROR|WARN|INFO) \|", SPEC, re.M))
    src_codes = set(re.findall(r'"([A-Z][A-Z0-9_]{5,})"', VALIDATOR_SRC)) - {
        "ERROR", "WARN", "INFO", "PASS", "FAIL",
        # /events codes read by the pause-aware POSE_GAP check, not validation issue codes
        "SESSION_PAUSED", "SESSION_RESUMED", "SESSION_STOPPED", "SESSION_RECOVERED",
    }  # fmt: skip
    assert len(spec_codes) >= 24
    assert (spec_codes | src_codes) <= set(MESSAGES), (spec_codes | src_codes) - set(MESSAGES)
    for code, m in MESSAGES.items():
        assert m.title and m.text and m.action, code
        assert len(m.title.split()) <= 6, code
        spec_sev = re.search(rf"^\| {code} \| (ERROR|WARN|INFO) \|", SPEC, re.M)
        if spec_sev:
            assert spec_sev.group(1) == m.severity, code  # table agrees with the spec


def test_severity_policy():
    for code, m in MESSAGES.items():
        assert (
            m.next == {"ERROR": "rescan", "WARN": "proceed_with_note", "INFO": "none"}[m.severity]
        ), code


def test_docs_are_generated_from_table():
    doc = (CAPTUREKIT_ROOT / "docs" / "verdict-messages.md").read_text()
    assert doc == to_markdown(), "run: ckit verdict-docs --out capturekit/docs/verdict-messages.md"


def test_android_asset_is_generated_from_table():
    asset = CAPTUREKIT_ROOT / "android/sample-app/src/main/assets/verdict_messages.json"
    assert asset.read_bytes().decode("utf-8") == to_json(), (
        "run: ckit verdict-export --out ../android/sample-app/src/main/assets/verdict_messages.json"
    )
    d = json.loads(to_json())
    assert set(d["codes"]) == set(MESSAGES) and set(d["headlines"]) == {"FAIL", "WARN", "PASS"}


def test_verdict_export_cli(tmp_path):
    out = tmp_path / "v.json"
    r = CliRunner().invoke(app, ["verdict-export", "--out", str(out)])
    assert r.exit_code == 0, r.output
    assert out.read_bytes().decode("utf-8") == to_json()


def test_for_report_orders_and_decides():
    r = for_report([
        {"code": "NO_AR_GEOMETRY", "severity": "INFO", "message": "m"},
        {"code": "POSE_GAP", "severity": "WARN", "message": "gap"},
        {"code": "KEYFRAME_FILE_MISSING", "severity": "ERROR", "message": "x"},
    ])  # fmt: skip
    assert r["verdict"] == "FAIL" and r["rescan"]
    assert [i["code"] for i in r["items"]] == [
        "KEYFRAME_FILE_MISSING",
        "POSE_GAP",
        "NO_AR_GEOMETRY",
    ]
    assert for_report([])["verdict"] == "PASS"
    assert for_report([{"code": "POSE_GAP", "severity": "WARN"}])["verdict"] == "WARN"


@pytest.fixture(scope="module")
def source(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("vsrc") / "src"
    import_raw(RAW, out)
    return out


def _codes(manifest_validation: dict) -> set[tuple[str, str]]:
    return {(i["code"], i["severity"]) for i in manifest_validation["issues"]}


def _py_semantic(bundle: Path) -> set[tuple[str, str]]:
    rep = validate(bundle, "semantic")
    return {(i["code"], i["severity"]) for i in rep.to_json()["issues"] if i["level"] == "semantic"}


@pytest.mark.parametrize("scenario", ["plain", "hot"])
def test_cpp_on_device_validator_matches_python(source, tmp_path, scenario):
    out = tmp_path / scenario
    kw = {"platform_events": [(0.5, {"thermal": 3})]} if scenario == "hot" else {}
    simulate(source, out, **kw)
    manifest = json.loads((out / "manifest.json").read_text())
    cpp = _codes(manifest["validation"])
    py = _py_semantic(out)
    assert cpp == py, cpp ^ py
    assert manifest["validation"]["verdict"] == validate(out, "semantic").verdict


def test_recovered_bundle_carries_partial_warning_and_matches_python(source, tmp_path):
    import subprocess
    import sys

    from ckit import capi

    out = tmp_path / "killed"
    code = (
        "from pathlib import Path\nfrom ckit.simulate import simulate\n"
        f"simulate(Path({str(source)!r}), Path({str(out)!r}), kill_after_events=800)\n"
    )
    assert subprocess.run([sys.executable, "-c", code], capture_output=True).returncode == -9
    st, _ = capi.recover(out)
    assert st == 0
    manifest = json.loads((out / "manifest.json").read_text())
    cpp = _codes(manifest["validation"])
    assert ("PARTIAL_BUNDLE", "WARN") in cpp
    assert cpp == _py_semantic(out)


def test_verdict_cli(source, tmp_path):

    from ckit.cli import app

    out = tmp_path / "v"
    simulate(source, out)
    r = CliRunner().invoke(app, ["verdict", str(out)])
    assert r.exit_code == 0, r.output
    d = json.loads(r.output)
    assert d["verdict"] in ("PASS", "WARN") and "headline" in d


assert app  # keep import (CLI registration is exercised by test_verdict_cli)


def _cpp_validate(bundle: Path) -> set[tuple[str, str]]:
    from ckit.core import load

    return _codes(json.loads(load().bundle.validate_semantic(str(bundle))))


def test_cpp_and_python_agree_on_damaged_bundles(source, tmp_path):
    import shutil

    good = tmp_path / "good"
    simulate(source, good)
    kfs = sorted((good / "keyframes").glob("*.jpg"))
    assert len(kfs) > 0

    missing = tmp_path / "missing"
    shutil.copytree(good, missing)
    (missing / "keyframes" / kfs[0].name).unlink()
    cpp, py = _cpp_validate(missing), _py_semantic(missing)
    assert ("KEYFRAME_FILE_MISSING", "ERROR") in cpp and cpp == py

    # Streams cut mid-file: both validators must agree on what the readable prefix shows.
    cut = tmp_path / "cut"
    shutil.copytree(good, cut)
    data = (cut / "streams.mcap").read_bytes()
    (cut / "streams.mcap").write_bytes(data[: len(data) // 2])
    cpp_cut = {c for c, _ in _cpp_validate(cut)}
    py_cut = {c for c, _ in _py_semantic(cut)}
    for code in ("MISSING_INTRINSICS", "KEYFRAME_WITHOUT_POSE", "FEW_KEYFRAMES"):
        assert (code in cpp_cut) == (code in py_cut), code
