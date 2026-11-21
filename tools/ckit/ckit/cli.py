"""`ckit` command-line entry point (stub)."""

from pathlib import Path
from typing import Annotated

import typer

from ckit import __version__

app = typer.Typer(help="CaptureKit Scan Bundle tools.", no_args_is_help=True)


def _version_callback(value: bool) -> None:
    if value:
        typer.echo(f"ckit {__version__}")
        raise typer.Exit()


@app.callback()
def main(
    version: Annotated[
        bool,
        typer.Option("--version", callback=_version_callback, is_eager=True, help="Show version."),
    ] = False,
) -> None:
    """CaptureKit Scan Bundle tools."""


def _dump(obj) -> None:
    import json

    typer.echo(json.dumps(obj, indent=2, sort_keys=True))


@app.command("inspect-raw")
def inspect_raw_cmd(
    session: Path,
    plot: Annotated[Path | None, typer.Option(help="Write a top-down trajectory PNG.")] = None,
    clock: Annotated[bool, typer.Option(help="Print clock-check offsets only.")] = False,
) -> None:
    """Summarize a raw recorder session (rates, drops, tracking) and plot the trajectory."""
    from ckit.inspect_raw import clock_offsets, summarize
    from ckit.inspect_raw import plot as do_plot
    from ckit.raw import RawSession

    s = RawSession.load(session)
    if clock:
        _dump(clock_offsets(s))
        return
    _dump(summarize(s))
    if plot:
        do_plot(s, plot)


@app.command("synth")
def synth_cmd(
    scenario: str,
    out: Path,
    seed: int = 0,
    duration: Annotated[float | None, typer.Option(help="Cap duration in seconds.")] = None,
    frame_hz: float = 5.0,
) -> None:
    """Render a synthetic golden-scenario raw session (G1..G8), see ADR 0002."""
    from ckit.synth.scenarios import generate

    generate(scenario, out, seed=seed, duration=duration, frame_hz=frame_hz)
    typer.echo(str(out))


@app.command("import-raw")
def import_raw_cmd(
    raw: Path,
    out: Path,
    select_keyframes: Annotated[
        bool, typer.Option(help="Keep only frames the core keyframe selector picks (Phase 4).")
    ] = False,
    config: Annotated[Path | None, typer.Option(help="Flat JSON quality config overrides.")] = None,
) -> None:
    """Convert a raw session into a Scan Bundle (all frames kept unless --select-keyframes)."""
    import json

    from ckit.bundle.import_raw import import_raw, import_raw_selected

    if select_keyframes:
        cfg = json.loads(config.read_text()) if config else None
        _dump(import_raw_selected(raw, out, cfg))
    else:
        _dump(import_raw(raw, out))


@app.command("pack")
def pack_cmd(
    bundle: Path,
    out: Annotated[
        Path | None, typer.Option(help="Archive path (default: <bundle folder>.zip beside it).")
    ] = None,
    delete_source: Annotated[
        bool, typer.Option(help="Delete the folder once the archive is verified and validates.")
    ] = False,
) -> None:
    """Pack a Scan Bundle folder into ONE verified .zip (ADR 0034). Exit code 1 on any problem."""
    import shutil

    from ckit.bundle.archive import ArchiveError, is_inside, pack_bundle
    from ckit.bundle.validate import validate

    try:
        res = pack_bundle(bundle, out)
    except ArchiveError as e:
        typer.echo(str(e), err=True)
        raise typer.Exit(1) from e
    rep = validate(res.archive)
    typer.echo(
        f"{res.archive}  {res.entries} files  {res.source_bytes} -> {res.archive_bytes} bytes"
        f"  validate={rep.verdict}"
    )
    if rep.verdict == "FAIL":  # the folder is kept: never delete the only copy of a scan that fails
        typer.echo(rep.summary(), err=True)
        raise typer.Exit(1)
    if delete_source:
        # Last look before the only other copy goes: the archive exists, outside the folder.
        if not res.archive.is_file() or is_inside(res.archive, bundle):
            typer.echo(
                f"{res.archive}: not deleting {bundle}: no separate archive on disk", err=True
            )
            raise typer.Exit(1)
        shutil.rmtree(bundle)
        typer.echo(f"deleted {bundle}")


@app.command("validate")
def validate_cmd(
    bundle: Path,
    level: Annotated[str, typer.Option(help="structural|integrity|semantic|quality")] = "semantic",
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
) -> None:
    """Validate a Scan Bundle folder or .zip archive. Exit code 1 on any ERROR."""
    import json

    from ckit.bundle.validate import validate

    rep = validate(bundle, level)
    if json_out:
        json_out.write_text(json.dumps(rep.to_json(), indent=2, sort_keys=True) + "\n")
    typer.echo(rep.summary())
    raise typer.Exit(1 if rep.verdict == "FAIL" else 0)


