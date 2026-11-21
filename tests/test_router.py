"""Recipe router: table-driven rules (capabilities, refusals, no silent accuracy loss)."""

from __future__ import annotations

import json

import pytest

from router.recipes import Request, plan


def names(p):
    return [s.name for s in p.stages]


@pytest.mark.light
def test_standard_recipe_tier_a():
    p = plan(Request({"mesh"}, "A", n_frames=60))
    assert p.recipe == "standard" and names(p) == [
        "spatial_run",
        "qa_spatial",
        "recon_mesh",
        "qa_final",
    ]
    assert "--depth-scale" in p.stages[0].command and "--splat no" in p.stages[2].command
    assert not p.refused and not p.over_budget
    # QA runs before the expensive stage
    assert names(p).index("qa_spatial") < names(p).index("recon_mesh")


@pytest.mark.light
def test_splat_without_cuda_goes_to_gpu_worker_not_local():
    p = plan(Request({"splat"}, "A", has_cuda=False))
    st = next(s for s in p.stages if s.name == "recon_splat")
    assert st.where == "gpu_worker" and any("CUDA" in w for w in p.warnings)
    assert p.est_minutes_local < st.est_minutes + 5  # splat time is not counted as local
    assert (
        next(
            s
            for s in plan(Request({"splat"}, "A", has_cuda=True)).stages
            if s.name == "recon_splat"
        ).where
        == "local"
    )


@pytest.mark.light
def test_rgb_only_refuses_semantic_outputs_and_warns_about_scale():
    p = plan(Request({"floorplan", "mesh"}, "C"))
    assert "floorplan" in p.refused and "gravity" in p.refused["floorplan"]
    assert "recon_mesh" in names(p) and any("NOT metric" in w for w in p.warnings)
    assert "--prior none" in p.stages[0].command and "--depth-scale" not in p.stages[0].command
    assert plan(Request({"semantic"}, "C")).recipe == "refused"


@pytest.mark.light
def test_needs_rescan_blocks_expensive_outputs_but_not_preview():
    p = plan(Request({"mesh", "splat", "preview"}, "A", spatial_verdict="needs_rescan"))
    assert set(p.refused) == {"mesh", "splat"} and p.recipe == "preview"
    assert all("rescan" in r for r in p.refused.values())
    assert plan(Request({"mesh"}, "A", spatial_verdict="needs_rescan")).stages == []


@pytest.mark.light
def test_budget_never_changes_accuracy_settings():
    base = plan(Request({"mesh"}, "A", n_frames=100, resolution_level=0))
    tight = plan(Request({"mesh"}, "A", n_frames=100, resolution_level=0, time_budget_min=5))
    assert tight.over_budget and any("NOT reduced" in w for w in tight.warnings)
    assert [s.command for s in tight.stages] == [s.command for s in base.stages]  # identical plan
    assert not plan(Request({"mesh"}, "A", n_frames=10, time_budget_min=60)).over_budget


@pytest.mark.light
def test_tier_b_uses_mvs_with_fallback_note_and_costs_more():
    a, b = (
        plan(Request({"floorplan"}, "A", n_frames=100)),
        plan(Request({"floorplan"}, "B", n_frames=100)),
    )
    assert b.est_minutes_local > a.est_minutes_local and b.fallbacks
    assert "--depth-scale" in a.stages[0].command and "--depth-scale" not in b.stages[0].command


@pytest.mark.light
def test_preview_limits_frames_and_validation_errors():
    p = plan(Request({"preview"}, "A", n_frames=500))
    assert "--max-frames 30" in p.stages[0].command and p.recipe == "preview"
    with pytest.raises(ValueError):
        plan(Request({"hologram"}, "A"))
    with pytest.raises(ValueError):
        plan(Request({"mesh"}, "Z"))


@pytest.mark.light
def test_cli_prints_json_plan():
    from typer.testing import CliRunner

    from router.cli import app

    r = CliRunner().invoke(app, ["plan", "-o", "mesh", "-o", "floorplan", "--tier", "A"])
    assert r.exit_code == 0, r.output
    d = json.loads(r.output)
    assert d["recipe"] == "standard" and d["stages"][0]["name"] == "spatial_run"
