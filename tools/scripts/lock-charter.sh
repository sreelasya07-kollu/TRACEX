#!/usr/bin/env bash
# OWNER ONLY. Run after filling in (or officially amending) the charter.
# To amend: first run  chmod u+w docs/00-PROJECT-CHARTER.md , edit, then run this again.
# Claude Code is blocked from running this script by scripts/hooks/protect-charter.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
H=$(bash scripts/_hash.sh docs/00-PROJECT-CHARTER.md)
printf '%s\n' "$H" > docs/.charter.sha256
chmod a-w docs/00-PROJECT-CHARTER.md
echo "LOCKED: Charter is now read-only. SHA-256: $H"
echo "Now commit docs/00-PROJECT-CHARTER.md and docs/.charter.sha256 together."