@app.command("export-colmap")
def export_colmap_cmd(
    bundle: Path,
    out: Path,
    max_gap_ms: Annotated[float, typer.Option(help="Max pose bracket for interpolation.")] = 100.0,
) -> None:
    """Write a COLMAP text model (cameras, images as cam_from_world wxyz, empty points)."""
    from ckit.convert.colmap import export_colmap

    _dump(export_colmap(bundle, out, int(max_gap_ms * 1e6)))


@app.command("reprojection-report")
def reprojection_report_cmd(
    bundles: list[Path],
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
) -> None:
    """Triangulate each bundle with its own poses (pycolmap) and report reprojection error."""
    import json

    from ckit.convert.reprojection import report, table

    rep = report(bundles)
    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(rep, indent=2, sort_keys=True) + "\n")
    typer.echo(table(rep))
    typer.echo(json.dumps(rep["summary"], sort_keys=True))


@app.command("recon-bench")
def recon_bench_cmd(
    scan: Path,
    baseline: Annotated[
        Path | None, typer.Option(help="Second bundle (folder or .zip) to compare side by side.")
    ] = None,
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
) -> None:
    """Offline reconstruction report (single-threaded, seeded) of a bundle folder or .zip archive;
    `--baseline` adds delta columns."""
    import contextlib
    import json

    from ckit.bundle.archive import bundle_dir
    from ckit.recon_bench import delta, recon_bench, stable, table

    try:
        with (
            contextlib.ExitStack() as unpacked
        ):  # a .zip is unpacked to a temp folder, removed after
            rep = recon_bench(unpacked.enter_context(bundle_dir(scan)))
            base = recon_bench(unpacked.enter_context(bundle_dir(baseline))) if baseline else None
    except ValueError as e:  # includes ArchiveError
        typer.echo(str(e), err=True)
        raise typer.Exit(1) from e
    if json_out:
        # `seconds` is wall-clock and non-deterministic: kept out of the JSON.
        out = stable(rep)
        if base is not None:
            out["baseline"] = stable(base)
            out["delta"] = delta(rep, base)
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n")
    typer.echo(table(rep, base))


@app.command("replay")
def replay_cmd(
    bundle: Path,
    sink: Annotated[list[str] | None, typer.Option(help="Core sink name (repeatable).")] = None,
    out: Annotated[Path | None, typer.Option(help="Write report JSON here (else stdout).")] = None,
    fast: Annotated[
        bool, typer.Option("--fast/--realtime", help="Fast (default) or realtime.")
    ] = True,
    speed: Annotated[float, typer.Option(help="Realtime speed factor.")] = 1.0,
    seed: int = 0,
    drop_frames: Annotated[float, typer.Option(help="Drop probability per frame.")] = 0.0,
    jitter_ms: Annotated[float, typer.Option(help="+- jitter on frame/IMU t_ns.")] = 0.0,
    inject_tracking_loss: Annotated[
        list[str] | None, typer.Option(help="T0:T1 seconds from start (repeatable).")
    ] = None,
    inject_thermal: Annotated[
        list[str] | None, typer.Option(help="T:STATE nominal|fair|serious|critical (repeatable).")
    ] = None,
    config: Annotated[
        Path | None, typer.Option(help="JSON {sink: {key: value}} overrides.")
    ] = None,
) -> None:
    """Replay a bundle (folder or .zip) through core sinks; prints a deterministic JSON report."""
    import json

    from ckit.bundle.archive import ArchiveError, bundle_dir
    from ckit.replay.driver import Faults, dumps, replay

    f = Faults(
        drop_frames=drop_frames,
        jitter_ms=jitter_ms,
        tracking_loss=[Faults.parse_window(w) for w in inject_tracking_loss or []],
        thermal=[Faults.parse_thermal(t) for t in inject_thermal or []],
        seed=seed,
    )
    cfg = json.loads(config.read_text()) if config else None
    try:
        with bundle_dir(bundle) as folder:
            text = dumps(replay(folder, sink, f, realtime=not fast, speed=speed, sink_config=cfg))
    except ArchiveError as e:
        typer.echo(str(e), err=True)
        raise typer.Exit(1) from e
    if out:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
    else:
        typer.echo(text, nl=False)


