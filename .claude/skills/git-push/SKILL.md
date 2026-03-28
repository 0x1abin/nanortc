---
name: git-push
description: >
  Pre-flight CI validation, commit, and push workflow for the NanoRTC project (C99/CMake embedded WebRTC).
  Runs scripts/ci-check.sh locally before pushing to prevent broken code reaching the remote.
  Auto-fixes clang-format issues, detects patch-on-patch anti-patterns in today's commits,
  checks documentation freshness against code changes, and pushes with upstream tracking.
  Use this skill whenever the user wants to push code, validate before pushing, or do a checked push.
  Trigger on: "push", "git push", "推送", "提交并推送", "push to remote", "push my changes",
  "validated push", "检查并推送", "本地验证后推送", "validate and push", "ci check then push",
  "跑完 CI 再推". This skill replaces plain git push — use it instead.
---

# git-push — NanoRTC Pre-flight Push Workflow

Run local CI, fix formatting, review commits, check doc freshness, then push. The goal is to
catch everything GitHub Actions would catch, before the code leaves your machine.

## Step 1: Pre-flight Status

Gather context. Run ALL of these in parallel:

```bash
git status
git branch --show-current
git remote -v
git log --oneline -5
git rev-parse --abbrev-ref @{upstream} 2>/dev/null || echo "NO_UPSTREAM"
```

Handle edge cases before proceeding:

- **Detached HEAD**: Stop. Tell the user to create or checkout a branch first.
- **No remote configured**: Stop. Tell the user to add a remote.
- **On main/master with uncommitted changes**: Warn and ask for confirmation before continuing.
- **Clean working tree AND up-to-date with remote**: Tell the user nothing to push. Stop.

## Step 2: CI Validation

This is the ONE command that validates everything. Do not replicate its individual checks.

```bash
./scripts/ci-check.sh
```

This script covers: architecture constraints (no platform headers, no malloc in src/),
clang-format, all 3 profile builds (DATA/AUDIO/MEDIA), ctest for each profile,
symbol prefix validation, no global mutable state, and AddressSanitizer build+test.

**If it fails:**

1. Check if the failure is clang-format. Look for `FAIL` next to `clang-format check` in the
   output while other checks passed. If so, auto-fix:
   ```bash
   clang-format -i src/*.c src/*.h include/*.h crypto/*.h crypto/*.c
   ```
   Then re-run `./scripts/ci-check.sh`. If it passes now, the formatting fixes become part of
   the uncommitted changes handled in Step 3.

2. If the failure is NOT clang-format (build error, test failure, architecture constraint
   violation, symbol naming, ASan failure): **stop the entire flow**. Show the failure output
   clearly to the user. Do NOT attempt automatic fixes for anything other than formatting.

## Step 3: Handle Uncommitted Changes

Only reached after Step 2 CI validation passes. If `git status` shows uncommitted changes
(including any clang-format auto-fixes from Step 2):

1. Show changes:
   ```bash
   git status
   git diff HEAD
   ```

2. Stage relevant files by name. **Never stage** files matching: `.env*`, `credentials*`,
   `*.key`, `*.pem`, `*.p12`. Warn the user if such files appear in the working tree.

3. Generate a commit message with conventional commit prefix (`feat:`, `fix:`, `docs:`,
   `refactor:`, `test:`, `chore:`, `style:`) based on the actual changes. If the only change
   is formatting, use `style: auto-fix clang-format`.

4. Commit using HEREDOC format:
   ```bash
   git commit -m "$(cat <<'EOF'
   <type>: <description>

   Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
   EOF
   )"
   ```

If no uncommitted changes, skip this step.

## Step 4: Patch-on-Patch Review (Advisory)

Detect layered incremental fixes in today's commits that could be squashed into cleaner history.

```bash
# Today's commits on this branch
git log --since="00:00" --format='%h %s' --no-merges

# Source files modified in multiple commits today
git log --since="00:00" --format='%h' --no-merges | \
  xargs -I{} git diff-tree --no-commit-id --name-only -r {} | \
  grep -E '\.(c|h)$' | sort | uniq -c | sort -rn
```

**If fewer than 2 commits today:** Skip entirely. Proceed to Step 5.

Look for these signals:
- Any `.c` or `.h` file modified in 3+ commits today (strong signal)
- Fix chains: "implement X" then "fix X" or "fix typo in X"
- Messages containing "forgot to", "actually", "another attempt"

If detected, warn the user:

> Patch-on-patch detected: `nano_stun.c` modified across multiple commits today.
> Consider squashing with `git rebase -i` before pushing.

This is advisory only — if the user says proceed, continue to Step 5.

## Step 5: Documentation Freshness Check (Advisory)

Compare changed files against upstream to detect potentially stale documentation.

```bash
UPSTREAM=$(git rev-parse --abbrev-ref @{upstream} 2>/dev/null || echo "origin/main")
git diff --name-only "$UPSTREAM"...HEAD 2>/dev/null || git diff --name-only HEAD~5..HEAD
```

Apply these mapping rules:

| Changed files | Check this doc | Reason |
|---|---|---|
| `src/nano_*.c` or `src/nano_*.h` | `docs/QUALITY_SCORE.md` | Module grade may need updating |
| `include/nanortc.h` | `ARCHITECTURE.md` | Public API change affects architecture |
| New file in `src/` | `docs/engineering/development-workflow.md` | Module order may need updating |
| `CMakeLists.txt` | `AGENTS.md` | Build instructions may have changed |
| New `tests/test_*.c` | `docs/QUALITY_SCORE.md` | Test coverage column may need updating |
| `crypto/nanortc_crypto*.c` | `docs/references/rfc-index.md` | Crypto RFC refs may need adding |
| Any `src/` or `crypto/` file | `docs/exec-plans/active/*.md` | Active plan progress may need updating |

If staleness detected, list the potential gaps and let the user decide:

> Docs may need updating:
> - `docs/QUALITY_SCORE.md` — nano_stun implemented, grade may need bumping from D
> - `ARCHITECTURE.md` — nanortc.h public API changed

Do not block. The user decides whether to update docs before or after pushing.

## Step 6: Push

Check upstream tracking and push:

```bash
# Check if upstream exists
git rev-parse --abbrev-ref @{upstream} 2>/dev/null
```

- **No upstream**: `git push -u origin $(git branch --show-current)`
- **Has upstream**: `git push`

## Step 7: Post-push Summary

Display a summary in English:

```
Push complete
  Branch: feat/stun-parser
  Commits: 3
  Remote: origin (git@github.com:user/nanortc.git)
  CI validation: PASSED
  Doc reminders: 1 (QUALITY_SCORE.md)
```

If branch is not `main`, suggest creating a PR.

## Rules

- **Never force push** unless the user explicitly requests it. Warn if targeting `main`/`master`.
- **Never skip CI.** Always run `./scripts/ci-check.sh` before pushing. No shortcuts.
- **Never commit secrets.** Abort and warn if any staged file looks like a key, token, or credential.
- **Do not replicate ci-check.sh.** The script handles architecture constraints, formatting,
  3 profile builds, tests, symbol checks, and ASan. Just run it. If the script is updated,
  the skill automatically benefits.
- **Only auto-fix formatting.** Build failures, test failures, and constraint violations need
  human judgment. clang-format is the only auto-fixable issue.
- **Patch-on-patch and doc freshness are advisory.** Warn but never block the push.
- **HEREDOC for commit messages.** Always use the heredoc format.
- **Co-Authored-By trailer** on all auto-generated commits.
- **Prefer new commits over amending.** Format fixes get their own commit.
