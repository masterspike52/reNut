# Migrating a project onto `Renderer`

> **Scope**: for anyone (human or AI) bringing an existing, independently-developed
> fork/branch into `masterspike52/reNut:Renderer`. Read this before starting a
> migration; read [`START_HERE.md`](START_HERE.md) first if you haven't already — the
> rules there apply here too, this file is the added process for combining two
> divergent histories cleanly. This describes the actual process used to bring
> `linux-work`'s native-renderer work onto `Renderer` (see `history.md`'s "opened a real
> PR branch" entry) — it's a proven process, not a theoretical one.

## Step 0: check whether you even share git history with the target branch

Before anything else:

```sh
git fetch <upstream-remote>
git merge-base <your-branch> <upstream-remote>/Renderer
```

If this prints a commit, you have real shared ancestry and a normal `git merge` or PR
compare will produce a sane, reviewable diff — proceed with a standard merge/rebase.

**If it prints nothing, stop.** Your branch has zero shared history with the target,
even if the content overlaps heavily by file name. This happens when a project's local
checkout was `git init`'d fresh at some point instead of cloned from upstream — it's not
obvious from `git log` alone unless you specifically check ancestry, and it means a
GitHub PR from that branch would not show a normal diff; it would look like you're
replacing almost everything, even files that are byte-identical. This exact situation is
why this file exists — `linux-work` had this problem, confirmed via this same check.

**The fix**: build a new branch on the target's real tip, and manually port your work
onto it file-by-file (Step 2 below), rather than trying to merge/rebase the disconnected
history. Do not force-push your old branch's history onto the new one, and do not use
`git merge --allow-unrelated-histories` — it does not solve the reviewability problem,
it just produces a merge commit with the same wall-of-diff issue.

## Step 1: never bring SDK/vendor source with you

If your project's changes touch an external SDK checkout (as this project's native
renderer did, until the rewrite described in `archive/native-renderer-rewrite-plan.md`),
**do not commit that SDK's source into your branch, and do not add new files inside its
checkout either** — see `nativevk.md`'s hard rule and the plan doc for why "my files
live outside the SDK's own source" is not sufficient on its own if the *code* still
depends on the SDK's internals to function. The target for any rendering work is zero
dependency on rexglue-sdk's own pipeline: hook the game's calls directly, own the
resource/rendering code in this repo.

If your project genuinely needs an SDK-side change, capture it as a minimal, documented
patch (same mechanism as `cmake/patches/rexglue_sdk_native_renderer.patch`) — never as
committed source — and keep it as small as the `nativevk.md` hard rule describes.

## Step 2: bring your branch over in two passes, not one bulk merge

**Pass 1 — files that only exist on your side.** These merge with zero conflict by
construction:

```sh
git checkout -b my-migration <upstream-remote>/Renderer
comm -23 <(git ls-tree -r --name-only your-branch | sort) \
         <(git ls-tree -r --name-only <upstream-remote>/Renderer | sort) \
         > /tmp/new_files.txt
git checkout your-branch -- $(cat /tmp/new_files.txt)
git commit -m "Bring over <project>-only files with no overlap on Renderer"
```

**Pass 2 — files that exist on both sides.** Do not bulk-overwrite these — `Renderer`
carries other collaborators' independent work in them. For each overlapping file:

```sh
diff <(git show <upstream-remote>/Renderer:path/to/file) <(git show your-branch:path/to/file)
```

Then classify it:
- **Identical** — nothing to do.
- **Trivial** (whitespace/newline-only) — leave as `Renderer`'s version.
- **One side is a strict superset/bugfix of the other** — verify by reading every
  removed line and confirming it's stale, not a dropped capability, then take that
  side wholesale.
- **Both sides added independent, real content** (the common case: two people each
  added their own cvar, hook, function, `#ifdef` branch) — hand-merge. Keep both
  additions; do not let one side's change silently delete the other's.
- **Real conflict** (same address/name/key used for two different things) — resolve
  by understanding what each side actually needs, the same way this project's hooks.toml
  merge resolved a duplicate hook address by checking which piece of code the new file
  actually called into (see `history.md`).

This is real, careful, file-by-file work — there is no shortcut that doesn't risk
silently deleting someone else's contribution. Budget time for it accordingly.

## Step 3: verify before calling it done

- Rebuild everything your changes touch; deploy build artifacts and `md5sum`-verify
  copies match, per `START_HERE.md` rule 4.
- Confirm braces/structure are intact on any file you hand-edited (a quick
  `s.count('{') == s.count('}')` check catches most mechanical mistakes).
- Playtest, don't just compile — a clean build is not the same as a correct migration.
- If you touched a generated patch file (Step 1's SDK-patch mechanism), regenerate it
  from a clean diff of the actual external checkout, and sanity-check its line count and
  content before committing — don't assume the checkout only has your intended changes
  in it. It may have unrelated, unverified work sitting in it from other investigation;
  regenerating blindly would ship that too.

## Step 4: clean up before opening the PR

- Strip development-log-style comments (dates, "user-requested", "this session",
  references to your own local planning docs that won't exist for the reviewer) down to
  just the technical reasoning. A comment referencing a date or a conversation reads as
  noise to someone who wasn't there.
- Fix any dangling references to files that didn't come over with you (local-only
  planning docs, session-specific notes).
- Write the PR description in two clearly separated sections: what works immediately
  after merging with no extra setup, and what's not ready / still needs work. Don't let
  scaffolding read as a finished feature — say plainly if something is in-progress.
- Double-check the PR's base branch is actually `Renderer`, not `main` or another
  default — GitHub does not always guess correctly, especially across forks.

## Common mistakes this process exists to prevent

- Opening a PR from a branch with no shared history and being surprised by a
  50,000-line diff instead of the few thousand lines you actually changed.
- Silently deleting another contributor's cvar/hook/fix because a bulk file overwrite
  looked like the fast path.
- Shipping an SDK-checkout patch that includes unrelated, unverified experimentation
  because it was sitting uncommitted in that checkout when the patch was regenerated.
- A PR description that reads like the whole feature works, when large parts of it are
  scaffolding that needs a separate manual setup step (or doesn't work at all yet).
