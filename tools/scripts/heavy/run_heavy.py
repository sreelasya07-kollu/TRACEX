"""Run inventory items by id or priority, in dependency order, with resource accounting.

  .venv/bin/python scripts/heavy/run_heavy.py --ids P3-04 P3-12        # explicit
  .venv/bin/python scripts/heavy/run_heavy.py --priority P0 P1         # by priority
  .venv/bin/python scripts/heavy/run_heavy.py --priority P0 --dry-run  # show plan only

Each item -> runs/heavy/<date>/<id>/{cmd.txt, stdout.log, meta.json}; summary.json + summary.md
in runs/heavy/<date>/. Items whose status is not_implemented or whose command contains TODO
are recorded as skipped with that reason. Dependencies are pulled in automatically
(--no-deps disables that). Failures don't stop the run; dependants of a failed item are
skipped. Peak VRAM: nvidia-smi sampling (whole GPU, used memory). Peak RSS: psutil over
the process tree. Disk: bytes added under runs/ and data/ during the item (os.walk delta).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

import psutil
import yaml

REPO = Path(__file__).resolve().parents[2]
INV = REPO / "docs" / "heavy-compute" / "inventory.yaml"


def _tree_bytes(paths) -> int:
    tot = 0
    for p in paths:
        for root, _, files in os.walk(p):
            for f in files:
                try:
                    tot += os.lstat(os.path.join(root, f)).st_size
                except OSError:
                    pass
    return tot


class Sampler(threading.Thread):
    def __init__(self, pid: int, dt: float = 0.5):
        super().__init__(daemon=True)
        self.pid, self.dt, self.stop = pid, dt, threading.Event()
        self.peak_rss = 0
        self.peak_vram_mb = None
        self.has_smi = shutil.which("nvidia-smi") is not None

    def run(self):
        while not self.stop.is_set():
            try:
                p = psutil.Process(self.pid)
                rss = p.memory_info().rss + sum(
                    c.memory_info().rss for c in p.children(recursive=True)
                )
                self.peak_rss = max(self.peak_rss, rss)
            except psutil.Error:
                pass
            if self.has_smi:
                q = subprocess.run(
                    ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                    capture_output=True,
                    text=True,
                )
                vals = [float(x) for x in q.stdout.split() if x.strip()]
                if vals:
                    self.peak_vram_mb = max(self.peak_vram_mb or 0, max(vals))
            time.sleep(self.dt)


def plan(items: dict, wanted: list[str], with_deps: bool) -> list[str]:
    order, seen = [], set()

    def visit(i):
        if i in seen:
            return
        seen.add(i)
        if with_deps:
            for d in items[i]["deps"]:
                visit(d)
        order.append(i)

    for w in wanted:
        visit(w)
    return order


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ids", nargs="*", default=[])
    ap.add_argument("--priority", nargs="*", default=[])
    ap.add_argument("--no-deps", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--out", type=Path, default=REPO / "runs" / "heavy" / f"{datetime.now():%Y%m%d-%H%M%S}"
    )
    a = ap.parse_args()
    inv = yaml.safe_load(INV.read_text())
    items = {it["id"]: it for it in inv["items"]}
    wanted = list(a.ids) + [i for i, it in items.items() if it["priority"] in a.priority]
    unknown = [w for w in wanted if w not in items]
    if unknown:
        raise SystemExit(f"unknown ids: {unknown}")
    order = plan(items, wanted, not a.no_deps)
    print("plan:", " -> ".join(order))
    if a.dry_run:
        for i in order:
            it = items[i]
            why = (
                "not_implemented"
                if it["status"] == "not_implemented"
                else "TODO command"
                if "TODO" in it["command"]
                else "run"
            )
            print(f"  {i}: {why}: {it['command']}")
        return
    a.out.mkdir(parents=True, exist_ok=True)
    results = {}
    for i in order:
        it, d = items[i], a.out / i
        d.mkdir(parents=True, exist_ok=True)
        meta = {
            "id": i,
            "name": it["name"],
            "command": it["command"],
            "started": None,
            "status": None,
            "reason": None,
        }
        failed_deps = [x for x in it["deps"] if results.get(x, {}).get("status") == "failed"]
        if it["status"] == "not_implemented":
            meta |= {"status": "skipped", "reason": "not_implemented (inventory)"}
        elif "TODO" in it["command"]:
            meta |= {"status": "skipped", "reason": "command contains TODO"}
        elif failed_deps:
            meta |= {"status": "skipped", "reason": f"dependency failed: {failed_deps}"}
        else:
            (d / "cmd.txt").write_text(it["command"] + "\n")
            disk0 = _tree_bytes([REPO / "runs", REPO / "data"])
            t0 = time.time()
            meta["started"] = datetime.now().isoformat()
            with open(d / "stdout.log", "w") as log:
                # Inventory commands call bare `python`: put the project venv first on PATH.
                env = os.environ | {"PATH": f"{REPO / '.venv' / 'bin'}:{os.environ['PATH']}"}
                proc = subprocess.Popen(
                    ["bash", "-lc", it["command"]],
                    cwd=REPO,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    env=env,
                )
                s = Sampler(proc.pid)
                s.start()
                rc = proc.wait()
                s.stop.set()
                s.join()
            meta |= {
                "status": "ok" if rc == 0 else "failed",
                "returncode": rc,
                "wall_time_s": time.time() - t0,
                "peak_rss_gb": s.peak_rss / 2**30,
                "peak_vram_gb": None if s.peak_vram_mb is None else s.peak_vram_mb / 1024,
                "disk_added_gb": (_tree_bytes([REPO / "runs", REPO / "data"]) - disk0) / 2**30,
            }
            tail = (d / "stdout.log").read_text(errors="replace")[-2000:]
            if rc != 0:
                meta["reason"] = tail[-500:]
            if any(
                k in tail for k in ("CUDA out of memory", "Killed", "MemoryError", "No space left")
            ):
                meta["resource_error"] = True
        (d / "meta.json").write_text(json.dumps(meta, indent=1))
        results[i] = meta
        print(
            f"[{i}] {meta['status']}" + (f": {meta['reason'][:120]}" if meta.get("reason") else "")
        )
    (a.out / "summary.json").write_text(json.dumps(results, indent=1))
    lines = [
        "| id | status | wall s | peak RSS GB | peak VRAM GB | disk GB | reason |",
        "|---|---|---|---|---|---|---|",
    ]
    for i, m in results.items():

        def f(k, m=m):
            v = m.get(k)
            return "" if v is None else (f"{v:.2f}" if isinstance(v, float) else str(v))

        lines.append(
            f"| {i} | {m['status']} | {f('wall_time_s')} | {f('peak_rss_gb')} | "
            f"{f('peak_vram_gb')} | {f('disk_added_gb')} | "
            f"{(m.get('reason') or '')[:80].replace('|', '/').replace(chr(10), ' ')} |"
        )
    (a.out / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"summary: {a.out / 'summary.md'}")


if __name__ == "__main__":
    main()
