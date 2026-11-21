"""`ckit replay-compare`: diff two replay reports with numeric tolerances.

Tolerances: --abs (default 1e-9) and --rel (default 1e-6) apply to every number; --tol KEY=ABS
sets an absolute tolerance for any leaf whose dotted path ends with KEY (e.g. path_length_m=0.01).
"""

from __future__ import annotations


def _flatten(o, prefix="") -> dict:
    out = {}
    if isinstance(o, dict):
        for k, v in o.items():
            out.update(_flatten(v, f"{prefix}.{k}" if prefix else k))
    elif isinstance(o, list):
        out[prefix + ".__len__"] = len(o)
        for i, v in enumerate(o):
            out.update(_flatten(v, f"{prefix}[{i}]"))
    else:
        out[prefix] = o
    return out


def compare(a: dict, b: dict, abs_tol=1e-9, rel_tol=1e-6, key_tol: dict | None = None) -> list[str]:
    key_tol = key_tol or {}
    fa, fb = _flatten(a), _flatten(b)
    diffs = []
    for k in sorted(set(fa) | set(fb)):
        if k not in fa or k not in fb:
            diffs.append(f"{k}: {'missing in A' if k not in fa else 'missing in B'}")
            continue
        va, vb = fa[k], fb[k]
        num = (int, float)
        if isinstance(va, num) and isinstance(vb, num) and not isinstance(va, bool):
            tol = next((t for s, t in key_tol.items() if k.endswith(s)), None)
            lim = tol if tol is not None else max(abs_tol, rel_tol * max(abs(va), abs(vb)))
            if abs(va - vb) > lim:
                diffs.append(f"{k}: {va} != {vb} (|d|={abs(va - vb):.3g} > {lim:.3g})")
        elif va != vb:
            diffs.append(f"{k}: {va!r} != {vb!r}")
    return diffs
