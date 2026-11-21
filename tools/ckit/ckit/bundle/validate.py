"""`ckit validate`: the executable definition of a valid Scan Bundle (spec/scanbundle/SPEC.md)."""

from __future__ import annotations

import bisect
import hashlib
import json
import math
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path

import jsonschema

from ckit.bundle import SPEC_DIR, SUPPORTED_MAJOR, SUPPORTED_MINOR
from ckit.bundle.archive import ArchiveError, bundle_dir, is_archive
from ckit.bundle.streams import IMU_TOPICS, REQUIRED_TOPICS, read_streams

LEVELS = ["structural", "integrity", "semantic", "quality"]
KEYFRAME_POSE_TOL_NS = 50_000_000
POSE_GAP_WARN_NS = 500_000_000
QUAT_TOL = 1e-3
MIN_KEYFRAMES = 10
REDACTION_FILE = "privacy/redaction.json"
# 2026-11-17T09:30:00Z, optional fraction, Z or +/-hh:mm offset (light check, not a calendar check)
_ISO8601_UTC = re.compile(
    r"\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])T([01]\d|2[0-3]):[0-5]\d:[0-5]\d(\.\d+)?(Z|[+-]\d{2}:\d{2})"
)


@dataclass
class Issue:
    code: str
    severity: str  # ERROR | WARN | INFO
    level: str
    message: str
    where: str = ""


@dataclass
class Report:
    bundle: str
    level: str
    issues: list[Issue] = field(default_factory=list)

    def add(self, code, severity, level, message, where="") -> None:
        self.issues.append(Issue(code, severity, level, message, where))

    @property
    def verdict(self) -> str:
        sev = {i.severity for i in self.issues}
        return "FAIL" if "ERROR" in sev else "WARN" if "WARN" in sev else "PASS"

    def codes(self, severity: str | None = None) -> set[str]:
        return {i.code for i in self.issues if severity is None or i.severity == severity}

    def to_json(self) -> dict:
        return {
            "bundle": self.bundle,
            "level": self.level,
            "verdict": self.verdict,
            "issues": [asdict(i) for i in self.issues],
        }

    def summary(self) -> str:
        n = {s: sum(1 for i in self.issues if i.severity == s) for s in ("ERROR", "WARN", "INFO")}
        lines = [
            f"{self.verdict}  {self.bundle}  (errors={n['ERROR']} warnings={n['WARN']} "
            f"info={n['INFO']}, level={self.level})"
        ]
        lines += [
            f"  {i.severity:5} {i.code}: {i.message}" + (f" [{i.where}]" if i.where else "")
            for i in self.issues
        ]
        return "\n".join(lines)


def _schema(name: str) -> dict:
    return json.loads((SPEC_DIR / "schema" / name).read_text())


def _sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _finite(*vals: float) -> bool:
    return all(math.isfinite(v) for v in vals)


GUIDE_COUNT_KEYS = {
    "guide_labels": "/guide/labels",
    "guide_shell": "/guide/shell",
    "guide_gaps": "/guide/gaps",
}


def _unit(v: float) -> bool:
    return math.isfinite(v) and 0 <= v <= 1


def _guide_outputs(st, rep: Report) -> None:
    """Schema 1.3.0 derived channels (ADR 0028): value and wall-reference sanity (C++ parity)."""
    labels, shell, gaps = st.get("/guide/labels"), st.get("/guide/shell"), st.get("/guide/gaps")
    if not (labels or shell or gaps):
        return
    bad, bad_t, orphan = 0, 0, 0

    def mark(ok: bool, t: int) -> None:
        nonlocal bad, bad_t
        if not ok:
            if not bad:
                bad_t = t
            bad += 1

    walls: set[str] = set()
    for r in shell:
        ok = math.isfinite(r.ceiling_h) and r.ceiling_h > 0 and len(r.corners) % 2 == 0
        ok = ok and all(math.isfinite(c) for c in r.corners)
        for w in r.walls:
            ok = ok and _finite(w.x0, w.z0, w.x1, w.z1)
            walls.add(w.id)
        mark(ok, r.t_ns)
    for r in labels:
        for o in r.objects:
            mark(
                _finite(o.cx, o.cy, o.cz, o.sx, o.sy, o.sz, o.yaw)
                and _unit(o.conf)
                and o.sx > 0
                and o.sy > 0
                and o.sz > 0,
                r.t_ns,
            )
        for p in r.openings:
            mark(
                _finite(p.x0, p.z0, p.x1, p.z1, p.y0, p.y1) and _unit(p.conf) and p.kind in (0, 1),
                r.t_ns,
            )
            if p.wall and p.wall not in walls:
                orphan += 1
    for r in gaps:
        for g in r.gaps:
            mark(_finite(g.x0, g.z0, g.x1, g.z1) and _unit(g.pri) and 0 <= g.type <= 5, r.t_ns)
    if bad:
        rep.add(
            "GUIDE_OUTPUT_BAD_VALUE",
            "WARN",
            "semantic",
            f"{bad} /guide/* items non-finite, out of range or malformed",
            f"/guide/* t={bad_t}",
        )
    if orphan:
        rep.add(
            "LABEL_WALL_UNKNOWN",
            "INFO",
            "semantic",
            f"{orphan} openings name a wall id that no /guide/shell record lists",
            "/guide/labels",
        )


