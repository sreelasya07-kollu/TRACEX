"""`python -m router.cli plan`: print the stage plan for a request (JSON)."""

from __future__ import annotations

import json
from typing import Annotated

import typer

from router.recipes import Request, plan

app = typer.Typer(no_args_is_help=True, help="Requirements-to-recipe router.")


@app.callback()
def _main() -> None:
    """Keeps `plan` a named subcommand."""


@app.command("plan")
def plan_cmd(
    outputs: Annotated[list[str], typer.Option("--output", "-o", help="repeat for each output")],
    tier: Annotated[str, typer.Option(help="A (depth+poses) | B (poses) | C (RGB only)")] = "A",
    frames: int = 60,
    cuda: Annotated[bool, typer.Option(help="a CUDA GPU is available here")] = False,
    budget_min: float | None = None,
    spatial_verdict: str | None = None,
    resolution_level: int = 0,
):
    p = plan(
        Request(set(outputs), tier, frames, cuda, budget_min, spatial_verdict, resolution_level)
    )
    typer.echo(json.dumps(p.to_dict(), indent=1))


if __name__ == "__main__":
    app()
