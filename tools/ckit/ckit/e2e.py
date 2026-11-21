"""`ckit e2e`: one end-to-end report per scan (Phase 12 field tooling).

Combines the user-facing verdict, the guidance replay summary, pycolmap registration and
reprojection (reused from quality_report / convert.reprojection), duration and the optional
research log written next to the bundle (`<bundle>.research.json`, never part of the bundle).
Each section fails on its own: an error is recorded in the section, the report is still written.
Understanding section = end state of the derived /guide/* channels (ADR 0028). Missing-area recall
is scored against `<bundle>.truth.json` ({"missing_areas": [{x0,z0,x1,z1}]}, metres), else null.
"""

from __future__ import annotations

import html
import json
from collections.abc import Callable
from pathlib import Path

E2E_VERSION = "e2e1"
MAX_SFM_IMAGES = 80  # registration cost bound; keyframes are sampled evenly beyond this


def _section(fn: Callable[[], dict]) -> dict:
    try:
        return fn()
    except Exception as e:  # noqa: BLE001 - one failing section must not lose the report
        return {"error": f"{type(e).__name__}: {e}"}


def research_path(bundle: Path) -> Path:
    bundle = bundle.resolve()
    return bundle.parent / f"{bundle.name}.research.json"


def _manifest(bundle: Path) -> dict:
    return json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))


def _basics(bundle: Path) -> dict:
    m = _manifest(bundle)
    clock, counts, dev = m.get("clock", {}), m.get("counts", {}), m.get("device", {})
    t0, t1 = clock.get("t_start_ns"), clock.get("t_end_ns")
    return {
        "bundle_id": m.get("bundle_id"),
        "schema_version": m.get("schema_version"),
        "device": {k: dev.get(k) for k in ("platform", "model", "os", "app_version")},
        "completion": m.get("completion", {}).get("status"),
        "duration_s": round((t1 - t0) / 1e9, 3) if t0 is not None and t1 is not None else None,
        "frames": counts.get("frames"),
        "keyframes": counts.get("keyframes"),
    }


def _verdict(bundle: Path) -> dict:
    from ckit.bundle.validate import validate
    from ckit.verdict import for_report

    v = for_report(validate(bundle, "quality").to_json()["issues"])
    return {
        "verdict": v["verdict"],
        "headline": v["headline"],
        "rescan": v["rescan"],
        "codes": [i["code"] for i in v["items"]],
    }


def _guidance(bundle: Path) -> dict:
    from ckit.guide_eval import replay_bundle

    if not (bundle / "streams.mcap").is_file():
        raise FileNotFoundError("streams.mcap missing: nothing to replay")
    g = replay_bundle(bundle)
    keep = ("tier", "coverage_completeness", "instructions", "by_type", "repetition_rate")
    out = {k: g[k] for k in keep}
    dur = _basics(bundle)["duration_s"]
    out["instructions_per_min"] = round(g["instructions"] * 60.0 / dur, 3) if dur else None
    return out


def _keyframe_files(bundle: Path) -> list[str]:
    files = [f["path"] for f in _manifest(bundle).get("files", []) if f.get("role") == "keyframe"]
    if len(files) <= MAX_SFM_IMAGES:
        return files
    step = len(files) / MAX_SFM_IMAGES
    return [files[int(i * step)] for i in range(MAX_SFM_IMAGES)]


def _sfm(bundle: Path) -> dict:
    from ckit.convert.reprojection import report_bundle
    from ckit.quality_report import registration_rate

    files = _keyframe_files(bundle)
    if len(files) < 3:
        return {"error": f"only {len(files)} keyframes; need >= 3"}
    reg = registration_rate(bundle, files)
    rep = report_bundle(bundle)
    return {
        "images": reg["images"],
        "registered": reg["registered"],
        "registered_ratio": reg["registration_rate"],
        "reproj_images": rep["images"],  # all posed frames, not only the registered keyframes
        "reproj_mean_px": rep["reproj_mean_px"],
        "reproj_median_px": rep["reproj_median_px"],
        "images_with_points": rep["images_with_points"],
    }


