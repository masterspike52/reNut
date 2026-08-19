# Start here

You're an AI working on reNut. Read this file in full before doing anything else — it's
short on purpose. It tells you the rules, then where to go for the rest.

## What reNut is, in one paragraph

reNut is a native Linux/Windows port of *Banjo-Kazooie: Nuts & Bolts* (Xbox 360), built
on **rexglue** (a static-recompilation SDK) plus this repo's own game-specific fixes,
tooling, and a from-scratch native GPU renderer effort (`rexgpu-nativevk`). CPU
recompilation ships and is stable. GPU has two interchangeable plugins: `rexgpu-xenos`
(stock, runtime-translated, the correct reference baseline) and `rexgpu-nativevk`
(experimental, statically-compiled pipelines, in progress). If you need more than that,
go to [`research.md`](research.md).

**Planned architecture change (decided, not yet started):** `rexgpu-nativevk` as
described above and in `nativevk.md` is going away. It still substitutes pipelines
from inside rexglue-sdk's own draw call (`VulkanCommandProcessor::IssueDraw`), which
means it still depends on the SDK's GPU-emulation stack (texture cache, render-target
cache, shared memory, primitive processor) to get there, even though its own source
lives outside the SDK checkout. The decided direction is a full replacement:
UnleashedRecomp-style, hooking the game's D3D9 calls directly at their guest addresses
and implementing Vulkan rendering entirely independently, with zero dependency on
rexglue-sdk's rendering pipeline at all. See
[`archive/native-renderer-rewrite-plan.md`](archive/native-renderer-rewrite-plan.md)
for the real plan (existing groundwork already found: every D3D9 entry point needed
already has a known guest address, and `render_hooks_stub.cpp` has a matching orphaned
stub scaffold). Until that lands, treat everything below about `rexgpu-nativevk` as
current-but-temporary, not the end state.

## Hard rules

1. **rexglue-sdk lives outside this repo and should almost never need real edits.**
   The SDK is a separate external checkout (`REXSDK_DIR`); reNut's git history contains
   *zero* SDK source files, ever — only a generated patch that captures what little the
   SDK genuinely needs changed. Before editing anything under `rexglue-sdk-src/`, read
   [`nativevk.md`](nativevk.md)'s hard-rule section and look for a hook-based alternative
   first. If an SDK edit really is unavoidable, keep it minimal, document why inline at
   the call site, and regenerate+commit the patch in the same commit as the reNut-side
   change that depends on it.

2. **Never commit SDK source, generated build output, or game assets.** Check
   `.gitignore` before adding new generated/external paths. When staging with a broad
   `git add`, review `git status` output before committing — this project has previously
   had large personal reference material (Xbox 360 SDK docs) sitting untracked in the
   repo root; that kind of thing gets moved to `reNut-build-scratch/` (a sibling
   directory, outside the repo), not committed.

3. **Verify claims against the real tree/binary, not memory or assumption.** This
   project's history ([`history.md`](history.md)) has multiple entries where a plausible-
   sounding claim turned out wrong on direct inspection (grep, `nm -D`, binary hash
   comparison, actual playtesting) — always prefer that over restating something
   remembered from an earlier session or a doc that might be stale.

4. **After any change to the nativevk/xenos plugins**: rebuild both
   (`ninja rexgpu-nativevk rexgpu-xenos`), copy the built `.so` files from the SDK
   checkout's `out/linux-amd64/` into the game's build dir, `md5sum` both copies to
   confirm they match, and sanity-load each with
   `python3 -c "import ctypes; ctypes.CDLL(path)"` before considering the change done.
   Rebuilding `renut` itself is a separate step only needed when reNut's own sources
   changed.

5. **When something looks broken, find the real root cause before patching around it.**
   Several fixes in `history.md` trace back to a wrong assumption stated confidently
   before being checked. If you're not sure, say so and check, rather than guess.

6. **If a fix reaches a live, confirmed-broken state and the root cause isn't found
   quickly, revert cleanly rather than keep patching forward.** See
   [`archive/deferred-vfetch-flush.md`](archive/deferred-vfetch-flush.md) for a real
   example of this rule being followed correctly.

7. **Division of labor**: the AI fixes/edits/builds; the user plays the actual game to
   verify. Don't claim a rendering/gameplay fix "works" without the user having tested
   it in-game — a clean compile is not the same as a correct fix.

8. **Destructive git operations need explicit confirmation** — force-push, reset --hard,
   amending someone else's commit, discarding uncommitted work. Run `git status` first.
   Only commit when asked to.

## Where the rest of the documentation lives

- [`nativevk.md`](nativevk.md) — the native renderer / rexglue-sdk boundary: setup,
  the hard rule in full, how to keep the patch current. Read before touching the
  renderer or the SDK checkout.
- [`MIGRATION.md`](MIGRATION.md) — the process for bringing an independently-developed
  branch/fork onto `Renderer`: checking git ancestry first, never bringing SDK source
  along, reconciling overlapping files without clobbering other people's work, and
  cleanup before opening a PR. Read before migrating any project onto this branch.
- [`research.md`](research.md) — narrative background: what the project is, current
  architecture, how comparable projects (UnleashedRecomp, DPRecomp, Xenia) solve the
  same problems. Read for context, or to explain the project to someone new.
- [`history.md`](history.md) — terse, chronological log of what was tried, what broke,
  what fixed it. Read for provenance on a specific past decision, or to pick up a
  session exactly where it left off (top section).
- [`archive/`](archive) — dead ends and superseded plans, kept only so the same mistake
  isn't repeated blind. Not part of the fork (see `.gitignore`) — local reference only.
  Skip unless a specific archived topic comes back up.

If you're about to do something not covered by a rule above and you're unsure whether
the user would want it done autonomously, ask rather than assume.
