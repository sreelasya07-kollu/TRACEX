#!/usr/bin/env bash
# Fast gate: light tests + lint + charter integrity. Exits non-zero on the first failure.
# Run after `uv sync --extra <cpu|cu126|cu130>`.
set -euo pipefail
cd "$(dirname "$0")/.."
uv run ruff check
uv run pytest -m light -q
bash scripts/verify-charter.sh