def _research(path: Path) -> dict:
    r = json.loads(path.read_text(encoding="utf-8"))
    intents = r.get("intents") or {}
    if isinstance(intents, list):  # list of used intents -> counts
        counts: dict[str, int] = {}
        for i in intents:
            name = i if isinstance(i, str) else i.get("intent", "?")
            counts[name] = counts.get(name, 0) + 1
        intents = counts
    return {
        "file": path.name,
        "participant": r.get("participant"),
        "arm": r.get("arm"),
        "room": r.get("room"),
        "device": r.get("device"),
        "intents": intents,
        "voice_stats": r.get("voice_stats"),
        "tts_start_median_ms": r.get("tts_start_median_ms"),
    }


GAP_TOL_M = 0.5  # a gap hits a labelled missing area when their boxes come within this distance
GAP_TYPES = ["missing_wall", "unseen_corner", "unseen_ceiling", "unseen_floor", "occluded", "weak"]


def truth_path(bundle: Path) -> Path:
    bundle = bundle.resolve()
    return bundle.parent / f"{bundle.name}.truth.json"


def _box(
    x0: float, z0: float, x1: float, z1: float, tol: float
) -> tuple[float, float, float, float]:
    return (min(x0, x1) - tol, min(z0, z1) - tol, max(x0, x1) + tol, max(z0, z1) + tol)


def _hit(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> bool:
    return a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]


def score_gaps(gaps: list[dict], truth: list[dict]) -> dict:
    """Missing-area recall and false-gap rate of the final /guide/gaps (world metres).

    truth = [{"x0","z0","x1","z1"}, ...]. A labelled area is recalled when a gap box (+GAP_TOL_M)
    overlaps it; a gap is false when it overlaps no labelled area."""
    tb = [_box(t["x0"], t["z0"], t["x1"], t["z1"], 0.0) for t in truth]
    gb = [_box(g["x0"], g["z0"], g["x1"], g["z1"], GAP_TOL_M) for g in gaps]
    recalled = sum(1 for t in tb if any(_hit(g, t) for g in gb))
    false = sum(1 for g in gb if not any(_hit(g, t) for t in tb))
    return {
        "n_truth": len(tb),
        "n_gaps": len(gb),
        "recalled": recalled,
        "recall": round(recalled / len(tb), 3) if tb else None,
        "false_gaps": false,
        "false_gap_rate": round(false / len(gb), 3) if gb else None,
    }


def _understanding(bundle: Path, truth: Path | None) -> dict:
    """End state of the derived /guide/* channels (ADR 0028); with a truth file, gap scoring."""
    from ckit.bundle.streams import read_streams

    mcap = bundle / "streams.mcap"
    if not mcap.exists():
        return {"present": False}
    st = read_streams(mcap)
    labels, shell, gaps = st.get("/guide/labels"), st.get("/guide/shell"), st.get("/guide/gaps")
    if not (labels or shell or gaps):
        return {"present": False}
    last_l = labels[-1] if labels else None
    last_s = shell[-1] if shell else None
    last_g = gaps[-1] if gaps else None
    by_type: dict[str, int] = {}
    for g in last_g.gaps if last_g else []:
        name = GAP_TYPES[g.type] if 0 <= g.type < len(GAP_TYPES) else str(g.type)
        by_type[name] = by_type.get(name, 0) + 1
    out: dict = {
        "present": True,
        "records": {"labels": len(labels), "shell": len(shell), "gaps": len(gaps)},
        "objects": len(last_l.objects) if last_l else 0,
        "openings": len(last_l.openings) if last_l else 0,
        "walls": len(last_s.walls) if last_s else 0,
        "shell_closed": bool(last_s.closed) if last_s else None,
        "shell_low_confidence": bool(last_s.low_confidence) if last_s else None,
        "gaps": len(last_g.gaps) if last_g else 0,
        "gaps_by_type": by_type,
    }
    if truth is not None and truth.exists():
        areas = json.loads(truth.read_text(encoding="utf-8")).get("missing_areas", [])
        final = [
            {"x0": g.x0, "z0": g.z0, "x1": g.x1, "z1": g.z1}
            for g in (last_g.gaps if last_g else [])
        ]
        out["gap_score"] = score_gaps(final, areas)
    return out


