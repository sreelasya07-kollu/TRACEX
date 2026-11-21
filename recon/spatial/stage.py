"""Stage bookkeeping: wall time, peak RAM (psutil sampling), versions, hashes -> meta.json."""

from __future__ import annotations

import time
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path

from formats.models import StageMeta
from formats.util import PeakRam, sha256_file, versions


class StageRecorder:
    """Collects metrics/outputs during a stage; `record_stage` writes meta.json at the end."""

    def __init__(self, stage: str, out_dir: Path, device: str, params: dict):
        self.stage, self.out_dir, self.device, self.params = stage, out_dir, device, params
        self.inputs: dict[str, str] = {}
        self.outputs: dict[str, str] = {}
        self.metrics: dict = {}
        self.notes: list[str] = []
        self.status = "ok"

    def add_input(self, path: Path, digest: str | None = None):
        self.inputs[str(path)] = digest or sha256_file(path)

    def add_output(self, path: Path):
        self.outputs[str(path.relative_to(self.out_dir.parent))] = sha256_file(path)


@contextmanager
def record_stage(stage: str, out_dir: Path, device: str = "cpu", params: dict | None = None):
    out_dir.mkdir(parents=True, exist_ok=True)
    rec = StageRecorder(stage, out_dir, device, params or {})
    started, t0 = datetime.now(UTC), time.perf_counter()
    vram = None
    try:
        import torch

        if torch.cuda.is_available():
            torch.cuda.reset_peak_memory_stats()
    except ImportError:
        torch = None
    with PeakRam() as ram:
        try:
            yield rec
        except BaseException as e:
            rec.status = "failed"
            rec.notes.append(f"{type(e).__name__}: {e}")
            raise
        finally:
            if torch is not None and torch.cuda.is_available():
                vram = torch.cuda.max_memory_allocated() / 2**20
            meta = StageMeta(
                stage=stage,
                status=rec.status,
                started=started.isoformat(),
                ended=datetime.now(UTC).isoformat(),
                wall_time_s=time.perf_counter() - t0,
                device=device,
                peak_ram_mb=ram.peak / 2**20,
                peak_vram_mb=vram,
                versions=versions(),
                params=rec.params,
                inputs=rec.inputs,
                outputs=rec.outputs,
                metrics=rec.metrics,
                notes=rec.notes,
            )
            (out_dir / "meta.json").write_text(meta.model_dump_json(indent=1))
