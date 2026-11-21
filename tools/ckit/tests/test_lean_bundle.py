"""Issue #10 / ADR 0034: lean bundle (no IMU by default, schema 1.4.0) and the one-file .zip."""

import json
import shutil
import zipfile
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.bundle import validate as validate_mod
from ckit.bundle.import_raw import import_raw, import_raw_selected
from ckit.bundle.streams import IMU_TOPICS, read_streams
from ckit.bundle.validate import validate
from ckit.cli import app
from ckit.simulate import simulate

RAW = Path(__file__).parent / "fixtures" / "raw_small"
runner = CliRunner()


@pytest.fixture(scope="module")
def source(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("lean_src") / "src"
    import_raw(RAW, out)
    return out


@pytest.fixture(scope="module")
def lean(source, tmp_path_factory) -> Path:
    """A bundle recorded by the live session with its defaults: the lean 1.4.0 form."""
    out = tmp_path_factory.mktemp("lean_run") / "scan_20261120_100000"
    assert simulate(source, out)["state"] == "COMPLETE"
    return out


def _copy(src: Path, tmp_path: Path, name: str = "scan_20261120_100000") -> Path:
    dst = tmp_path / name
    shutil.copytree(src, dst)
    return dst


def test_default_session_does_not_record_the_imu_and_says_so(lean):
    st = read_streams(lean / "streams.mcap")
    assert st.finalized
    assert st.get("/imu/gyro") == [] and st.get("/imu/accel") == []
    assert len(st.get("/pose")) > 0 and len(st.get("/frame_index")) > 0  # the rest is unchanged
    m = json.loads((lean / "manifest.json").read_text())
    assert m["schema_version"] == "1.4.0"
    assert m["counts"]["imu_gyro"] == 0 and m["counts"]["imu_accel"] == 0
    assert m["capture_settings"]["record_imu"] is False
    rep = validate(lean, "quality")
    assert rep.codes("ERROR") == set(), rep.summary()
    assert "UNKNOWN_MINOR" not in rep.codes()


def test_record_imu_is_the_research_switch(source, tmp_path):
    out = tmp_path / "imu"
    assert simulate(source, out, {"record_imu": 1})["state"] == "COMPLETE"
    st = read_streams(out / "streams.mcap")
    assert len(st.get("/imu/gyro")) > 0
    m = json.loads((out / "manifest.json").read_text())
    assert m["schema_version"] != "1.4.0" and m["capture_settings"]["record_imu"] is True
    assert validate(out, "quality").codes("ERROR") == set()


def test_the_imu_channels_are_optional_only_from_1_4_0(lean, tmp_path, monkeypatch):
    real = validate_mod.read_streams

    def without_imu(path):  # a writer that left the channels out entirely
        st = real(path)
        st.topics_present = st.topics_present - IMU_TOPICS
        return st

    monkeypatch.setattr(validate_mod, "read_streams", without_imu)
    assert "MISSING_CHANNEL" not in validate(lean, "structural").codes()  # 1.4.0: optional
    old = _copy(lean, tmp_path, "old")
    m = json.loads((old / "manifest.json").read_text())
    m["schema_version"] = "1.3.0"
    (old / "manifest.json").write_text(json.dumps(m))
    rep = validate(old, "structural")
    missing = [i.where for i in rep.issues if i.code == "MISSING_CHANNEL"]
    assert sorted(missing) == ["/imu/accel", "/imu/gyro"]  # still required before 1.4.0


def test_pack_makes_one_archive_that_validates_like_the_folder(lean, tmp_path):
    folder = _copy(lean, tmp_path)
    r = runner.invoke(app, ["pack", str(folder)])
    assert r.exit_code == 0, r.output
    z = tmp_path / "scan_20261120_100000.zip"
    assert z.is_file() and folder.is_dir()  # the folder stays unless asked
    assert "validate=" in r.output
    with zipfile.ZipFile(z) as zf:
        names = zf.namelist()
    assert names[0] == "manifest.json" and "streams.mcap" in names
    assert not any(n.startswith(("imu", "debug")) for n in names)
    on_folder, on_zip = validate(folder, "quality"), validate(z, "quality")
    assert on_zip.bundle == str(z)
    assert on_zip.verdict == on_folder.verdict
    assert on_zip.codes() == on_folder.codes()


def test_pack_is_reproducible_and_smaller_than_the_loose_files(lean, tmp_path):
    folder = _copy(lean, tmp_path)
    a = tmp_path / "a.zip"
    b = tmp_path / "b.zip"
    assert runner.invoke(app, ["pack", str(folder), "--out", str(a)]).exit_code == 0
    assert runner.invoke(app, ["pack", str(folder), "--out", str(b)]).exit_code == 0
    assert a.read_bytes() == b.read_bytes()
    loose = sum(p.stat().st_size for p in folder.rglob("*") if p.is_file())
    assert a.stat().st_size < loose


def test_delete_source_only_after_a_verified_valid_archive(lean, tmp_path):
    folder = _copy(lean, tmp_path)
    r = runner.invoke(app, ["pack", str(folder), "--delete-source"])
    assert r.exit_code == 0, r.output
    assert not folder.exists() and (tmp_path / "scan_20261120_100000.zip").is_file()
    assert validate(tmp_path / "scan_20261120_100000.zip").verdict != "FAIL"


def test_a_folder_that_fails_validation_or_packing_is_never_deleted(lean, tmp_path):
    bad_major = _copy(lean, tmp_path, "bad_major")
    m = json.loads((bad_major / "manifest.json").read_text())
    m["schema_version"] = "2.0.0"  # packs fine (the manifest is not self-listed) but validates FAIL
    (bad_major / "manifest.json").write_text(json.dumps(m))
    r = runner.invoke(app, ["pack", str(bad_major), "--delete-source"])
    assert r.exit_code == 1 and bad_major.is_dir()
    broken = _copy(lean, tmp_path, "broken")
    next((broken / "keyframes").glob("*.jpg")).unlink()
    r = runner.invoke(app, ["pack", str(broken), "--delete-source"])
    assert r.exit_code == 1 and broken.is_dir()
    assert "listed file missing" in r.output
    assert not (tmp_path / "broken.zip").exists()
    r = runner.invoke(app, ["pack", str(tmp_path / "nothing")])
    assert r.exit_code == 1 and "not a bundle folder" in r.output


def test_out_inside_the_folder_never_deletes_the_only_copy(lean, tmp_path):
    """Reviewer finding: --out inside the folder + --delete-source used to remove both."""
    folder = _copy(lean, tmp_path)
    before = sorted(p.name for p in folder.rglob("*"))
    r = runner.invoke(
        app, ["pack", str(folder), "--out", str(folder / "scan.zip"), "--delete-source"]
    )
    assert r.exit_code == 1 and "outside the bundle folder" in r.output
    assert folder.is_dir() and sorted(p.name for p in folder.rglob("*")) == before


def test_a_bad_archive_is_a_validator_error_not_a_crash(tmp_path):
    junk = tmp_path / "junk.zip"
    junk.write_bytes(b"definitely not a zip")
    rep = validate(junk)
    assert rep.verdict == "FAIL" and rep.codes("ERROR") == {"ARCHIVE_UNREADABLE"}
    evil = tmp_path / "evil.zip"
    with zipfile.ZipFile(evil, "w") as zf:
        zf.writestr("../escape.txt", b"x")
        zf.writestr("manifest.json", b"{}")
    rep = validate(evil)
    assert rep.codes("ERROR") == {"ARCHIVE_UNREADABLE"}
    assert not (tmp_path.parent / "escape.txt").exists()
    cli = runner.invoke(app, ["validate", str(evil)])
    assert cli.exit_code == 1 and "ARCHIVE_UNREADABLE" in cli.output


def test_replay_reads_the_archive_like_the_folder(lean, tmp_path):
    folder = _copy(lean, tmp_path)
    z = tmp_path / "scan_20261120_100000.zip"
    assert runner.invoke(app, ["pack", str(folder), "--out", str(z)]).exit_code == 0
    args = ["--sink", "metrics", "--sink", "quality", "--fast"]
    a = runner.invoke(app, ["replay", str(folder), *args])
    b = runner.invoke(app, ["replay", str(z), *args])
    assert a.exit_code == 0 and b.exit_code == 0, b.output
    assert a.output == b.output and len(a.output) > 100
    bad = tmp_path / "bad.zip"
    bad.write_bytes(b"nope")
    r = runner.invoke(app, ["replay", str(bad), *args])
    assert r.exit_code == 1 and "Traceback" not in r.output


def test_recon_bench_reads_the_archive_like_the_folder(tmp_path):
    from ckit.core import load

    load()
    folder = tmp_path / "scan_20261120_110000"
    import_raw_selected(RAW, folder)
    z = tmp_path / "scan_20261120_110000.zip"
    assert runner.invoke(app, ["pack", str(folder), "--out", str(z)]).exit_code == 0
    ja, jb = tmp_path / "a.json", tmp_path / "b.json"
    ra = runner.invoke(app, ["recon-bench", str(folder), "--json", str(ja)])
    rb = runner.invoke(app, ["recon-bench", str(z), "--baseline", str(z), "--json", str(jb)])
    assert ra.exit_code == 0 and rb.exit_code == 0, rb.output
    a, b = json.loads(ja.read_text()), json.loads(jb.read_text())
    assert {k: v for k, v in b.items() if k not in ("baseline", "delta")} == a
    assert all(v == 0 for v in b["delta"].values())
    bad = tmp_path / "bad.zip"
    bad.write_bytes(b"nope")
    r = runner.invoke(app, ["recon-bench", str(bad)])
    assert r.exit_code == 1 and "Traceback" not in r.output
