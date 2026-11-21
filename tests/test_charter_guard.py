"""Charter lock tooling: PreToolUse hook decisions and verify-charter.sh on a temp copy."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.light

REPO = Path(__file__).resolve().parents[1]
HOOK = REPO / "scripts" / "hooks" / "protect-charter.sh"
CHARTER = "docs/00-PROJECT-CHARTER.md"


def _find_bash() -> tuple[str | None, str]:
    """Git Bash / system bash. WSL's System32 launcher is not usable for these scripts."""
    candidates: list[str] = []
    found = shutil.which("bash")
    if found:
        candidates.append(found)
    if os.name == "nt":
        git = shutil.which("git")
        if git:
            # <Git>\cmd\git.exe or <Git>\bin\git.exe or <Git>\mingw64\bin\git.exe
            for root in (Path(git).parents[1], Path(git).parents[2]):
                candidates += [
                    str(root / "bin" / "bash.exe"),
                    str(root / "usr" / "bin" / "bash.exe"),
                ]
    for cand in candidates:
        p = Path(cand)
        if p.is_file() and "system32" not in str(p.resolve()).lower():
            return str(p), ""
    if found:
        return None, f"bash resolves under System32 (WSL launcher): {found}"
    return None, "bash not found on PATH"


BASH, _WHY = _find_bash()
needs_bash = pytest.mark.skipif(BASH is None, reason=_WHY)


def _hook(tool: str, tool_input: dict) -> subprocess.CompletedProcess:
    payload = {
        "session_id": "t",
        "hook_event_name": "PreToolUse",
        "tool_name": tool,
        "tool_input": tool_input,
    }
    return subprocess.run(
        [BASH, HOOK.as_posix()],
        input=json.dumps(payload).encode(),
        capture_output=True,
        timeout=60,
    )


@needs_bash
@pytest.mark.parametrize(
    ("tool", "tool_input", "expected"),
    [
        ("Edit", {"file_path": CHARTER, "old_string": "a", "new_string": "b"}, 2),
        (
            "Write",
            {"file_path": r"C:\Users\me\repo\docs\00-project-charter.MD", "content": "x"},
            2,
        ),
        ("NotebookEdit", {"notebook_path": "/repo/docs/.charter.sha256", "new_source": ""}, 2),
        ("Edit", {"file_path": "README.md", "old_string": "a", "new_string": f"see {CHARTER}"}, 0),
        ("Read", {"file_path": CHARTER}, 0),
        ("Bash", {"command": f"cat {CHARTER} 2>&1", "description": "rm > tee"}, 0),
        ("Bash", {"command": "git log", "description": f"Confirm charter {CHARTER} > x"}, 0),
        ("Bash", {"command": f'echo "a -> {CHARTER}"'}, 0),
        ("Bash", {"command": f"sed -n 1p {CHARTER}"}, 0),
        ("Bash", {"command": "cat scripts/lock-charter.sh"}, 0),
        ("Bash", {"command": f"mv x {CHARTER}"}, 2),
        ("Bash", {"command": f"cp x {CHARTER}"}, 2),
        ("Bash", {"command": f"git checkout -- {CHARTER}"}, 2),
        ("Bash", {"command": f"sed -i s/a/b/ {CHARTER}"}, 2),
        ("Bash", {"command": f"echo hi > {CHARTER}"}, 2),
        ("Bash", {"command": 'echo x >>"docs/.charter.sha256"'}, 2),
        ("Bash", {"command": f"echo ok\nrm -f {CHARTER}"}, 2),
        ("Bash", {"command": "bash scripts/lock-charter.sh"}, 2),
        ("Bash", {"command": "cd /repo && ./scripts/lock-charter.sh"}, 2),
        ("PowerShell", {"command": r"Set-Content -Path docs\00-PROJECT-CHARTER.md -Value x"}, 2),
        ("PowerShell", {"command": r"Get-Content docs\00-PROJECT-CHARTER.md"}, 0),
        # lock-charter.sh via interpreter paths, quotes, .exe and prefixes
        ("Bash", {"command": "/usr/bin/bash scripts/lock-charter.sh"}, 2),
        (
            "PowerShell",
            {"command": r'& "C:\Program Files\Git\bin\bash.exe" scripts/lock-charter.sh'},
            2,
        ),
        ("PowerShell", {"command": r"bash.exe scripts\lock-charter.sh"}, 2),
        ("Bash", {"command": "if true; then bash scripts/lock-charter.sh; fi"}, 2),
        ("Bash", {"command": "for i in 1; do ./scripts/lock-charter.sh; done"}, 2),
        ("Bash", {"command": "timeout 5 scripts/lock-charter.sh"}, 2),
        ("Bash", {"command": "echo | xargs bash scripts/lock-charter.sh"}, 2),
        ("Bash", {"command": "uv run bash -e scripts/lock-charter.sh"}, 2),
        ("Bash", {"command": "uv run scripts/lock-charter.sh"}, 2),
        ("Bash", {"command": "git diff -- scripts/lock-charter.sh"}, 0),
        # PowerShell aliases
        ("PowerShell", {"command": r"del docs\00-PROJECT-CHARTER.md"}, 2),
        ("PowerShell", {"command": r"move x docs\00-PROJECT-CHARTER.md"}, 2),
        ("PowerShell", {"command": r"ri docs\.charter.sha256"}, 2),
        ("PowerShell", {"command": r"sc docs\00-PROJECT-CHARTER.md x"}, 2),
        # redirect target must be the protected file itself
        ("Bash", {"command": f"cat {CHARTER} > {CHARTER}.bak"}, 0),
        ("Bash", {"command": f"cat x > {CHARTER}; echo ok"}, 2),
    ],
)
def test_hook_decision(tool: str, tool_input: dict, expected: int) -> None:
    res = _hook(tool, tool_input)
    assert res.returncode == expected, res.stderr.decode(errors="replace")
    if expected == 2:
        assert b"BLOCKED" in res.stderr


