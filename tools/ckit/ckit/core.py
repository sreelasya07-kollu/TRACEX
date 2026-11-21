"""Loads the compiled C++ core (`_ckcore`, built by scripts/build_core_py.sh)."""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

CAPTUREKIT_ROOT = Path(__file__).resolve().parents[2]


def load():
    try:
        return importlib.import_module("_ckcore")
    except ImportError:
        pass
    d = os.environ.get("CKIT_CORE_DIR") or str(CAPTUREKIT_ROOT / "build" / "core-py" / "py")
    if d not in sys.path:
        sys.path.insert(0, d)
    try:
        return importlib.import_module("_ckcore")
    except ImportError as e:
        raise ImportError(
            f"_ckcore not found in {d}. Build it with: capturekit/scripts/build_core_py.sh"
        ) from e