def _minor(manifest: dict) -> int:
    """Minor of a 1.x schema_version, 0 when absent/unparsable (so old rules apply)."""
    try:
        major, minor, _ = (int(x) for x in str(manifest.get("schema_version")).split("."))
    except ValueError:
        return 0
    return minor if major == 1 else 0


def _consent_checks(rep: Report, manifest: dict, files: list[dict]) -> None:
    S = "structural"
    consent = manifest.get("consent")
    if not (
        isinstance(consent, dict)
        and all(isinstance(consent.get(k), str) and consent[k] for k in ("version", "at"))
    ):
        rep.add("CONSENT_MISSING", "ERROR", S, "manifest.consent needs non-empty version and at")
    elif not _ISO8601_UTC.fullmatch(consent["at"]):
        rep.add("CONSENT_MISSING", "ERROR", S, "manifest.consent.at is not an ISO 8601 time")
    privacy = manifest.get("privacy")
    mode = privacy.get("redaction_mode") if isinstance(privacy, dict) else None
    if mode not in ("none", "faces"):
        rep.add("REDACTION_MANIFEST_MISSING", "ERROR", S, "manifest.privacy.redaction_mode absent")
    elif mode == "faces" and not any(f["path"] == REDACTION_FILE for f in files):
        rep.add(
            "REDACTION_MANIFEST_MISSING",
            "ERROR",
            S,
            "redaction_mode faces but no file",
            REDACTION_FILE,
        )


def _redaction_entries(bundle: Path) -> tuple[list[str], int]:
    """Scan privacy/redaction.json: (basenames of status-ok entries whose keyframe is not on disk,
    number of entries whose file path escapes bundle/keyframes/).

    ADR 0021 note: the log is flushed every 10 entries, so a forced kill can leave entries whose
    JPEG never landed. The log is untrusted input: paths are resolved and never followed outside
    keyframes/ (no `..`, no absolute path), and never echoed beyond the basename."""
    try:
        doc = json.loads((bundle / REDACTION_FILE).read_text())
        entries = doc["keyframes"]
    except (OSError, ValueError, KeyError, TypeError):
        return [], 0
    root = (bundle / "keyframes").resolve()
    missing: list[str] = []
    bad = 0
    for e in entries:
        if not isinstance(e, dict) or not isinstance(e.get("file"), str):
            continue
        try:
            target = (bundle / e["file"]).resolve()
            target.relative_to(root)
        except (ValueError, OSError):
            bad += 1
            continue
        if e.get("status") == "ok" and not target.is_file():
            missing.append(target.name)
    return missing, bad


def validate(bundle: Path, level: str = "semantic") -> Report:
    """Validate a bundle folder, or a ``.zip`` archive of one (ADR 0034).

    An archive is unpacked to a temporary folder first and the report names the archive; one that
    cannot be opened safely is an ARCHIVE_UNREADABLE ERROR.
    """
    bundle = Path(bundle)
    if not is_archive(bundle):
        return _validate_folder(bundle, level)
    try:
        with bundle_dir(bundle) as folder:
            rep = _validate_folder(folder, level)
    except ArchiveError as e:
        failed = Report(str(bundle), level)
        failed.add("ARCHIVE_UNREADABLE", "ERROR", "structural", str(e)[:300])
        return failed
    rep.bundle = str(bundle)
    return rep