@app.command("replay-compare")
def replay_compare_cmd(
    a: Path,
    b: Path,
    abs_tol: Annotated[float, typer.Option("--abs")] = 1e-9,
    rel_tol: Annotated[float, typer.Option("--rel")] = 1e-6,
    tol: Annotated[list[str] | None, typer.Option(help="KEY=ABS per-key tolerance.")] = None,
) -> None:
    """Compare two replay reports. Exit 1 if they differ beyond tolerance."""
    import json

    from ckit.replay.compare import compare

    kt = {k: float(v) for k, v in (t.split("=", 1) for t in tol or [])}
    diffs = compare(json.loads(a.read_text()), json.loads(b.read_text()), abs_tol, rel_tol, kt)
    for d in diffs:
        typer.echo(d)
    typer.echo(f"{len(diffs)} difference(s)")
    raise typer.Exit(1 if diffs else 0)


@app.command("replay-plot")
def replay_plot_cmd(bundle: Path, out: Path) -> None:
    """Top-down trajectory PNG coloured by tracking state."""
    from ckit.replay.plot import plot

    typer.echo(str(plot(bundle, out)))


@app.command("quality-report")
def quality_report_cmd(
    bundles: list[Path],
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
    no_sfm: Annotated[bool, typer.Option("--no-sfm", help="Skip pycolmap registration.")] = False,
    uniform: Annotated[
        bool, typer.Option(help="Also register the same count picked at a fixed interval.")
    ] = False,
    config: Annotated[Path | None, typer.Option(help="Flat JSON quality config overrides.")] = None,
) -> None:
    """Keyframe density, rejection histogram, pycolmap registration rate of selected keyframes."""
    import json

    from ckit.quality_report import quality_report

    cfg = json.loads(config.read_text()) if config else None
    rows = [quality_report(b, cfg, register=not no_sfm, uniform=uniform) for b in bundles]
    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps({"bundles": rows}, indent=2, sort_keys=True) + "\n")
    for r in rows:
        sfm = r.get("sfm", {})
        typer.echo(
            f"{r['bundle']:<16} frames={r['frames']:>4} kf={r['keyframes']:>4} "
            f"kf/m={r['keyframes_per_m']:<7} rejected={r['hard_rejected']:>4} "
            f"reg={sfm.get('registration_rate', '-')} "
            f"reg_uniform={r.get('sfm_uniform_same_count', {}).get('registration_rate', '-')} "
            f"hard={r['hard_reject_histogram']}"
        )


@app.command("simulate-capture")
def simulate_capture_cmd(
    source: Path,
    out: Path,
    threaded: Annotated[bool, typer.Option(help="Use the core worker thread.")] = False,
    kill_after: Annotated[
        int | None, typer.Option(help="SIGKILL this process after N input events (crash test).")
    ] = None,
    config: Annotated[Path | None, typer.Option(help="Flat JSON session config.")] = None,
) -> None:
    """Replay a bundle through the real C API like a phone would; --kill-after simulates a crash."""
    import json

    from ckit.simulate import simulate

    cfg = json.loads(config.read_text()) if config else None
    _dump(simulate(source, out, cfg, threaded, kill_after))


@app.command("recover")
def recover_cmd(bundle: Path) -> None:
    """Turn an interrupted session directory into a valid partial bundle (ck_recover)."""
    from ckit.capi import recover

    st, rep = recover(bundle)
    _dump(rep)
    raise typer.Exit(0 if st == 0 else 1)


@app.command("verdict")
def verdict_cmd(bundle: Path, level: str = "semantic") -> None:
    """Validate a bundle and print the user-facing verdict (plain-language messages and actions)."""
    from ckit.bundle.validate import validate
    from ckit.verdict import for_report

    rep = validate(bundle, level)
    _dump(for_report(rep.to_json()["issues"]))


@app.command("verdict-docs")
def verdict_docs_cmd(out: Path | None = None) -> None:
    """Write docs/verdict-messages.md from the verdict table."""
    from ckit.verdict import to_markdown

    text = to_markdown()
    if out:
        out.write_text(text)
    else:
        typer.echo(text, nl=False)


@app.command("verdict-export")
def verdict_export_cmd(out: Path | None = None) -> None:
    """Write the verdict table as JSON (sample-app assets/verdict_messages.json)."""
    from ckit.verdict import to_json

    text = to_json()
    if out:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
    else:
        typer.echo(text, nl=False)


@app.command("upload")
def upload_cmd(
    bundle: Path,
    server: Annotated[str, typer.Option(help="tus endpoint")] = "http://localhost:1080/files/",
    verifier: Annotated[
        str, typer.Option(help="verifier base URL ('' = do not wait)")
    ] = "http://localhost:8081",
    delete_after_verify: Annotated[
        bool, typer.Option(help="Delete local files after COMPLETE (needs --consent).")
    ] = False,
    consent: Annotated[
        bool, typer.Option(help="User consented to upload and local deletion.")
    ] = False,
) -> None:
    """Upload a bundle via tus (manifest last), resume after kill, wait for the server verdict."""
    from ckit.upload import TusUploader
    from ckit.upload import delete_after_verify as retention

    if not consent:
        typer.echo("refusing to upload without --consent (consent flag is recorded by the app)")
        raise typer.Exit(2)
    res = TusUploader(server, verifier or None).upload_bundle(bundle)
    deleted = retention(bundle, res, delete_after_verify) if delete_after_verify else False
    _dump({"status": res.status, "reason": res.reason, "repaired": res.repaired,
           "retries": res.retries, "deleted_local": deleted})  # fmt: skip
    raise typer.Exit(0 if res.status in ("COMPLETE", "UPLOADED") else 1)


