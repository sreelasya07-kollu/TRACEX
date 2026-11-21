"""Reader for Phase 0 raw sessions (docs/program-spec/recording-protocol.md)."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


def _jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path) as f:
        return [json.loads(ln) for ln in f if ln.strip()]


@dataclass
class RawSession:
    root: Path
    device: dict
    poses: list[dict]
    imu: list[dict]
    intrinsics: list[dict]
    frames: list[dict]
    planes: list[dict] = field(default_factory=list)
    points: list[dict] = field(default_factory=list)
    clockcheck: list[dict] = field(default_factory=list)

    @classmethod
    def load(cls, root: Path) -> RawSession:
        root = Path(root)
        frames = _jsonl(root / "frames.jsonl")
        if not frames and (root / "frames").is_dir():
            # Recorders may name files <index>_<t_ns>.jpg without an index file.
            for i, p in enumerate(sorted((root / "frames").glob("*.jpg"))):
                t = int(p.stem.split("_")[-1])
                frames.append({"i": i, "t_ns": t, "file": f"frames/{p.name}", "exposure_ns": 0})
        return cls(
            root=root,
            device=json.loads((root / "device.json").read_text()),
            poses=_jsonl(root / "poses.jsonl"),
            imu=_jsonl(root / "imu.jsonl"),
            intrinsics=_jsonl(root / "intrinsics.jsonl"),
            frames=frames,
            planes=_jsonl(root / "planes.jsonl"),
            points=_jsonl(root / "points.jsonl"),
            clockcheck=_jsonl(root / "clockcheck.jsonl"),
        )

    def imu_times(self, kind: str) -> np.ndarray:
        return np.array([r["t_ns"] for r in self.imu if r["type"] == kind], dtype=np.int64)


def rate_stats(t_ns: np.ndarray, nominal_hz: float | None = None) -> dict:
    """Rate, gap statistics and dropped-sample estimate for a timestamp series."""
    if len(t_ns) < 2:
        return {
            "count": int(len(t_ns)),
            "hz": 0.0,
            "dropped": 0,
            "max_gap_ms": 0.0,
            "non_monotonic": 0,
        }
    d = np.diff(t_ns)
    med = float(np.median(d))
    hz = 1e9 / med if med > 0 else 0.0
    period = 1e9 / nominal_hz if nominal_hz else med
    dropped = int(np.sum(np.maximum(0, np.round(d / period) - 1))) if period > 0 else 0
    return {
        "count": int(len(t_ns)),
        "hz": round(hz, 2),
        "dropped": dropped,
        "max_gap_ms": round(float(d.max()) / 1e6, 3),
        "non_monotonic": int(np.sum(d <= 0)),
    }