def _validate_folder(bundle: Path, level: str) -> Report:
    upto = LEVELS.index(level)
    rep = Report(str(bundle), level)
    S = "structural"

    # ---- structural -------------------------------------------------------------------
    mpath = bundle / "manifest.json"
    manifest = None
    if not mpath.exists():
        rep.add("MISSING_MANIFEST", "ERROR", S, "manifest.json not found")
    else:
        try:
            manifest = json.loads(mpath.read_text())
            jsonschema.validate(manifest, _schema("manifest.schema.json"))
        except (json.JSONDecodeError, jsonschema.ValidationError) as e:
            msg = e.message if isinstance(e, jsonschema.ValidationError) else str(e)
            rep.add("MANIFEST_SCHEMA", "ERROR", S, msg[:300])
            manifest = manifest if isinstance(manifest, dict) else None
    if manifest and isinstance(manifest.get("schema_version"), str):
        try:
            major, minor, _ = (int(x) for x in manifest["schema_version"].split("."))
            if major != SUPPORTED_MAJOR:
                rep.add("UNSUPPORTED_MAJOR", "ERROR", S, f"schema_version {major}.x not supported")
                return rep
            if minor > SUPPORTED_MINOR:
                rep.add("UNKNOWN_MINOR", "WARN", S, f"schema_version minor {minor} is newer")
        except ValueError:
            pass
    cpath = bundle / "camera.json"
    try:
        jsonschema.validate(json.loads(cpath.read_text()), _schema("camera.schema.json"))
    except FileNotFoundError:
        rep.add("CAMERA_SCHEMA", "ERROR", S, "camera.json not found")
    except (json.JSONDecodeError, jsonschema.ValidationError) as e:
        msg = e.message if isinstance(e, jsonschema.ValidationError) else str(e)
        rep.add("CAMERA_SCHEMA", "ERROR", S, msg[:300])
    files = manifest.get("files", []) if manifest else []
    files = [f for f in files if isinstance(f, dict) and "path" in f]
    if manifest and _minor(manifest) >= 2:  # consent/privacy are mandatory from 1.2.0 (ADR 0020)
        _consent_checks(rep, manifest, files)
    for f in files:
        if not (bundle / f["path"]).is_file():
            rep.add("MISSING_FILE", "ERROR", S, "listed file missing", f["path"])
    spath = bundle / "streams.mcap"
    streams = None
    if spath.exists():
        streams = read_streams(spath)
        if not streams.readable:
            rep.add("MCAP_UNREADABLE", "ERROR", S, "; ".join(streams.problems)[:300])
        elif not streams.finalized:
            rep.add("MCAP_TRUNCATED", "ERROR", S, "no valid MCAP footer/summary (not finalized)")
        if streams.readable:
            imu_optional = manifest is not None and _minor(manifest) >= 4  # lean bundle (ADR 0034)
            for t in REQUIRED_TOPICS:
                if imu_optional and t in IMU_TOPICS:
                    continue
                if t not in streams.topics_present:
                    rep.add("MISSING_CHANNEL", "ERROR", S, "required channel absent", t)
    elif not any(i.code == "MISSING_FILE" for i in rep.issues):
        rep.add("MISSING_FILE", "ERROR", S, "streams.mcap missing", "streams.mcap")

    # ---- integrity --------------------------------------------------------------------
    if upto >= 1 and manifest:
        listed = set()
        for f in files:
            p = bundle / f["path"]
            listed.add(f["path"])
            if not p.is_file():
                continue
            if "size" in f and p.stat().st_size != f["size"]:
                rep.add(
                    "SIZE_MISMATCH",
                    "ERROR",
                    "integrity",
                    f"size {p.stat().st_size} != {f['size']}",
                    f["path"],
                )
            if "sha256" in f and _sha256(p) != f["sha256"]:
                rep.add("SHA256_MISMATCH", "ERROR", "integrity", "sha256 differs", f["path"])
        for p in sorted(bundle.rglob("*")):
            rel = p.relative_to(bundle).as_posix()
            if p.is_file() and rel != "manifest.json" and rel not in listed:
                rep.add("UNLISTED_FILE", "WARN", "integrity", "file not in manifest", rel)
        nofile, bad_path = _redaction_entries(bundle) if REDACTION_FILE in listed else ([], 0)
        if bad_path:
            rep.add(
                "REDACTION_ENTRY_BAD_PATH",
                "WARN",
                "integrity",
                f"{bad_path} redaction entries name a file outside keyframes/ (ignored)",
                REDACTION_FILE,
            )
        if nofile:
            rep.add(
                "REDACTION_ENTRY_NO_FILE",
                "INFO",
                "integrity",
                f"{len(nofile)} redaction entries name a missing keyframe (first: {nofile[0]})",
                REDACTION_FILE,
            )

    # ---- semantic ---------------------------------------------------------------------
    if upto >= 2 and streams and streams.readable:
        _semantic(rep, bundle, manifest, streams)

    # ---- quality (stub until Phase 4 metrics exist) ------------------------------------
    if upto >= 3:
        frames = streams.get("/frame_index") if streams else []
        if not any(getattr(f, "quality", 0) for f in frames):
            rep.add("QUALITY_NOT_EVALUATED", "INFO", "quality", "no per-frame quality metrics")
    return rep


