# 002: GitHub Actions for CI

- **Date:** 2026-11-17
- **Status:** Accepted
- **Decided by:** owner
- **Task:** T-001 (adoption); T-005 (project tests in CI)
- **Related charter item:** §10 Amendment rule (charter integrity)

## Context
The locked charter is protected by a hash check (`scripts/verify-charter.sh`). A local git
hook can be skipped, so the check must also run on the hosting side. The repo is hosted on
GitHub.

## Options considered
1. GitHub Actions: native to the host, works with branch protection and CODEOWNERS.
2. Local hook only: no server-side guarantee.

## Decision
GitHub Actions. The workflow in `.github/workflows/` runs `bash scripts/verify-charter.sh`
on every push and pull request.

## Consequences
- The check is tamper-proof only once branch protection requires it (T-002); until then it
  is tamper-evident.
- CI runs no project tests yet; adding `pytest -m light` and `ruff check` is T-005.
