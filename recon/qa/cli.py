"""`python -m qa.cli report`: Job Report (verdict + anchored rescan instructions)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Annotated

import typer

from formats.spatial_map import load_spatial_map
from qa.gates import build_report

app = typer.Typer(no_args_is_help=True, help="QA gate and Job Report.")


@app.callback()
def _main() -> None:
    """Keeps `report` a named subcommand."""


@app.command("report")
def report(
    spatial_map: Annotated[Path, typer.Option(help="<run>/spatial_map")],
    out: Annotated[Path, typer.Option(help="job_report.json to write")],
    reconstruction: Annotated[Path | None, typer.Option(help="<run>/reconstruction")] = None,
):
    r = build_report(load_spatial_map(spatial_map), reconstruction)
    out.write_text(r.model_dump_json(indent=1))
    fails = [g.name for g in r.gates if g.status == "fail"]
    warns = [g.name for g in r.gates if g.status == "warn"]
    typer.echo(
        json.dumps(
            {"verdict": r.verdict, "fail": fails, "warn": warns, "rescan_regions": len(r.rescan)}
        )
    )


if __name__ == "__main__":
    app()
