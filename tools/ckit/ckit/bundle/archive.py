"""Scan Bundle archive: one deterministic zip instead of an open folder of loose files.

Issue #10, ADR 0034. A bundle folder (manifest.json, camera.json, streams.mcap,
keyframes/*.jpg, ...) is packed into ``<folder name>.zip``: JPEGs are stored (already
compressed), everything else is deflated, entries are sorted (manifest.json first) and carry
a fixed timestamp, so the same bundle always packs the same way.

Archives are untrusted input on the processing side, so extraction refuses unsafe names
(absolute paths, ``..``, backslashes, symlinks) and absurd sizes. Readers use
:func:`bundle_dir`, which hands a folder to code that wants files and unpacks a ``.zip`` to a
temporary directory first.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import shutil
import tempfile
import zipfile
import zlib
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

MANIFEST = "manifest.json"
ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)  # fixed entry time: no wall clock in the archive
MAX_ENTRIES = 20_000
MAX_UNPACKED_BYTES = 4 * 1024**3
FREE_SPACE_MARGIN = 1.1  # unpacking needs this much free space per byte it will write
COMPRESS_LEVEL = 6
_ALREADY_COMPRESSED = frozenset({".jpg", ".jpeg", ".png", ".zip"})
_CHUNK = 1 << 20
_S_IFMT, _S_IFLNK = 0o170000, 0o120000
# What reading a damaged zip entry can raise: bad CRC / structure, corrupt deflate, truncation.
_READ_ERRORS = (zipfile.BadZipFile, zlib.error, EOFError, OSError)


class ArchiveError(ValueError):
    """The archive (or the folder to pack) is unsafe, incomplete or inconsistent."""


@dataclass(frozen=True)
class PackResult:
    archive: Path
    entries: int
    source_bytes: int
    archive_bytes: int


def is_archive(path: str | Path) -> bool:
    p = Path(path)
    return p.is_file() and p.suffix.lower() == ".zip"


def _unsafe(name: str) -> bool:
    """True for a name that could escape the extraction folder or is not a plain relative path."""
    if not name or "\\" in name or "\x00" in name or name.startswith("/"):
        return True
    parts = name.split("/")
    return any(":" in part or part in ("", ".", "..") for part in parts)


def is_inside(path: str | Path, folder: str | Path) -> bool:
    """True if ``path`` is ``folder`` or lies under it (symlinks and ``..`` resolved)."""
    p, f = Path(path).resolve(), Path(folder).resolve()
    return p == f or f in p.parents


def _has_duplicates(names: list[str]) -> bool:
    """Duplicate entry names, also ones differing only in case (they collide on Windows/macOS)."""
    folded = [n.casefold() for n in names]
    return len(folded) != len(set(folded))


def _copy_into(zf: zipfile.ZipFile, path: Path, name: str) -> tuple[int, int]:
    """Writes ``path`` as entry ``name``; returns (crc32, size) of what was read from the source."""
    crc = size = 0
    with path.open("rb") as fin, zf.open(_zip_info(name), "w") as fout:
        for chunk in iter(lambda: fin.read(_CHUNK), b""):
            fout.write(chunk)
            crc = zlib.crc32(chunk, crc)
            size += len(chunk)
    return crc, size


def _fsync(path: Path) -> None:
    with path.open("rb+") as f:
        f.flush()
        os.fsync(f.fileno())


def _sha256(stream) -> tuple[str, int]:
    h, n = hashlib.sha256(), 0
    for chunk in iter(lambda: stream.read(_CHUNK), b""):
        h.update(chunk)
        n += len(chunk)
    return h.hexdigest(), n


def _zip_info(name: str) -> zipfile.ZipInfo:
    zi = zipfile.ZipInfo(name, date_time=ZIP_EPOCH)
    zi.create_system = 3  # Unix, whatever machine packs it
    zi.external_attr = 0o100644 << 16
    stored = PurePosixPath(name).suffix.lower() in _ALREADY_COMPRESSED
    zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    return zi


def _listed_files(src: Path) -> list[str]:
    """manifest.json first, then the files it lists, sorted. Raises if the folder disagrees."""
    try:
        manifest = json.loads((src / MANIFEST).read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise ArchiveError(f"{src}: cannot read {MANIFEST}: {e}") from e
    listed = sorted(
        f["path"] for f in manifest.get("files", []) if isinstance(f, dict) and "path" in f
    )
    for name in listed:
        if _unsafe(name):
            raise ArchiveError(f"{src}: manifest lists an unsafe path {name!r}")
        if not (src / name).is_file():
            raise ArchiveError(f"{src}: listed file missing: {name}")
    on_disk = {
        p.relative_to(src).as_posix()
        for p in src.rglob("*")
        if p.is_file() and not p.name.endswith(".tmp")
    }
    extra = sorted(on_disk - set(listed) - {MANIFEST})
    if extra:
        raise ArchiveError(f"{src}: files not listed in the manifest: {', '.join(extra[:5])}")
    return [MANIFEST, *listed]


def verify_archive(path: str | Path) -> list[str]:
    """Problems found in an archive (empty list = sound).

    Checks CRCs and entry names, that every file the manifest lists is present with the listed
    size and sha256, and that nothing else is in the archive. Never raises on a damaged zip.
    """
    problems: list[str] = []
    try:
        zf = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        return [f"not a readable zip: {e}"]
    with zf:
        names = [i.filename for i in zf.infolist()]
        if _has_duplicates(names):
            problems.append("duplicate entry names")
        bad_names = [n for n in names if _unsafe(n)]
        if bad_names:
            return [*problems, f"unsafe entry names: {bad_names[:3]}"]
        if MANIFEST not in names:
            return [*problems, f"{MANIFEST} missing"]
        try:
            manifest = json.loads(zf.read(MANIFEST))
        except (json.JSONDecodeError, *_READ_ERRORS) as e:
            return [*problems, f"{MANIFEST} unreadable: {e}"]
        listed = {
            f["path"]: f for f in manifest.get("files", []) if isinstance(f, dict) and "path" in f
        }
        for name, meta in listed.items():
            if name not in names:
                problems.append(f"listed file missing: {name}")
                continue
            try:
                with zf.open(name) as fh:
                    digest, size = _sha256(fh)
            except _READ_ERRORS as e:  # bad CRC, corrupt deflate stream, truncated data
                problems.append(f"unreadable entry {name}: {e}")
                continue
            if "size" in meta and size != meta["size"]:
                problems.append(f"size mismatch: {name} ({size} != {meta['size']})")
            if "sha256" in meta and digest != meta["sha256"]:
                problems.append(f"sha256 mismatch: {name}")
        extra = sorted(set(names) - set(listed) - {MANIFEST})
        if extra:
            problems.append(f"entries not listed in the manifest: {extra[:5]}")
        try:  # entries the manifest does not list are read too: a CRC error anywhere is unsound
            bad = zf.testzip()
        except _READ_ERRORS as e:
            bad = f"({e})"
        if bad and not any(bad in p for p in problems):
            problems.append(f"CRC error in {bad}")
    return problems


def pack_bundle(
    src: str | Path, out: str | Path | None = None, *, verify: bool = True
) -> PackResult:
    """Pack the bundle folder ``src`` into one zip (default: ``<src>.zip`` beside it).

    The archive is written to a temporary file, verified (``verify``) and only then moved into
    place, so a failure never leaves a bad archive behind.
    """
    src = Path(src)
    if not src.is_dir():
        raise ArchiveError(f"{src}: not a bundle folder")
    out = Path(out) if out else src.parent / f"{src.name}.zip"
    if is_inside(out, src):  # the archive would vanish together with the folder it was packed from
        raise ArchiveError(f"{out}: the archive must be outside the bundle folder {src}")
    names = _listed_files(src)
    tmp = out.with_name(out.name + ".tmp")
    read: dict[str, tuple[int, int]] = {}
    try:
        with zipfile.ZipFile(tmp, "w", compresslevel=COMPRESS_LEVEL) as zf:
            for name in names:
                read[name] = _copy_into(zf, src / name, name)
        with zipfile.ZipFile(tmp) as zf:  # what was stored is what was read from the folder
            for name, (crc, size) in read.items():
                info = zf.getinfo(name)
                if info.CRC != crc or info.file_size != size:
                    raise ArchiveError(f"{out.name}: {name} differs from the source after packing")
        if verify:
            problems = verify_archive(tmp)
            if problems:
                raise ArchiveError(f"{out.name}: verification failed: {'; '.join(problems[:5])}")
        _fsync(tmp)  # on disk before the caller may delete the folder
        os.replace(tmp, out)
    finally:
        tmp.unlink(missing_ok=True)
    return PackResult(out, len(names), sum(size for _, size in read.values()), out.stat().st_size)


def extract_archive(path: str | Path, dest: str | Path) -> Path:
    """Unpack ``path`` into the (new or empty) folder ``dest``.

    Refuses unsafe names, symlinks, duplicate names and archives with too many entries or too
    much data, and turns a damaged entry into an :class:`ArchiveError`.
    """
    dest = Path(dest)
    try:
        zf = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise ArchiveError(f"{path}: not a readable zip: {e}") from e
    with zf:
        infos = [i for i in zf.infolist() if not i.is_dir()]
        if len(infos) > MAX_ENTRIES:
            raise ArchiveError(f"{path}: more than {MAX_ENTRIES} entries")
        if sum(i.file_size for i in infos) > MAX_UNPACKED_BYTES:
            raise ArchiveError(f"{path}: unpacks to more than {MAX_UNPACKED_BYTES} bytes")
        names = [i.filename for i in infos]
        if _has_duplicates(names):
            raise ArchiveError(f"{path}: duplicate entry names")
        probe = next(p for p in (dest, *dest.parents) if p.exists())
        free = shutil.disk_usage(probe).free
        if sum(i.file_size for i in infos) * FREE_SPACE_MARGIN > free:
            raise ArchiveError(f"{path}: not enough free space to unpack it ({free} bytes free)")
        dest.mkdir(parents=True, exist_ok=True)
        root = dest.resolve()
        for info in infos:
            if _unsafe(info.filename) or (info.external_attr >> 16) & _S_IFMT == _S_IFLNK:
                raise ArchiveError(f"{path}: unsafe entry {info.filename!r}")
            target = (root / info.filename).resolve()
            if root not in target.parents:
                raise ArchiveError(f"{path}: entry escapes the folder: {info.filename!r}")
            try:  # also a clash like entries "a" and "a/b": a file where a folder is needed
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as fin, target.open("wb") as fout:
                    shutil.copyfileobj(fin, fout, _CHUNK)
            except _READ_ERRORS as e:
                raise ArchiveError(f"{path}: cannot read {info.filename}: {e}") from e
    return dest


@contextlib.contextmanager
def bundle_dir(path: str | Path) -> Iterator[Path]:
    """A folder holding the bundle.

    ``path`` itself for a folder, a temporary unpacked copy (removed on exit) for a ``.zip``.
    Anything else is passed through so the caller reports its own "not found".
    """
    p = Path(path)
    if not is_archive(p):
        yield p
        return
    with tempfile.TemporaryDirectory(prefix="ckit-bundle-") as tmp:
        yield extract_archive(p, Path(tmp) / p.stem)
