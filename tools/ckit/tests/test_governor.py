"""Phase 5: replay with injected thermal events changes the governor policy as specified."""

from pathlib import Path

import pytest

from ckit.bundle.import_raw import import_raw
from ckit.replay.driver import Faults, dumps, replay


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("g") / "bundle"
    import_raw(Path(__file__).parent / "fixtures" / "raw_small", out)
    return out


def _gov(bundle, thermal, cfg=None):
    rep = replay(bundle, ["governor"], Faults(thermal=thermal), sink_config={"governor": cfg or {}})
    return rep["sinks"]["governor"]


def test_no_events_stays_nominal(bundle):
    g = _gov(bundle, [])
    assert g["transitions"] == [] and g["final_policy"]["level"] == "NOMINAL"


def test_serious_lowers_resolution_and_blocks_upload(bundle):
    g = _gov(bundle, [(0.2, "serious")])
    assert g["transitions"][0]["to"] == "LOWER_RESOLUTION"
    assert g["transitions"][0]["reason"] == "thermal=2"
    p = g["final_policy"]
    assert p["capture_resolution_tier"] == 1
    assert not p["upload_allowed"] and not p["overlay_enabled"]


def test_critical_pauses(bundle):
    g = _gov(bundle, [(0.1, "fair"), (0.3, "critical")])
    assert [t["to"] for t in g["transitions"]] == ["REDUCE_KEYFRAMES", "SAFE_PAUSE"]
    assert g["final_policy"]["must_pause"] and g["safe_pauses"] == 1


def test_recovery_with_short_dwell(bundle):
    g = _gov(bundle, [(0.1, "serious"), (0.2, "nominal")], {"down_dwell_s": 0.1})
    assert g["transitions"][-1]["to"] == "NOMINAL"
    assert g["max_level"] == "LOWER_RESOLUTION"


def test_governor_report_deterministic(bundle):
    f = Faults(thermal=[(0.1, "serious")], seed=4, jitter_ms=2)
    a = replay(bundle, ["metrics", "quality", "governor"], f)
    b = replay(bundle, ["metrics", "quality", "governor"], f)
    assert dumps(a) == dumps(b)
