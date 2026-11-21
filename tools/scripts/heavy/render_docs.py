"""Render docs/heavy-compute/INVENTORY.md and RESULTS_TEMPLATE.md from inventory.yaml.

.venv/bin/python scripts/heavy/render_docs.py
"""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

import yaml

DOCS = Path(__file__).resolve().parents[2] / "docs" / "heavy-compute"


def _hours(rt: str) -> tuple[float, float] | None:
    """Parse the first 'a-b h' or 'a h' / 'a min' range from a runtime string."""
    m = re.search(r"([\d.]+)\s*-\s*([\d.]+)\s*h\b", rt)
    if m:
        return float(m[1]), float(m[2])
    m = re.search(r"([\d.]+)\s*h\b", rt)
    if m:
        return float(m[1]), float(m[1])
    m = re.search(r"<\s*([\d.]+)\s*min", rt)
    if m:
        return 0.0, float(m[1]) / 60
    return None


def esc(s) -> str:
    return str(s).replace("|", "\\|").replace("\n", " ")


def main() -> None:
    inv = yaml.safe_load((DOCS / "inventory.yaml").read_text())
    items, ds = inv["items"], inv["datasets"]
    req = Counter(r for it in items for r in it["req"])
    status = Counter(it["status"] for it in items)
    prio = Counter(it["priority"] for it in items)
    gpu_lo = gpu_hi = 0.0
    unparsed = []
    for it in items:
        if "GPU" in it["req"]:
            h = _hours(it["est"]["runtime"])
            if h:
                gpu_lo += h[0]
                gpu_hi += h[1]
            else:
                unparsed.append(it["id"])
    peak_vram = max(it["est"]["vram_gb"] or 0 for it in items)
    peak_ram = max(it["est"]["ram_gb"] or 0 for it in items)
    used_ds = {d for it in items for d in it["datasets"]}
    dl = sum(ds[d]["download_gb"] or 0 for d in used_ds)
    work = max(it["est"]["disk_gb"] or 0 for it in items)

    out = [
        "# Heavy-compute inventory (generated from inventory.yaml: do not edit by hand)",
        "",
        "All resource numbers are **ESTIMATES** unless the basis says MEASURED. Calibration runs",
        "(measured locally) are listed at the end. `status: not_implemented` means the code does",
        "not exist yet. That is a missing feature, not a hardware limit.",
        "",
        "## Summary",
        "",
        "| Quantity | Value |",
        "|---|---|",
        f"| Items | {len(items)} ({', '.join(f'{k}: {v}' for k, v in sorted(status.items()))}) |",
        f"| Priority | {', '.join(f'{k}: {v}' for k, v in sorted(prio.items()))} |",
        f"| Requirement classes (items may have several) | {', '.join(f'{k}: {v}' for k, v in sorted(req.items()))} |",
        f"| GPU-hours (sum over GPU items with a parsable runtime) | ESTIMATE {gpu_lo:.0f}-{gpu_hi:.0f} h; not summable: {', '.join(unparsed) or '-'} |",
        f"| Peak VRAM (largest single item) | ESTIMATE {peak_vram} GB |",
        f"| Peak system RAM (largest single item) | ESTIMATE {peak_ram} GB |",
        f"| Dataset downloads (all used datasets, MEASURED HEAD sizes) | {dl:.1f} GB (excl. real Scan Bundles: none exist) |",
        f"| Largest single-item working disk | ESTIMATE {work} GB |",
        "",
        "## Datasets",
        "",
        "| key | name | download | licence | note |",
        "|---|---|---|---|---|",
    ]
    for k, d in ds.items():
        size = "n/a" if d["download_gb"] is None else f"{d['download_gb']} GB"
        out.append(
            f"| {k} | {esc(d['name'])} | {size} | {esc(d['license'])} | {esc(d.get('note', ''))} |"
        )
    for ph in sorted({it["phase"] for it in items}):
        out += [
            "",
            f"## Phase {ph}",
            "",
            "| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
        ]
        for it in (i for i in items if i["phase"] == ph):
            e = it["est"]
            out.append(
                " | ".join(
                    [
                        f"| {it['id']}",
                        esc(it["name"]),
                        it["status"],
                        esc(it["local_blocker"]),
                        "/".join(it["req"]),
                        f"{e['vram_gb']} / {e['ram_gb']} / {e['disk_gb']}",
                        esc(e["runtime"]),
                        esc(e["basis"]),
                        ", ".join(it["datasets"]) or "-",
                        f"`{esc(it['command'])}`",
                        esc(", ".join(it["outputs"]) or "-"),
                        esc(f"{', '.join(it['metrics']['record'])} → {it['metrics']['pass']}"),
                        it["priority"],
                        ", ".join(it["deps"]) or "-",
                    ]
                )
                + " |"
            )
    out += [
        "",
        "## Calibration runs (MEASURED locally)",
        "",
        "| id | what | values |",
        "|---|---|---|",
    ]
    for k, c in inv["calibration"].items():
        vals = {a: b for a, b in c.items() if a not in ("what", "src")}
        out.append(f"| {k} | {esc(c['what'])} | {esc(vals)} (src: {esc(c['src'])}) |")
    (DOCS / "INVENTORY.md").write_text("\n".join(out) + "\n")

    t = [
        "# Big-machine results (fill with MEASURED values only; leave blank until run)",
        "",
        "Machine: `python scripts/heavy/check_machine.py > machine.txt` (attach). Each row links its",
        "`runs/heavy/<date>/<id>/meta.json`.",
        "",
        "| id | name | status (ok/failed/skipped) | wall time | peak VRAM GB | peak RSS GB | disk GB | key metrics | pass? | meta.json |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for it in items:
        t.append(f"| {it['id']} | {esc(it['name'])} |  |  |  |  |  |  |  |  |")
    (DOCS / "RESULTS_TEMPLATE.md").write_text("\n".join(t) + "\n")
    print(f"wrote INVENTORY.md ({len(items)} items) and RESULTS_TEMPLATE.md")


if __name__ == "__main__":
    main()


# ---- MACHINE_SPEC.md (tiers evaluated with check_machine.verdict on the inventory) ----------
TIERS = [
    # name, vram, ram, cores, disk, example, $/h range (ESTIMATE, on-demand list prices vary)
    ("T0 local (this laptop)", 0, 15.2, 8, 9.7, "Intel Core Ultra 5 226V, no NVIDIA GPU", None),
    (
        "T1 minimum",
        24,
        64,
        16,
        500,
        "1x 24 GB GPU (RTX 4090 / L4 / A10G class), 64 GB RAM, 16 cores, 500 GB NVMe",
        (0.7, 1.5),
    ),
    (
        "T2 recommended",
        48,
        128,
        32,
        1000,
        "1x 48 GB GPU (L40S / RTX 6000 Ada class), 128 GB RAM, 32 cores, 1 TB NVMe",
        (1.5, 3.0),
    ),
    (
        "T3 headroom",
        80,
        256,
        64,
        2000,
        "1x 80 GB GPU (A100/H100 80 GB class), 256 GB RAM, 64 cores, 2 TB NVMe",
        (2.5, 5.0),
    ),
]


def machine_spec() -> None:
    import sys

    sys.path.insert(0, str(Path(__file__).parent))
    from check_machine import verdict

    inv = yaml.safe_load((DOCS / "inventory.yaml").read_text())
    items = inv["items"]
    gpu_h = []
    for it in items:
        if "GPU" in it["req"] and (h := _hours(it["est"]["runtime"])):
            gpu_h.append(h)
    lo, hi = sum(h[0] for h in gpu_h), sum(h[1] for h in gpu_h)
    out = [
        "# Big-machine spec (generated from inventory.yaml by scripts/heavy/render_docs.py)",
        "",
        "Derived from the inventory's per-item ESTIMATES (largest single item: "
        f"{max(i['est']['vram_gb'] or 0 for i in items)} GB VRAM, "
        f"{max(i['est']['ram_gb'] or 0 for i in items)} GB RAM, "
        f"{max(i['est']['disk_gb'] or 0 for i in items)} GB working disk; datasets 56.9 GB).",
        "Tier verdicts use the same rule as `scripts/heavy/check_machine.py`. Items that are",
        "NOT_IMPLEMENTED or have a TODO command are listed separately: no machine runs them yet.",
        "",
        "| Tier | Example | VRAM | RAM | cores | disk | runnable | blocked | cost ESTIMATE for the "
        f"{lo:.0f}-{hi:.0f} GPU-h of summable GPU items |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    detail = []
    for name, vram, ram, cores, disk, ex, price in TIERS:
        m = {"vram_gb": vram, "ram_total_gb": ram, "disk_free_gb": disk}
        v = {it["id"]: verdict(it, m) for it in items}
        run = [i for i, x in v.items() if x == "RUNNABLE"]
        blk = {i: x for i, x in v.items() if x.startswith("BLOCKED")}
        cost = (
            "n/a"
            if price is None
            else f"${lo * price[0]:.0f}-${hi * price[1]:.0f} (at ${price[0]}-{price[1]}/h, verify with provider)"
        )
        out.append(
            f"| {name} | {ex} | {vram} GB | {ram} GB | {cores} | {disk} GB | {len(run)} | {len(blk)} | {cost} |"
        )
        detail += [
            "",
            f"### {name}",
            "",
            f"- Runnable ({len(run)}): {', '.join(run) or '-'}",
            f"- Blocked ({len(blk)}): "
            + ("; ".join(f"{i}: {x[9:-1]}" for i, x in blk.items()) or "-"),
        ]
    ni = [it["id"] for it in items if it["status"] == "not_implemented"]
    todo = [
        it["id"] for it in items if it["status"] != "not_implemented" and "TODO" in it["command"]
    ]
    out += detail + [
        "",
        "### Not runnable on any tier yet",
        "",
        f"- NOT_IMPLEMENTED ({len(ni)}): {', '.join(ni)}",
        f"- Implemented/partial but command TODO ({len(todo)}): {', '.join(todo)}",
        "",
        "## Recommendation",
        "",
        "**T1 is the minimum** that runs every currently implemented GPU item (largest VRAM need among",
        "runnable items is 8 GB; 24 GB covers the splat training estimates of 16 GB once Part 4 exists).",
        "**T2 is recommended**: P3-10 stress (24 GB VRAM, 64 GB RAM), OpenMVS full-res densify (64 GB RAM)",
        "and the end-to-end runs (64 GB RAM, 100 GB disk) fit with margin, and 1 TB NVMe holds all",
        "datasets (56.9 GB) plus several full runs. All resource figures are ESTIMATES (see inventory",
        "`basis`); re-derive after the first T1/T2 run fills RESULTS_TEMPLATE.md.",
    ]
    (DOCS / "MACHINE_SPEC.md").write_text("\n".join(out) + "\n")
    print("wrote MACHINE_SPEC.md")


if __name__ == "__main__":
    machine_spec()