def e2e(
    bundle: Path,
    research: Path | None = None,
    sfm: bool = True,
    guide: bool = True,
    truth: Path | None = None,
) -> dict:
    """None paths default to `<bundle>.research.json` / `<bundle>.truth.json` beside the bundle."""
    bundle = bundle.resolve()  # `ckit e2e .` must still name outputs after the bundle dir
    rpath = research if research is not None else research_path(bundle)
    res = _section(lambda: _research(rpath)) if research is not None or rpath.exists() else None
    basics = _section(lambda: _basics(bundle))
    rdev = (res or {}).get("device")
    tpath = truth if truth is not None else truth_path(bundle)
    und = _section(lambda: _understanding(bundle, tpath))
    score = und.get("gap_score") or {}
    return {
        "e2e_version": E2E_VERSION,
        "bundle": bundle.name,
        "participant": (res or {}).get("participant"),
        "arm": (res or {}).get("arm"),
        "room": (res or {}).get("room"),
        "device": rdev or (basics.get("device") or {}).get("model"),
        "basics": basics,
        "verdict": _section(lambda: _verdict(bundle)),
        "guidance": _section(lambda: _guidance(bundle)) if guide else {"skipped": True},
        "sfm": _section(lambda: _sfm(bundle)) if sfm else {"skipped": True},
        "understanding": und,
        "missing_area_recall": score.get("recall"),  # null without /guide/gaps or a truth file
        "research": res,
    }


def _rows(d: dict, prefix: str = "") -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict) and v and prefix.count(".") < 1:
            out += _rows(v, key + ".")
        else:
            out.append(
                (key, json.dumps(v, sort_keys=True) if isinstance(v, dict | list) else str(v))
            )
    return out


def to_html(rep: dict) -> str:
    """One self-contained page (stdlib only, no scripts, no external assets)."""
    e = html.escape
    v = rep.get("verdict", {})
    head = f"{e(str(v.get('verdict', '?')))}: {e(str(v.get('headline', v.get('error', ''))))}"
    parts = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        f"<title>e2e {e(rep['bundle'])}</title>",
        "<style>body{font:14px sans-serif;margin:2em}td,th{border:1px solid #ccc;padding:2px 6px;"
        "text-align:left}table{border-collapse:collapse}h2{margin-top:1.2em}</style></head><body>",
        f"<h1>Scan {e(rep['bundle'])}</h1><p><b>{head}</b></p>",
    ]
    for sec in ("basics", "verdict", "guidance", "sfm", "understanding", "research"):
        val = rep.get(sec)
        parts.append(f"<h2>{e(sec)}</h2>")
        if not isinstance(val, dict):
            parts.append(f"<p>{e(str(val))}</p>")
            continue
        parts.append("<table>")
        parts += [f"<tr><th>{e(k)}</th><td>{e(x)}</td></tr>" for k, x in _rows(val)]
        parts.append("</table>")
    parts.append(
        f"<p>missing_area_recall: {e(str(rep['missing_area_recall']))} "
        "(needs /guide/gaps and a truth file)</p>"
    )
    parts.append("</body></html>\n")
    return "\n".join(parts)


def write(rep: dict, out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    j = out_dir / f"{rep['bundle']}.e2e.json"
    h = out_dir / f"{rep['bundle']}.e2e.html"
    j.write_text(json.dumps(rep, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    h.write_text(to_html(rep), encoding="utf-8")
    return j, h
