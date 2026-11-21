"""Depth sources: sensor depth (LiDAR/Kinect/rendered) and monocular depth (Depth-Anything-V2).

MonoDepth caches predictions on disk keyed by sha256(image bytes) + model id + revision +
input size, so ablation runs reuse inference (brief §10.D.3).
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

# name -> (hf_id, licence, kind). Base/Large are non-commercial: opt-in only.
MODELS = {
    ("small", "metric"): (
        "depth-anything/Depth-Anything-V2-Metric-Indoor-Small-hf",
        "Apache-2.0",
        "metric",
    ),
    ("base", "metric"): (
        "depth-anything/Depth-Anything-V2-Metric-Indoor-Base-hf",
        "CC-BY-NC-4.0",
        "metric",
    ),
    ("large", "metric"): (
        "depth-anything/Depth-Anything-V2-Metric-Indoor-Large-hf",
        "CC-BY-NC-4.0",
        "metric",
    ),
    ("small", "relative"): (
        "depth-anything/Depth-Anything-V2-Small-hf",
        "Apache-2.0",
        "inverse_relative",
    ),
}
DEFAULT_CACHE = Path(__file__).resolve().parents[2] / "runs" / "_cache" / "mono"


@dataclass
class SensorDepth:
    """Loads uint16-mm depth PNGs; applies range limits and an optional confidence threshold."""

    min_m: float = 0.1
    max_m: float = 8.0
    conf_min: int | None = None

    def load(self, depth_png: Path, conf_png: Path | None = None) -> np.ndarray:
        d = cv2.imread(str(depth_png), cv2.IMREAD_UNCHANGED).astype(np.float32) / 1000.0
        bad = (d < self.min_m) | (d > self.max_m)
        if self.conf_min is not None and conf_png is not None and conf_png.exists():
            bad |= cv2.imread(str(conf_png), cv2.IMREAD_UNCHANGED) < self.conf_min
        d[bad] = 0.0
        return d


class MonoDepth:
    """Depth-Anything-V2 via transformers. predict() returns float32 HxW at the image size:
    metres for kind=metric, relative inverse depth for kind=inverse_relative."""

    def __init__(
        self,
        size: str = "small",
        kind: str = "metric",
        device: str = "cpu",
        input_size: int = 518,
        batch: int = 4,
        fp16: bool | None = None,
        cache_dir: Path | None = DEFAULT_CACHE,
    ):
        import torch
        from transformers import AutoImageProcessor, AutoModelForDepthEstimation

        self.hf_id, self.licence, self.kind = MODELS[(size, kind)]
        self.name = f"depth-anything-v2-{kind}-{size}"
        self.device = device
        self.fp16 = (device.startswith("cuda")) if fp16 is None else fp16
        self.batch = batch
        self.input_size = int(input_size) // 14 * 14  # ViT patch multiple
        self.proc = AutoImageProcessor.from_pretrained(self.hf_id)
        self.proc.size = {"height": self.input_size, "width": self.input_size}
        self.model = AutoModelForDepthEstimation.from_pretrained(self.hf_id).eval().to(device)
        if self.fp16:
            self.model = self.model.half()
        self.revision = getattr(self.model.config, "_commit_hash", None) or "unknown"
        self.cache_dir = cache_dir
        self._torch = torch
        self.timings_ms: list[float] = []

    def info(self) -> dict:
        return {
            "name": self.name,
            "hf_id": self.hf_id,
            "revision": self.revision,
            "kind": self.kind,
            "licence": self.licence,
        }

    def _key(self, img_path: Path) -> str:
        h = hashlib.sha256(img_path.read_bytes()).hexdigest()
        tag = json.dumps([h, self.hf_id, self.revision, self.input_size, self.fp16])
        return hashlib.sha256(tag.encode()).hexdigest()[:32]

    def predict(self, paths: list[Path]) -> list[np.ndarray]:
        import time

        torch = self._torch
        out: list[np.ndarray | None] = [None] * len(paths)
        todo = []
        for i, p in enumerate(paths):
            c = self.cache_dir / f"{self._key(p)}.npy" if self.cache_dir else None
            if c is not None and c.exists():
                out[i] = np.load(c)
            else:
                todo.append(i)
        for s in range(0, len(todo), self.batch):
            idx = todo[s : s + self.batch]
            imgs = [cv2.cvtColor(cv2.imread(str(paths[i])), cv2.COLOR_BGR2RGB) for i in idx]
            t0 = time.perf_counter()
            inp = self.proc(images=imgs, return_tensors="pt").to(self.device)
            if self.fp16:
                inp["pixel_values"] = inp["pixel_values"].half()
            with torch.inference_mode():
                res = self.model(**inp)
            pp = self.proc.post_process_depth_estimation(
                res, target_sizes=[im.shape[:2] for im in imgs]
            )
            if self.device.startswith("cuda"):
                torch.cuda.synchronize()
            self.timings_ms.append((time.perf_counter() - t0) * 1e3 / len(idx))
            for i, r in zip(idx, pp, strict=True):
                d = r["predicted_depth"].float().cpu().numpy().astype(np.float32)
                out[i] = d
                if self.cache_dir is not None:
                    self.cache_dir.mkdir(parents=True, exist_ok=True)
                    np.save(self.cache_dir / f"{self._key(paths[i])}.npy", d)
        return out  # type: ignore[return-value]
