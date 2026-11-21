# 001: Adopt the project docs template

- **Date:** 2026-11-17
- **Status:** Accepted
- **Decided by:** owner
- **Task:** T-001
- **Related charter item:** §10 Amendment rule; workflow for all core features

## Context
Project facts were spread over HANDOFF.md, PROGRESS.md, docs/PLAN.md and docs/specs/, with
no single place for scope, open tasks or session handoffs, and no protection for the
original scope. Work is done by a planner -> coder -> reviewer agent pipeline that needs a
stable contract to read.

## Options considered
1. Keep the existing files only: no new files, but scope and tasks stay implicit and PLAN.md
   mixes history with open work.
2. Adopt the docs template (charter, 01-05, decisions, change requests, amendments, handoffs)
   and link the existing files as owners of their facts: one owner per fact, small read set
   per session; costs a one-time migration.

## Decision
Option 2, adapted to this repo:
- Existing files are not moved or renamed. HANDOFF.md, PROGRESS.md and docs/specs/ stay;
  PLAN.md is frozen (2026-11-14) and its open items moved to 04-TASKS.
- Facts are linked, never copied; the ownership map is docs/README.md.
- The charter is drafted at docs/charter-draft.md; the owner moves it to
  docs/00-PROJECT-CHARTER.md and locks it once. Scope = charter + approved
  docs/amendments/AMD-NNN-*.md; proposals go to docs/change-requests/.
- Handoffs live in docs/handoff/ (one new file per session).
- No backfill: the PROGRESS.md "Decisions and findings log" stays the record before this date.

## Consequences
- Agents read the charter, 04-TASKS, the current phase and the newest two handoffs at start;
  everything else on demand (root CLAUDE.md).
- Status, thresholds, versions, formats and licences keep their existing owners
  (PROGRESS.md, HANDOFF.md §5, pyproject.toml/uv.lock, INTERFACES.md/SCAN_BUNDLE.md, MODELS.md).
