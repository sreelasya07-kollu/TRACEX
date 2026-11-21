#!/usr/bin/env bash
# Run inventory items in dependency order with resource accounting (see run_heavy.py).
#   scripts/heavy/run_heavy.sh --priority P0 P1
#   scripts/heavy/run_heavy.sh --ids P3-04 P3-12
#   scripts/heavy/run_heavy.sh --priority P0 --dry-run
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec .venv/bin/python scripts/heavy/run_heavy.py "$@"
