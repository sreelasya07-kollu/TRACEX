"""`python -m export.cli all`: every export of a verified SpatialMap into one folder."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Annotated

import typer

from export.floorplan import measurements, write_dxf, write_svg
from export.pointcloud import write_laz
from export.usd import write_usda
from formats.spatial_map import load_spatial_map

app = typer.Typer(no_args_is_help=True, help="Exports from a SpatialMap.")


@app.callback()
def _main() -> None:
    """Keeps `all` a named subcommand when more commands are added."""


@app.command("all")
def export_all(
    spatial_map: Annotated[Path, typer.Option(help="<run>/spatial_map")],
    out: Annotated[Path, typer.Option(help="output directory")],
):
    sm = load_spatial_map(spatial_map)  # verifies hashes
    out.mkdir(parents=True, exist_ok=True)
    done, skipped = {}, {}
    for name, fn in (
        ("floorplan.svg", lambda: write_svg(sm, out / "floorplan.svg")),
        ("floorplan.dxf", lambda: write_dxf(sm, out / "floorplan.dxf")),
        ("room_shell.usda", lambda: write_usda(sm, out / "room_shell.usda")),
    ):
        try:
            fn()
            done[name] = True
        except ValueError as e:  # e.g. no floor polygon: say so, never write an empty file
            skipped[name] = str(e)
    n = write_laz(sm.fused_cloud, out / "fused_cloud.laz")
    done["fused_cloud.laz"] = n
    try:
        (out / "measurements.json").write_text(json.dumps(measurements(sm), indent=1))
        done["measurements.json"] = True
    except ValueError as e:
        skipped["measurements.json"] = str(e)
    typer.echo(json.dumps({"written": done, "skipped": skipped}))


if __name__ == "__main__":
    app()