@app.command("guide-eval")
def guide_eval_cmd(
    bundles: Annotated[
        list[Path] | None, typer.Argument(help="Replay these bundles instead.")
    ] = None,
    rooms: Annotated[
        str, typer.Option(help="comma list: box,L,furniture,furnished4,furnishedL")
    ] = "box,L,furniture",
    policies: Annotated[
        str, typer.Option(help="comma list: planner,scripted,wander")
    ] = "planner,scripted,wander",
    seeds: int = 3,
    max_s: Annotated[float, typer.Option(help="Simulated seconds per run.")] = 450.0,
    params: Annotated[str, typer.Option(help="Flat JSON GuideParams overrides.")] = "{}",
    compliance: float = 0.85,
    workers: int = 6,
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
) -> None:
    """Does guidance help? Simulated walkers (planner/scripted/wander) or guidance on bundles."""
    import json

    from ckit.guide_eval import replay_bundle, simulate_matrix, table

    p = json.loads(params)
    if bundles:
        rows = [replay_bundle(b, p) for b in bundles]
        rep = {"bundles": rows}
        for r in rows:
            typer.echo(
                f"{r['bundle']:<16} tier={r['tier']} coverage={r['coverage_completeness']} "
                f"instructions={r['instructions']} repetition={r['repetition_rate']} "
                f"demand/min={r['instruction_demand_per_min']} unsafe={r['unsafe_targets']}"
            )
    else:
        rep = simulate_matrix(
            tuple(rooms.split(",")),
            tuple(policies.split(",")),
            seeds,
            max_s,
            p,
            compliance,
            workers=workers,
        )
        typer.echo(table(rep))
    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(rep, indent=2, sort_keys=True) + "\n")


@app.command("e2e")
def e2e_cmd(
    bundle: Path,
    research: Annotated[
        Path | None, typer.Option(help="Research log (default: <bundle>.research.json beside it).")
    ] = None,
    out: Annotated[
        Path | None, typer.Option(help="Output dir for <bundle>.e2e.json/.html (default: beside).")
    ] = None,
    truth: Annotated[
        Path | None,
        typer.Option(help="Labelled missing areas (default: <bundle>.truth.json beside it)."),
    ] = None,
    no_sfm: Annotated[bool, typer.Option("--no-sfm", help="Skip pycolmap.")] = False,
    no_guide: Annotated[bool, typer.Option("--no-guide", help="Skip guidance replay.")] = False,
) -> None:
    """End-to-end scan report: verdict, guidance, pycolmap registration/reprojection, research."""
    from ckit.e2e import e2e, write

    rep = e2e(bundle, research, sfm=not no_sfm, guide=not no_guide, truth=truth)
    j, h = write(rep, out or bundle.parent)
    v, s, g = rep["verdict"], rep["sfm"], rep["guidance"]
    typer.echo(
        f"{rep['bundle']} verdict={v.get('verdict', v.get('error'))} "
        f"duration_s={rep['basics'].get('duration_s')} instructions={g.get('instructions')} "
        f"registered_ratio={s.get('registered_ratio')} reproj_mean_px={s.get('reproj_mean_px')}"
    )
    typer.echo(f"{j}\n{h}")


@app.command("trial-report")
def trial_report_cmd(
    root: Path,
    json_out: Annotated[Path | None, typer.Option("--json", help="Write JSON report here.")] = None,
    html_out: Annotated[Path | None, typer.Option("--html", help="Write HTML tables here.")] = None,
) -> None:
    """Aggregate *.e2e.json under ROOT by participant/device/room/arm (medians, tables)."""
    import json

    from ckit.trial_report import aggregate, load_reports, to_html, to_markdown

    skipped: list[str] = []
    reports = load_reports(root, skipped)
    for s in skipped:
        typer.echo(f"skipped {s}", err=True)
    if not reports:
        typer.echo(f"no *.e2e.json under {root}")
        raise typer.Exit(1)
    rep = aggregate(reports)
    for p, text in ((json_out, json.dumps(rep, indent=2, sort_keys=True) + "\n"),
                    (html_out, to_html(rep) if html_out else "")):  # fmt: skip
        if p:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
    typer.echo(to_markdown(rep), nl=False)
