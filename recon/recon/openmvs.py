"""OpenMVS (docker image `room-openmvs:2.4.0`, CPU) stage wrappers for Part 4.

All paths handed to the tools are relative to the mounted working folder `/out`, which is the
stage output directory, so scene files stay relocatable."""

from __future__ import annotations

import os
import shutil
import subprocess
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

IMAGE = "room-openmvs:2.4.0"
BIN = "/opt/openmvs/bin/OpenMVS"


class OpenMVSError(RuntimeError):
    pass


def available() -> bool:
    if shutil.which("docker") is None:
        return False
    r = subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True)
    return r.returncode == 0


@dataclass
class ToolRun:
    tool: str
    args: list[str]
    seconds: float


def docker_cmd(
    tool: str,
    args: list[str],
    out: Path,
    images: Path | None = None,
    memory_gb: float | None = None,
    name: str | None = None,
) -> list[str]:
    """The docker invocation for one OpenMVS tool. `memory_gb` caps the container (RAM and swap
    together), so a memory-hungry step is killed inside its own container instead of putting the
    whole desktop session under memory pressure."""
    cmd = [
        "docker",
        "run",
        "--rm",
        "--name",
        name or f"roomscan-{tool}-{uuid.uuid4().hex[:8]}",
        *(["-u", f"{os.getuid()}:{os.getgid()}"] if hasattr(os, "getuid") else []),
        "-v",
        f"{out.resolve()}:/out",
    ]
    if images is not None:
        cmd += ["-v", f"{images.resolve()}:/images:ro"]
    if memory_gb is not None:
        cmd += ["--memory", f"{memory_gb:g}g", "--memory-swap", f"{memory_gb:g}g"]
    cmd += [IMAGE, f"{BIN}/{tool}", *args, "-w", "/out"]
    return cmd


def run_tool(
    tool: str,
    args: list[str],
    out: Path,
    images: Path | None = None,
    timeout: int = 7200,
    memory_gb: float | None = None,
) -> ToolRun:
    name = f"roomscan-{tool}-{uuid.uuid4().hex[:8]}"
    cmd = docker_cmd(tool, args, out, images, memory_gb, name)
    t0 = time.perf_counter()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except BaseException:  # timeout / Ctrl-C: do not leave the container running unattended
        subprocess.run(["docker", "kill", name], capture_output=True)
        raise
    dt = time.perf_counter() - t0
    log = out / "logs"
    log.mkdir(exist_ok=True)
    (log / f"{tool}.log").write_text(
        "$ " + " ".join(cmd) + "\n" + r.stdout[-200_000:] + "\n" + r.stderr[-200_000:]
    )
    if r.returncode != 0:
        raise OpenMVSError(f"{tool} failed ({r.returncode}); see {log / (tool + '.log')}")
    return ToolRun(tool, args, dt)