def _paused_intervals(events, t_end: int) -> list[tuple[int, int]]:
    """[start, end] of every pause recorded in /events: SESSION_PAUSED (user or governor) until the
    next SESSION_RESUMED / STOPPED / RECOVERED, or `t_end` when the stream ends while paused."""
    out: list[tuple[int, int]] = []
    start = None
    for e in sorted(events, key=lambda e: e.t_ns):
        if e.code == "SESSION_PAUSED":
            start = e.t_ns if start is None else start
        elif start is not None and e.code in (
            "SESSION_RESUMED",
            "SESSION_STOPPED",
            "SESSION_RECOVERED",
        ):
            out.append((start, e.t_ns))
            start = None
    if start is not None:
        out.append((start, max(t_end, start)))
    return out


def _overlap_ns(a: int, b: int, intervals: list[tuple[int, int]]) -> int:
    """Nanoseconds of [a, b] inside paused intervals (a paused gap is not tracking loss)."""
    return sum(max(0, min(b, e) - max(a, s)) for s, e in intervals)


def _semantic(rep: Report, bundle: Path, manifest: dict | None, st) -> None:
    M = "semantic"
    for topic, times in st.log_times.items():
        msgs = st.get(topic)
        bad = sum(1 for a, b in zip(times, times[1:], strict=False) if b <= a)
        bad += sum(1 for m, lt in zip(msgs, times, strict=True) if m.t_ns != lt)
        if bad:
            rep.add("NON_MONOTONIC_TIME", "ERROR", M, f"{bad} out-of-order/mismatched t_ns", topic)
    poses = st.get("/pose")
    zero = [
        (t, m.t_ns)
        for t, ms in (("/pose", poses), ("/intrinsics", st.get("/intrinsics")))
        for m in ms
        if m.t_ns <= 0
    ]
    if zero:
        rep.add(
            "TIME_ZERO",
            "ERROR",
            M,
            f"{len(zero)} pose/intrinsics records with t_ns <= 0 (unmapped clock)",
            f"{zero[0][0]} t={zero[0][1]}",
        )
    nan = [p.t_ns for p in poses if not _finite(p.tx, p.ty, p.tz, p.qw, p.qx, p.qy, p.qz)]
    if nan:
        rep.add("NAN_VALUE", "ERROR", M, f"{len(nan)} poses with NaN/Inf", f"/pose t={nan[0]}")
    for topic in ("/imu/gyro", "/imu/accel"):
        n = sum(1 for s in st.get(topic) if not _finite(s.x, s.y, s.z))
        if n:
            rep.add("NAN_VALUE", "ERROR", M, f"{n} samples with NaN/Inf", topic)
    nonunit = [
        p.t_ns
        for p in poses
        if _finite(p.qw, p.qx, p.qy, p.qz)
        and abs(math.sqrt(p.qw**2 + p.qx**2 + p.qy**2 + p.qz**2) - 1) > QUAT_TOL
    ]
    if nonunit:
        rep.add(
            "NON_UNIT_QUATERNION",
            "ERROR",
            M,
            f"{len(nonunit)} non-unit quaternions",
            f"/pose t={nonunit[0]}",
        )
    intr = st.get("/intrinsics")
    if any(not _finite(k.fx, k.fy, k.cx, k.cy) or k.fx <= 0 for k in intr):
        rep.add("NAN_VALUE", "ERROR", M, "invalid intrinsics", "/intrinsics")
    frames = st.get("/frame_index")
    kfs = [f for f in frames if f.keyframe]
    if kfs and (not intr or intr[0].t_ns > kfs[0].t_ns):
        rep.add("MISSING_INTRINSICS", "ERROR", M, "no intrinsics at or before first keyframe")
    pt = [p.t_ns for p in poses]
    no_pose, missing, limited = [], [], 0
    for f in kfs:
        i = bisect.bisect_left(pt, f.t_ns)
        before = pt[i - 1] if i > 0 else None
        at_or_after = pt[i] if i < len(pt) else None
        exact = at_or_after == f.t_ns
        ok = exact or (
            before is not None
            and at_or_after is not None
            and f.t_ns - before <= KEYFRAME_POSE_TOL_NS
            and at_or_after - f.t_ns <= KEYFRAME_POSE_TOL_NS
        )
        if not ok:
            no_pose.append(f.t_ns)
        if not f.file or not (bundle / f.file).is_file():
            missing.append(f.file or f"frame_id={f.frame_id}")
        if f.tracking_state not in (0, 1):
            limited += 1
    if no_pose:
        rep.add(
            "KEYFRAME_WITHOUT_POSE",
            "ERROR",
            M,
            f"{len(no_pose)} keyframes lack bracketing poses",
            f"t={no_pose[0]}",
        )
    if missing:
        rep.add(
            "KEYFRAME_FILE_MISSING",
            "ERROR",
            M,
            f"{len(missing)} keyframe files missing",
            missing[0],
        )
    if limited:
        rep.add(
            "KEYFRAME_TRACKING_LIMITED", "WARN", M, f"{limited} keyframes while tracking not NORMAL"
        )
    paused = _paused_intervals(st.get("/events"), pt[-1] if pt else 0)
    gaps = [
        g
        for a, b in zip(pt, pt[1:], strict=False)
        if (g := b - a - _overlap_ns(a, b, paused)) > POSE_GAP_WARN_NS
    ]
    if gaps:
        rep.add("POSE_GAP", "WARN", M, f"{len(gaps)} gaps > 500 ms (max {max(gaps) / 1e6:.0f} ms)")
    if not kfs:
        rep.add("NO_KEYFRAMES", "ERROR", M, "the scan has no keyframes")
    elif len(kfs) < MIN_KEYFRAMES:
        rep.add("FEW_KEYFRAMES", "WARN", M, f"only {len(kfs)} keyframes")
    if not st.get("/ar/points") and not st.get("/ar/planes"):
        rep.add("NO_AR_GEOMETRY", "INFO", M, "no /ar/points or /ar/planes")
    dets = st.get("/ar/detections")
    if dets:
        ids = {f.frame_id for f in frames}
        bad = [
            d.t_ns
            for d in dets
            for b in d.boxes
            if not _finite(b.score, b.x0, b.y0, b.x1, b.y1)
            or not (b.x1 > b.x0 and b.y1 > b.y0 and 0 <= b.score <= 1)
        ]
        unknown = [d.t_ns for d in dets if d.frame_id not in ids]
        if bad:
            rep.add(
                "DETECTION_BAD_BOX",
                "WARN",
                M,
                f"{len(bad)} boxes non-finite, empty or with score outside [0,1]",
                f"/ar/detections t={bad[0]}",
            )
        if unknown:
            rep.add(
                "DETECTION_UNKNOWN_FRAME",
                "INFO",
                M,
                f"{len(unknown)} detection records name a frame_id absent from /frame_index",
                f"/ar/detections t={unknown[0]}",
            )
    _guide_outputs(st, rep)
    if manifest and isinstance(manifest.get("counts"), dict):
        c = manifest["counts"]
        actual = {
            "poses": len(poses),
            "imu_gyro": len(st.get("/imu/gyro")),
            "imu_accel": len(st.get("/imu/accel")),
            "intrinsics": len(intr),
            "frames": len(frames),
            "keyframes": len(kfs),
            "events": len(st.get("/events")),
        }
        if "detections" in c or dets:
            actual["detections"] = len(dets)
        for key, topic in GUIDE_COUNT_KEYS.items():
            if key in c or st.get(topic):
                actual[key] = len(st.get(topic))
        diff = [f"{k}: {c.get(k)} != {v}" for k, v in actual.items() if c.get(k) != v]
        if diff:
            rep.add("COUNT_MISMATCH", "WARN", M, "; ".join(diff))
    status = (manifest or {}).get("completion", {}).get("status")
    if status in ("partial", "recovered"):
        rep.add("PARTIAL_BUNDLE", "WARN", M, f"completion.status = {status}")
