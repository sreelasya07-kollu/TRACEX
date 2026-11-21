"""Print this machine's resources and which inventory items it can run.

  .venv/bin/python scripts/heavy/check_machine.py [--json]
Verdicts: RUNNABLE | BLOCKED (<reason>) | NOT_IMPLEMENTED | TODO_COMMAND
Only resources are checked; datasets are checked by scripts/heavy/fetch_datasets.sh.
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import psutil
import yaml

REPO = Path(__file__).resolve().parents[2]
INV = REPO / "docs" / "heavy-compute" / "inventory.yaml"


def gpus() -> list[dict]:
    if not shutil.which("nvidia-smi"):
        return []
    q = subprocess.run(
        [
            "nvidia-smi",
            "--query-gpu=name,memory.total,driver_version",
            "--format=csv,noheader,nounits",
        ],
        capture_output=True,
        text=True,
    )
    out = []
    for ln in q.stdout.strip().splitlines():
        name, mem, drv = [x.strip() for x in ln.split(",")]
        out.append({"name": name, "vram_gb": float(mem) / 1024, "driver": drv})
    return out


def tool_versions() -> dict:
    v = {}
    for mod in ["torch", "open3d", "pycolmap", "gsplat", "transformers"]:
        try:
            m = __import__(mod)
            v[mod] = m.__version__
        except Exception:  # noqa: BLE001
            v[mod] = None
    try:
        import torch

        v["torch_cuda"] = torch.version.cuda if torch.cuda.is_available() else None
    except Exception:  # noqa: BLE001
        v["torch_cuda"] = None
    try:
        import pycolmap

        v["pycolmap_cuda"] = bool(pycolmap.has_cuda)
    except Exception:  # noqa: BLE001
        pass
    for exe in ["colmap", "nvcc", "docker"]:
        v[exe] = shutil.which(exe)
    if shutil.which("docker"):
        imgs = subprocess.run(
            ["docker", "images", "--format", "{{.Repository}}:{{.Tag}}"],
            capture_output=True,
            text=True,
        ).stdout.split()
        v["docker_images"] = [i for i in imgs if i.startswith(("room-", "roomscan-"))]
    return v


def machine() -> dict:
    vm, sw = psutil.virtual_memory(), psutil.swap_memory()
    g = gpus()
    return {
        "cpu": platform.processor() or platform.machine(),
        "cores": os.cpu_count(),
        "ram_total_gb": vm.total / 2**30,
        "ram_available_gb": vm.available / 2**30,
        "swap_gb": sw.total / 2**30,
        "gpus": g,
        "vram_gb": max((x["vram_gb"] for x in g), default=0),
        "disk_free_gb": shutil.disk_usage(REPO).free / 2**30,
        "tmp_free_gb": shutil.disk_usage("/tmp").free / 2**30,
        "os": f"{platform.system()} {platform.release()}",
        "python": sys.version.split()[0],
        "tools": tool_versions(),
    }


def verdict(it: dict, m: dict) -> str:
    if it["status"] == "not_implemented":
        return "NOT_IMPLEMENTED"
    if "TODO" in it["command"]:
        return "TODO_COMMAND"
    e, why = it["est"], []
    if "GPU" in it["req"] and m["vram_gb"] < max(e["vram_gb"] or 0, 1):
        why.append(f"needs >= {max(e['vram_gb'] or 0, 1)} GB VRAM, found {m['vram_gb']:.0f}")
    if (e["ram_gb"] or 0) > m["ram_total_gb"]:
        why.append(f"needs ~{e['ram_gb']} GB RAM, found {m['ram_total_gb']:.0f}")
    if (e["disk_gb"] or 0) > m["disk_free_gb"]:
        why.append(f"needs ~{e['disk_gb']} GB disk, found {m['disk_free_gb']:.0f} free")
    return "RUNNABLE" if not why else "BLOCKED (" + "; ".join(why) + ")"


def main():
    m = machine()
    inv = yaml.safe_load(INV.read_text())
    rows = [(it["id"], it["priority"], verdict(it, m), it["name"]) for it in inv["items"]]
    if "--json" in sys.argv:
        print(
            json.dumps(
                {
                    "machine": m,
                    "items": [
                        dict(zip(("id", "priority", "verdict", "name"), r, strict=True))
                        for r in rows
                    ],
                },
                indent=1,
            )
        )
        return
    print(
        f"CPU {m['cpu']} x{m['cores']} | RAM {m['ram_total_gb']:.1f} GB "
        f"({m['ram_available_gb']:.1f} available) | swap {m['swap_gb']:.1f} GB"
    )
    print(
        f"GPU {m['gpus'] or 'none'} | disk free {m['disk_free_gb']:.1f} GB | /tmp "
        f"{m['tmp_free_gb']:.1f} GB | {m['os']} | Python {m['python']}"
    )
    print("tools:", json.dumps(m["tools"]))
    print()
    for r in rows:
        print(f"{r[0]:7s} {r[1]}  {r[2]:<60.60s}  {r[3][:70]}")
    counts = {}
    for r in rows:
        k = r[2].split(" ")[0]
        counts[k] = counts.get(k, 0) + 1
    print("\nsummary:", counts)


if __name__ == "__main__":
    main()
