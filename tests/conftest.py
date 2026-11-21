"""Resource-aware markers (docs/heavy-compute/INVENTORY.md). A marked test is skipped with
the requirement printed when this machine can't satisfy it, e.g.
`needs >= 16 GB VRAM, found 0 (no CUDA device)`."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import psutil
import pytest

REPO = Path(__file__).resolve().parents[1]


def pytest_addoption(parser):
    parser.addoption("--run-longrun", action="store_true", help="run tests marked longrun")


def _vram_gb() -> tuple[float, str]:
    try:
        import torch

        if torch.cuda.is_available():
            return torch.cuda.get_device_properties(
                0
            ).total_memory / 2**30, torch.cuda.get_device_name(0)
    except Exception:  # noqa: BLE001
        pass
    return 0.0, "no CUDA device"


def pytest_collection_modifyitems(config, items):
    vram, gpu_name = _vram_gb()
    ram = psutil.virtual_memory().total / 2**30
    disk = shutil.disk_usage(REPO).free / 2**30
    longrun_ok = config.getoption("--run-longrun") or os.environ.get("RUN_LONGRUN") == "1"
    for item in items:
        if (m := item.get_closest_marker("gpu")) is not None:
            need = m.kwargs.get("vram_gb", 1)
            if vram < need:
                item.add_marker(
                    pytest.mark.skip(
                        reason=f"needs >= {need} GB VRAM, found {vram:.0f} ({gpu_name})"
                    )
                )
        if (m := item.get_closest_marker("highmem")) is not None:
            need = m.kwargs.get("ram_gb", 32)
            if ram < need:
                item.add_marker(
                    pytest.mark.skip(reason=f"needs >= {need} GB RAM, found {ram:.1f} GB")
                )
        if (m := item.get_closest_marker("largedisk")) is not None:
            need = m.kwargs.get("disk_gb", 50)
            if disk < need:
                item.add_marker(
                    pytest.mark.skip(reason=f"needs >= {need} GB free disk, found {disk:.1f} GB")
                )
        if (m := item.get_closest_marker("longrun")) is not None and not longrun_ok:
            item.add_marker(
                pytest.mark.skip(
                    reason=f"long run (~{m.kwargs.get('hours', '?')} h): pass --run-longrun"
                )
            )