def _sha_lf(data: bytes) -> str:
    return hashlib.sha256(data.replace(b"\r", b"")).hexdigest()


@pytest.fixture
def tmp_repo(tmp_path: Path) -> Path:
    (tmp_path / "scripts").mkdir()
    (tmp_path / "docs").mkdir()
    for name in ("_hash.sh", "verify-charter.sh"):
        shutil.copy(REPO / "scripts" / name, tmp_path / "scripts" / name)
    return tmp_path


def _verify(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [BASH, (root / "scripts" / "verify-charter.sh").as_posix()],
        capture_output=True,
        timeout=60,
    )


TEXT = b"# Charter\n\nGoal: rooms.\n"


@needs_bash
def test_hash_strips_cr_and_matches_contract(tmp_repo: Path) -> None:
    f = tmp_repo / "docs" / "c.md"
    f.write_bytes(TEXT.replace(b"\n", b"\r\n"))
    res = subprocess.run(
        [BASH, (tmp_repo / "scripts" / "_hash.sh").as_posix(), f.as_posix()],
        capture_output=True,
        timeout=60,
    )
    assert res.returncode == 0, res.stderr
    assert res.stdout.decode().strip() == _sha_lf(TEXT)


@needs_bash
def test_verify_intact(tmp_repo: Path) -> None:
    (tmp_repo / CHARTER).write_bytes(TEXT)
    (tmp_repo / "docs" / ".charter.sha256").write_bytes(_sha_lf(TEXT).encode() + b"\n")
    res = _verify(tmp_repo)
    assert res.returncode == 0, res.stdout
    assert b"intact" in res.stdout


@needs_bash
def test_verify_tampered(tmp_repo: Path) -> None:
    (tmp_repo / CHARTER).write_bytes(TEXT + b"Extra scope.\n")
    (tmp_repo / "docs" / ".charter.sha256").write_bytes(_sha_lf(TEXT).encode() + b"\n")
    res = _verify(tmp_repo)
    assert res.returncode == 1, res.stdout
    assert b"CHARTER CHANGED" in res.stdout


@needs_bash
def test_verify_crlf_copy(tmp_repo: Path) -> None:
    (tmp_repo / CHARTER).write_bytes(TEXT.replace(b"\n", b"\r\n"))
    (tmp_repo / "docs" / ".charter.sha256").write_bytes(_sha_lf(TEXT).encode() + b"\r\n")
    res = _verify(tmp_repo)
    assert res.returncode == 0, res.stdout


@needs_bash
def test_verify_not_locked(tmp_repo: Path) -> None:
    (tmp_repo / CHARTER).write_bytes(TEXT)
    res = _verify(tmp_repo)
    assert res.returncode == 0, res.stdout
    assert b"not locked yet" in res.stdout
