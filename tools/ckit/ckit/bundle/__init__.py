"""Scan Bundle I/O and validation (spec/scanbundle/SPEC.md)."""

from pathlib import Path

from ckit.core import CAPTUREKIT_ROOT

SPEC_DIR = CAPTUREKIT_ROOT / "spec" / "scanbundle"
DESC_PATH = SPEC_DIR / "messages" / "capturekit.desc"
SUPPORTED_MAJOR = 1
# 1.1.0: /ar/detections (ADR 0016); 1.2.0: consent + privacy (ADR 0020); 1.3.0: /guide/* (ADR 0028);
# 1.4.0: /imu/* optional, lean bundles + zip archive (ADR 0034)
SUPPORTED_MINOR = 4

__all__ = ["SPEC_DIR", "DESC_PATH", "SUPPORTED_MAJOR", "SUPPORTED_MINOR", "Path"]
