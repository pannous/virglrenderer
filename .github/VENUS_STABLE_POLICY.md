# Venus Stable Branch Policy

## Purpose

The `venus-stable` branch maintains a stable, working version of virglrenderer with Venus backend before the breaking zero-copy changes. It serves as a reliable development base.

## Branch Protection Rules

✅ **Enabled protections:**
- Require pull request reviews (1 approval minimum)
- Dismiss stale pull request approvals when new commits are pushed
- Enforce restrictions for administrators
- Prevent force pushes
- Prevent branch deletion

## Merge Policy

### ✅ ALLOWED: Merges from upstream

You may merge commits from the official virglrenderer upstream (GitLab):
```bash
git checkout venus-stable
git fetch upstream
git merge upstream/main  # Or specific upstream commits
```

### ❌ FORBIDDEN: Merges from origin/main

**DO NOT merge from your fork's main branch** (`origin/main`), as it contains the breaking zero-copy code:
```bash
# ❌ NEVER DO THIS:
git merge origin/main
git merge main
```

### ✅ ALLOWED: Cherry-picks from main

You may cherry-pick specific safe commits from main:
```bash
git cherry-pick <commit-hash>  # Only for build scripts, docs, safe fixes
```

## Why This Policy?

The main branch contains breaking commits from Jan 23, 2026:
- `f48b5b19` - milestone zero-copy triangle (17 files, 301 insertions)
- `9b0a9ab2` - SHM fallback fix (attempted fix that didn't work)
- All subsequent zero-copy related changes

These broke Venus memory allocation and rendering. The venus-stable branch excludes this code to maintain working functionality.

## When to Merge Back

When venus-stable is ready to merge back into main:
1. Create a pull request from `venus-stable` → `main`
2. Review thoroughly to ensure no regressions
3. Update main branch to use stable codebase
4. Archive or delete broken zero-copy code from main

## Monitoring Compliance

A GitHub Action checks pull requests to venus-stable:
- ✅ Accepts merges from upstream (GitLab)
- ❌ Rejects merges that include origin/main commits
- ✅ Accepts individual cherry-picks
- ✅ Accepts new development commits

See `.github/workflows/venus-stable-guard.yml` for implementation.

## Questions?

See QEMU repository `/opt/other/qemu/notes/SOLUTION-venus-stable-branches.md` for the complete story of why venus-stable exists and what it excludes.
