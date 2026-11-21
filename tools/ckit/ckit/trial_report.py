"""`ckit trial-report DIR`: aggregate `*.e2e.json` reports by participant, device, room and arm.

Medians only (small n, outliers expected). Output: JSON plus markdown and HTML tables (no
plotting dependency). Thresholds and targets are human decisions; this only makes them measurable.
"""

from __future__ import annotations

import html
import json
import statistics
from pathlib import Path

GROUP_KEYS = ("participant", "device", "room", "arm")
METRICS = (
    "duration_s",
    "instructions",
    "instructions_per_min",
    "repetition_rate",
    "coverage_completeness",
    "registered_ratio",
    "reproj_mean_px",
)


def _num(v):
    return v if isinstance(v, int | float) and not isinstance(v, bool) else None


def flatten(rep: dict) -> dict:
    """One row per e2e report; missing or failed sections give None."""
    basics, guide, sfm = (rep.get(k) or {} for k in ("basics", "guidance", "sfm"))
    verdict = (rep.get("verdict") or {}).get("verdict")
    row = {k: str(rep[k]) if rep.get(k) is not None else "unknown" for k in GROUP_KEYS}
    row.update(
        bundle=rep.get("bundle"),
        verdict=verdict,
        duration_s=_num(basics.get("duration_s")),
        instructions=_num(guide.get("instructions")),
        instructions_per_min=_num(guide.get("instructions_per_min")),
        repetition_rate=_num(guide.get("repetition_rate")),
        coverage_completeness=_num(guide.get("coverage_completeness")),
        registered_ratio=_num(sfm.get("registered_ratio")),
        reproj_mean_px=_num(sfm.get("reproj_mean_px")),
    )
    return row


def _median(vals: list) -> float | None:
    xs = [v for v in vals if v is not None]
    return round(float(statistics.median(xs)), 4) if xs else None


def _summary(rows: list[dict]) -> dict:
    n = len(rows)
    passed = sum(1 for r in rows if r["verdict"] in ("PASS", "WARN"))
    out = {"n": n, "usable_rate": round(passed / n, 4) if n else None}
    out.update({f"median_{m}": _median([r[m] for r in rows]) for m in METRICS})
    return out


def load_reports(root: Path, skipped: list[str] | None = None) -> list[dict]:
    """Unreadable or malformed files are appended to `skipped` ("path: reason") and ignored."""
    out = []
    for p in sorted(root.rglob("*.e2e.json")):
        try:
            rep = json.loads(p.read_text(encoding="utf-8"))
            if not isinstance(rep, dict):
                raise ValueError("not a JSON object")
        except (OSError, UnicodeDecodeError, ValueError) as e:
            if skipped is not None:
                skipped.append(f"{p}: {type(e).__name__}: {e}")
            continue
        out.append(rep)
    return out


def aggregate(reports: list[dict]) -> dict:
    rows = [flatten(r) for r in reports]
    groups = {}
    for key in GROUP_KEYS:
        vals = sorted({r[key] for r in rows}, key=str)
        groups[key] = {v: _summary([r for r in rows if r[key] == v]) for v in vals}
    return {"overall": _summary(rows), "groups": groups, "rows": rows}


def _cols() -> list[str]:
    return ["n", "usable_rate"] + [f"median_{m}" for m in METRICS]


def _fmt(v) -> str:
    return "-" if v is None else str(v)


def to_markdown(rep: dict) -> str:
    cols = _cols()
    out = [
        "# Trial report",
        "",
        "usable = verdict PASS or WARN; medians ignore missing values.",
        "",
    ]
    sections = [("overall", {"all": rep["overall"]})] + list(rep["groups"].items())
    for name, table in sections:
        out += [f"## {name}", "", "| " + " | ".join([name] + cols) + " |"]
        out.append("|" + "---|" * (len(cols) + 1))
        out += [
            "| " + " | ".join([str(k)] + [_fmt(s[c]) for c in cols]) + " |"
            for k, s in table.items()
        ]
        out.append("")
    return "\n".join(out)


def to_html(rep: dict) -> str:
    e = html.escape
    cols = _cols()
    parts = [
        "<!doctype html><html><head><meta charset='utf-8'><title>Trial report</title>",
        "<style>body{font:14px sans-serif;margin:2em}td,th{border:1px solid #ccc;padding:2px 6px}"
        "table{border-collapse:collapse}</style></head><body><h1>Trial report</h1>",
    ]
    sections = [("overall", {"all": rep["overall"]})] + list(rep["groups"].items())
    for name, table in sections:
        parts.append(f"<h2>{e(name)}</h2><table><tr>")
        parts += [f"<th>{e(c)}</th>" for c in [name] + cols]
        parts.append("</tr>")
        for k, s in table.items():
            cells = [str(k)] + [_fmt(s[c]) for c in cols]
            parts.append("<tr>" + "".join(f"<td>{e(c)}</td>" for c in cells) + "</tr>")
        parts.append("</table>")
    parts.append("</body></html>\n")
    return "\n".join(parts)
