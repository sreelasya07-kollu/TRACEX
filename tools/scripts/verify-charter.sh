#!/usr/bin/env bash
# Fails (exit 1) if the charter was changed after being locked.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -f docs/.charter.sha256 ]; then
  echo "WARNING: Charter not locked yet. Run scripts/lock-charter.sh (owner only)."
  exit 0
fi
# Strip CR and all whitespace so CRLF / trailing-newline variants compare equal.
EXPECTED=$(tr -d ' \t\r\n' < docs/.charter.sha256 | tr 'A-F' 'a-f')
if [ ! -f docs/00-PROJECT-CHARTER.md ]; then
  echo "FAIL: docs/.charter.sha256 exists but docs/00-PROJECT-CHARTER.md is missing."
  exit 1
fi
ACTUAL=$(bash scripts/_hash.sh docs/00-PROJECT-CHARTER.md)
if [ "$EXPECTED" != "$ACTUAL" ]; then
  echo "FAIL: CHARTER CHANGED! The original project idea must not be edited."
  echo "   Revert with: git checkout -- docs/00-PROJECT-CHARTER.md"
  echo "   For real scope changes use docs/change-requests/."
  exit 1
fi
echo "OK: Charter intact."
