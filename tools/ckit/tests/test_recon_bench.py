"""`ckit recon-bench`: offline reconstruction report is complete, deterministic, and diffable."""

import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.cli import app

RAW = Path(__file__).parent / "fixtures" / "raw_small"
KEYS = {
    "package_bytes",
    "keyframes",
    "registered_pct",
    "reproj_images",
    "mean_reproj_px",
    "sparse_points",
    "coverage_yaw_pct",
}


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    try:
        from ckit.core import load

        load()
    except ImportError as e:
        pytest.skip(f"_ckcore not built: {e}")
    from ckit.bundle.import_raw import import_raw_selected

    out = tmp_path_factory.mktemp("rb") / "bundle"
    import_raw_selected(RAW, out)
    return out


def test_yaw_coverage_counts_sectors():
    from ckit.recon_bench import yaw_coverage_pct

    # Identity rotation looks down +Z (yaw 0); 90 deg about world Y looks down +X.
    s = 2**-0.5
    ident, quarter = [1.0, 0.0, 0.0, 0.0], [s, 0.0, s, 0.0]
    assert yaw_coverage_pct([ident]) == pytest.approx(100 / 36, abs=0.01)
    assert yaw_coverage_pct([ident, ident, quarter]) == pytest.approx(200 / 36, abs=0.01)
    assert yaw_coverage_pct([]) == 0.0
    # Looking straight down has no yaw: contributes nothing.
    down = [s, s, 0.0, 0.0]
    assert yaw_coverage_pct([down]) == 0.0


def test_report_has_all_keys_and_runs_are_identical(bundle, tmp_path):
    from ckit.recon_bench import recon_bench, stable

    a, b = recon_bench(bundle), recon_bench(bundle)
    assert KEYS | {"seconds", "bundle"} <= set(a)
    assert a["keyframes"] >= 3 and a["package_bytes"] > 0
    assert 0.0 <= a["registered_pct"] <= 100.0 and 0.0 <= a["coverage_yaw_pct"] <= 100.0
    assert a["seconds"] >= 0.0
    assert a["sparse_points"] > 0 and a["mean_reproj_px"] is not None
    assert a["reproj_images"] > 0
    assert json.dumps(stable(a), sort_keys=True) == json.dumps(stable(b), sort_keys=True)
    assert "seconds" not in stable(a)


def test_cli_json_identical_and_baseline_deltas(bundle, tmp_path):
    runner = CliRunner()
    j1, j2 = tmp_path / "a.json", tmp_path / "b.json"
    r1 = runner.invoke(app, ["recon-bench", str(bundle), "--json", str(j1)])
    r2 = runner.invoke(app, ["recon-bench", str(bundle), "--json", str(j2)])
    assert r1.exit_code == 0, r1.output
    assert r2.exit_code == 0, r2.output
    assert j1.read_bytes() == j2.read_bytes()
    assert KEYS <= set(json.loads(j1.read_text()))
    jb = tmp_path / "c.json"
    r = runner.invoke(
        app, ["recon-bench", str(bundle), "--baseline", str(bundle), "--json", str(jb)]
    )
    assert r.exit_code == 0, r.output
    assert "delta" in r.output and "baseline" in r.output
    rep = json.loads(jb.read_text())
    assert KEYS <= set(rep["delta"]) and all(rep["delta"][k] == 0 for k in KEYS)
    assert rep["baseline"]["keyframes"] == rep["keyframes"]


def test_package_bytes_counts_manifest_files_only(bundle, tmp_path):
    import shutil

    from ckit.recon_bench import package_bytes

    copy = tmp_path / "copy"
    shutil.copytree(bundle, copy)
    before = package_bytes(copy)
    (copy / "stray_work_file.bin").write_bytes(b"x" * 1000)
    assert package_bytes(copy) == before
    (copy / "manifest.json").write_text("not json")
    every = sum(p.stat().st_size for p in copy.rglob("*") if p.is_file())
    assert package_bytes(copy) == every  # no readable manifest: every file counts


def test_too_few_keyframes_is_a_clean_cli_error(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    (empty / "manifest.json").write_text('{"files": []}')
    r = CliRunner().invoke(app, ["recon-bench", str(empty)])
    assert r.exit_code == 1
    assert "keyframes" in r.output and "Traceback" not in r.output
