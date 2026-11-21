"""Enforce docs/INTERFACES.md §9: spatial/ and recon/ never import each other; pipeline
code never imports eval/."""

import ast
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = {
    "spatial": {"recon", "eval"},
    "recon": {"spatial", "eval"},
    "formats": {"spatial", "recon", "eval"},
    "export": {"spatial", "recon", "eval"},
    "qa": {"spatial", "recon", "eval"},
    "router": {"spatial", "recon", "eval", "qa", "export"},
}


def _imports(py: Path) -> set[str]:
    mods = set()
    for node in ast.walk(ast.parse(py.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Import):
            mods |= {a.name.split(".")[0] for a in node.names}
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            mods.add(node.module.split(".")[0])
    return mods


@pytest.mark.light
@pytest.mark.parametrize("pkg", sorted(FORBIDDEN))
def test_no_forbidden_imports(pkg):
    bad = {
        str(p.relative_to(ROOT)): sorted(_imports(p) & FORBIDDEN[pkg])
        for p in (ROOT / pkg).rglob("*.py")
    }
    bad = {k: v for k, v in bad.items() if v}
    assert not bad, f"forbidden imports: {bad}"
