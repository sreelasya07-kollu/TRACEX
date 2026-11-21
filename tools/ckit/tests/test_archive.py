"""Issue #10 / ADR 0034: one deterministic zip per scan, safe to open, verified before deleting."""

import hashlib
import json
import struct
import zipfile
from collections import namedtuple
from pathlib import Path

import pytest

from ckit.bundle import archive
from ckit.bundle.archive import (
    ArchiveError,
    bundle_dir,
    extract_archive,
    is_archive,
    is_inside,
    pack_bundle,
    verify_archive,
)

FILES = {
    "camera.json": b'{"model": "PINHOLE"}\n',
    "streams.mcap": b"MCAP0" + b"\x00" * 4096,
    "keyframes/000000.jpg": bytes(range(256)) * 8,
    "keyframes/000001.jpg": bytes(range(255, -1, -1)) * 8,
    "guidance_log.json": b"{}\n",
}


def make_bundle(root: Path, files: dict[str, bytes] | None = None) -> Path:
    """A minimal self-consistent bundle: the manifest lists every other file (size, sha256)."""
    files = FILES if files is None else files
    root.mkdir(parents=True)
    listed = []
    for name, data in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        listed.append({"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    (root / "manifest.json").write_text(json.dumps({"schema_version": "1.4.0", "files": listed}))
    return root


@pytest.fixture
def bundle(tmp_path) -> Path:
    return make_bundle(tmp_path / "scan_20261120_100000")


def test_pack_default_name_manifest_first_jpeg_stored_rest_deflated(bundle):
    r = pack_bundle(bundle)
    assert r.archive == bundle.parent / "scan_20261120_100000.zip"
    assert r.entries == len(FILES) + 1
    manifest_bytes = (bundle / "manifest.json").stat().st_size
    assert r.source_bytes == sum(len(d) for d in FILES.values()) + manifest_bytes
    assert r.archive_bytes == r.archive.stat().st_size
    with zipfile.ZipFile(r.archive) as zf:
        infos = zf.infolist()
        assert infos[0].filename == "manifest.json"
        assert [i.filename for i in infos[1:]] == sorted(FILES)
        for i in infos:
            assert i.date_time == archive.ZIP_EPOCH
            expect = zipfile.ZIP_STORED if i.filename.endswith(".jpg") else zipfile.ZIP_DEFLATED
            assert i.compress_type == expect, i.filename
        assert zf.getinfo("streams.mcap").compress_size < zf.getinfo("streams.mcap").file_size


def test_pack_is_deterministic_and_leaves_no_temp_file(bundle, tmp_path):
    a = pack_bundle(bundle, tmp_path / "a.zip").archive.read_bytes()
    b = pack_bundle(bundle, tmp_path / "b.zip").archive.read_bytes()
    assert a == b
    assert not list(tmp_path.glob("*.tmp"))


def test_roundtrip_extract_restores_every_file(bundle, tmp_path):
    z = pack_bundle(bundle).archive
    out = extract_archive(z, tmp_path / "out")
    for name, data in FILES.items():
        assert (out / name).read_bytes() == data
    assert (out / "manifest.json").read_bytes() == (bundle / "manifest.json").read_bytes()
    assert verify_archive(z) == []


def test_pack_refuses_unlisted_and_missing_files_and_leaves_nothing(bundle, tmp_path):
    (bundle / "notes.txt").write_text("stray")
    with pytest.raises(ArchiveError, match="not listed"):
        pack_bundle(bundle, tmp_path / "x.zip")
    (bundle / "notes.txt").unlink()
    (bundle / "guidance_log.json").unlink()
    with pytest.raises(ArchiveError, match="listed file missing"):
        pack_bundle(bundle, tmp_path / "x.zip")
    assert not (tmp_path / "x.zip").exists()
    assert not list(tmp_path.glob("*.tmp"))


def test_pack_ignores_tmp_files_and_rejects_non_bundles(bundle, tmp_path):
    (bundle / "streams.mcap.tmp").write_bytes(b"half written")
    assert pack_bundle(bundle, tmp_path / "ok.zip").entries == len(FILES) + 1
    with pytest.raises(ArchiveError, match="not a bundle folder"):
        pack_bundle(tmp_path / "missing")
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(ArchiveError, match="cannot read manifest.json"):
        pack_bundle(empty)


def rewrite(src: Path, dst: Path, edit) -> Path:
    """Copy a zip entry by entry through edit(name, data) -> (name, data) or None (drop it)."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
        for info in zin.infolist():
            kept = edit(info.filename, zin.read(info))
            if kept:
                zout.writestr(kept[0], kept[1])
    return dst


def test_verify_detects_tampered_missing_and_extra_entries(bundle, tmp_path):
    z = pack_bundle(bundle).archive
    flipped = rewrite(
        z, tmp_path / "t.zip", lambda n, d: (n, d[::-1] if n == "streams.mcap" else d)
    )
    assert any("sha256 mismatch: streams.mcap" in p for p in verify_archive(flipped))
    longer = rewrite(z, tmp_path / "s.zip", lambda n, d: (n, d + b"x" if n == "camera.json" else d))
    assert any("size mismatch: camera.json" in p for p in verify_archive(longer))
    dropped = rewrite(z, tmp_path / "d.zip", lambda n, d: None if n == "camera.json" else (n, d))
    assert verify_archive(dropped) == ["listed file missing: camera.json"]
    extra = rewrite(z, tmp_path / "e.zip", lambda n, d: (n, d))
    with zipfile.ZipFile(extra, "a") as zf:
        zf.writestr("debug/imu.bin", b"1")
    assert any("not listed" in p for p in verify_archive(extra))


def test_verify_detects_corruption_and_garbage(bundle, tmp_path):
    z = pack_bundle(bundle).archive
    raw = bytearray(z.read_bytes())
    raw[len(raw) // 3] ^= 0xFF  # inside the stored / deflated payload
    bad = tmp_path / "bad.zip"
    bad.write_bytes(bytes(raw))
    assert verify_archive(bad)  # reported, never raised
    with pytest.raises(ArchiveError, match="cannot read"):
        extract_archive(bad, tmp_path / "out")
    junk = tmp_path / "junk.zip"
    junk.write_bytes(b"not a zip at all")
    assert verify_archive(junk)[0].startswith("not a readable zip")
    no_manifest = rewrite(
        z, tmp_path / "nm.zip", lambda n, d: None if n == "manifest.json" else (n, d)
    )
    assert verify_archive(no_manifest) == ["manifest.json missing"]


@pytest.mark.parametrize(
    "name", ["../evil.txt", "/abs/evil.txt", "a/../../evil.txt", "a\\b.txt", "C:/x.txt"]
)
def test_extract_refuses_unsafe_names(tmp_path, name):
    z = tmp_path / "evil.zip"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr(name, b"x")
    with pytest.raises(ArchiveError, match="unsafe"):
        extract_archive(z, tmp_path / "out")
    assert not (tmp_path / "evil.txt").exists()
    assert verify_archive(z)  # reported as a problem too


def test_extract_refuses_symlinks_duplicates_and_bombs(tmp_path, monkeypatch):
    link = tmp_path / "link.zip"
    with zipfile.ZipFile(link, "w") as zf:
        zi = zipfile.ZipInfo("manifest.json")
        zi.create_system = 3
        zi.external_attr = 0o120777 << 16
        zf.writestr(zi, "/etc/passwd")
    with pytest.raises(ArchiveError, match="unsafe"):
        extract_archive(link, tmp_path / "o1")

    dup = tmp_path / "dup.zip"
    with pytest.warns(UserWarning), zipfile.ZipFile(dup, "w") as zf:
        zf.writestr("a.txt", b"1")
        zf.writestr("a.txt", b"2")
    with pytest.raises(ArchiveError, match="duplicate"):
        extract_archive(dup, tmp_path / "o2")

    big = tmp_path / "big.zip"
    with zipfile.ZipFile(big, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("zeros.bin", b"\x00" * 10_000)
    monkeypatch.setattr(archive, "MAX_UNPACKED_BYTES", 1000)
    with pytest.raises(ArchiveError, match="unpacks to more than"):
        extract_archive(big, tmp_path / "o3")
    monkeypatch.setattr(archive, "MAX_UNPACKED_BYTES", 16 * 1024**3)
    monkeypatch.setattr(archive, "MAX_ENTRIES", 0)
    with pytest.raises(ArchiveError, match="more than 0 entries"):
        extract_archive(big, tmp_path / "o4")


def test_pack_refuses_an_archive_path_inside_the_folder(bundle):
    """Reviewer finding: an archive written inside the folder would be deleted with it."""
    before = sorted(p.name for p in bundle.rglob("*"))
    for out in (
        bundle / "scan.zip",
        bundle / "keyframes" / "x.zip",
        bundle / ".." / bundle.name / "y.zip",
    ):
        with pytest.raises(ArchiveError, match="outside the bundle folder"):
            pack_bundle(bundle, out)
    assert sorted(p.name for p in bundle.rglob("*")) == before  # nothing was created in the folder
    assert is_inside(bundle / "a" / ".." / "b", bundle) and not is_inside(
        bundle.parent / "o.zip", bundle
    )


def test_extract_turns_a_file_where_a_folder_is_needed_into_an_archive_error(tmp_path):
    z = tmp_path / "clash.zip"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("a", b"file")
        zf.writestr("a/b", b"needs a folder called a")
    with pytest.raises(ArchiveError, match="cannot read"):
        extract_archive(z, tmp_path / "out")


def test_a_header_that_understates_the_size_cannot_write_more_than_it_claims(tmp_path):
    z = tmp_path / "lie.zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("big.bin", b"\x01" * 5000)
    raw = bytearray(z.read_bytes())
    central = raw.rindex(b"PK\x01\x02")
    raw[central + 24 : central + 28] = struct.pack("<I", 10)  # claimed uncompressed size
    z.write_bytes(bytes(raw))
    with pytest.raises(ArchiveError):
        extract_archive(z, tmp_path / "out")
    written = tmp_path / "out" / "big.bin"
    assert not written.exists() or written.stat().st_size <= 10
    assert verify_archive(z)  # reported as a problem too


def test_colons_and_case_collisions_are_unsafe(tmp_path):
    for name in ("keyframes/a:stream", "x/y:z/w", "C:evil"):
        z = tmp_path / "colon.zip"
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr(name, b"x")
        with pytest.raises(ArchiveError, match="unsafe"):
            extract_archive(z, tmp_path / "out")
    case = tmp_path / "case.zip"
    with zipfile.ZipFile(case, "w") as zf:
        zf.writestr("A.json", b"1")
        zf.writestr("a.json", b"2")
    with pytest.raises(ArchiveError, match="duplicate"):
        extract_archive(case, tmp_path / "out2")
    assert any("duplicate" in p for p in verify_archive(case))


def test_extract_refuses_when_the_disk_is_too_small(bundle, tmp_path, monkeypatch):
    z = pack_bundle(bundle).archive
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(archive.shutil, "disk_usage", lambda _path: usage(10_000, 9_990, 10))
    with pytest.raises(ArchiveError, match="not enough free space"):
        extract_archive(z, tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_bundle_dir_passes_folders_unpacks_zips_and_cleans_up(bundle):
    with bundle_dir(bundle) as d:
        assert d == bundle
    z = pack_bundle(bundle).archive
    assert is_archive(z) and not is_archive(bundle)
    with bundle_dir(z) as d:
        assert d.name == z.stem
        assert (d / "manifest.json").is_file() and (d / "keyframes" / "000001.jpg").is_file()
        unpacked = d
    assert not unpacked.exists()  # the temporary copy is gone
    missing = bundle.parent / "nope"
    with bundle_dir(missing) as d:
        assert d == missing  # passed through: the caller reports "not found"
    assert z.exists()
