#!/usr/bin/env bash
# Portable SHA-256 of a file with CR bytes removed (Linux / macOS / Git Bash),
# so a CRLF checkout on Windows hashes the same as the LF checkout in CI.
# Prints 64 lowercase hex characters.
set -euo pipefail
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
  echo "usage: _hash.sh <existing file>" >&2
  exit 2
fi
if command -v sha256sum >/dev/null 2>&1; then
  tr -d '\r' < "$1" | sha256sum | cut -d' ' -f1
else
  tr -d '\r' < "$1" | shasum -a 256 | cut -d' ' -f1
fi
