"""`python -m recon.cli run`: Part 4 end to end (SpatialMap -> Reconstruction folder)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Annotated

import typer

app = typer.Typer(no_args_is_help=True, help="Room Scanner Part 4: 3D reconstruction.")


@app.command()
def run(
    spatial_map: Annotated[Path, typer.Option(help="<run>/spatial_map from `spatial.cli run`")],
    out: Annotated[Path, typer.Option(help="output reconstruction directory (must not exist)")],
    resolution_level: Annotated[int, typer.Option(help="OpenMVS image downscale 2^level")] = 1,
    splat: Annotated[str, typer.Option(help="auto | yes | no")] = "auto",
    splat_steps: int = 7000,
    heldout_every: Annotated[int, typer.Option(help="0 = no held-out images")] = 8,
    refine: Annotated[bool, typer.Option(help="OpenMVS RefineMesh (slow on CPU)")] = False,
    max_threads: Annotated[int, typer.Option(help="OpenMVS threads (fewer = less RAM)")] = 4,
    memory_gb: Annotated[
        float | None, typer.Option(help="cap each OpenMVS container (RAM+swap, GB)")
    ] = None,
):
    from recon.dense import DenseParams
    from recon.package import run_reconstruction

    d = DenseParams(
        resolution_level=resolution_level,
        refine=refine,
        max_threads=max_threads,
        memory_gb=memory_gb,
    )
    r = run_reconstruction(
        spatial_map, out, d, splat=splat, splat_steps=splat_steps, heldout_every=heldout_every
    )
    typer.echo(json.dumps({"reconstruction": str(r)}))


if __name__ == "__main__":
    app()
