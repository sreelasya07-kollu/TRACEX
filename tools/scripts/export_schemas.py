"""Export JSON Schemas of every on-disk format to spec/ (from the pydantic models).
Usage: python scripts/export_schemas.py [--check]   (--check fails if spec/ is stale)"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from formats import models as m
from formats.scanbundle import ScanBundleManifest

ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = {
    "spatial_map/manifest": m.SpatialMapManifest,
    "spatial_map/cameras": m.CamerasFile,
    "spatial_map/planes": m.PlanesFile,
    "spatial_map/room": m.RoomModel,
    "spatial_map/quality": m.SpatialQuality,
    "workspace/priors": m.Priors,
    "workspace/workspace_meta": m.WorkspaceMeta,
    "reconstruction/manifest": m.ReconstructionManifest,
    "qa/job_report": m.JobReport,
    "scanbundle/manifest": ScanBundleManifest,
    "stage_meta": m.StageMeta,
    "depth_alignment": m.DepthAlignment,
}


def render(model) -> str:
    return json.dumps(model.model_json_schema(), indent=1, sort_keys=True) + "\n"


def main(check: bool) -> int:
    stale = []
    for name, model in SCHEMAS.items():
        path = ROOT / "spec" / f"{name}.schema.json"
        text = render(model)
        if check:
            if not path.exists() or path.read_text() != text:
                stale.append(name)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    if stale:
        print("stale schemas:", ", ".join(stale), "-> run scripts/export_schemas.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main("--check" in sys.argv))
