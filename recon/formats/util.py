"""Shared runtime helpers (hashing, tool versions, peak-RAM sampling) for Part 3 and Part 4."""

from __future__ import annotations

import hashlib
import platform
import subprocess
import threading
import time
from pathlib import Path

import psutil


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(chunk):
            h.update(b)
    return h.hexdigest()


def sha256_tree(paths: list[Path]) -> str:
    """Order-independent digest of many files (name + content hash)."""
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(p.name.encode())
        h.update(sha256_file(p).encode())
    return h.hexdigest()


def versions() -> dict[str, str]:
    out = {"python": platform.python_version()}
    for mod in ["numpy", "open3d", "pycolmap", "torch", "transformers", "cv2"]:
        try:
            out[mod] = __import__(mod).__version__
        except Exception:  # noqa: BLE001 - optional modules
            pass
    try:
        out["git_commit"] = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            cwd=Path(__file__).parent,
        ).stdout.strip()
    except OSError:
        pass
    return out


class PeakRam:
    """Samples RSS of this process + children every `dt` seconds."""

    def __init__(self, dt: float = 0.05):
        self.dt, self.peak, self._stop = dt, 0, threading.Event()
        self._proc = psutil.Process()
        self._t = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.is_set():
            try:
                rss = self._proc.memory_info().rss + sum(
                    c.memory_info().rss for c in self._proc.children(recursive=True)
                )
            except psutil.Error:
                rss = 0
            self.peak = max(self.peak, rss)
            time.sleep(self.dt)

    def __enter__(self):
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._t.join()
