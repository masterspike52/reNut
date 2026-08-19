# Native renderer: rexglue-sdk setup

> **Scope**: everything specific to the `rexgpu-nativevk` plugin and its relationship to
> the external `rexglue-sdk` checkout — setup, the hard boundary rule, and how to keep
> the patch current. Read this before touching anything under `rexglue-sdk-src/` or
> `src/graphics/nativevk/`. If you're not working on the renderer, skip to
> [`../START_HERE.md`](../START_HERE.md).

**This entire architecture is being replaced (decided, not started).** Even though
`rexgpu-nativevk`'s own source lives outside the SDK checkout, it still substitutes a
pipeline from *inside* rexglue-sdk's own draw call, so it still depends on the SDK's
GPU-emulation stack to get there. The decided direction is a full, UnleashedRecomp-style
rewrite with zero dependency on rexglue-sdk's rendering pipeline — see
[`archive/native-renderer-rewrite-plan.md`](archive/native-renderer-rewrite-plan.md) for
the real plan. Everything below describes the current, temporary architecture — accurate
today, but not the target. Once the rewrite starts, this file gets replaced rather than
patched.

## Hard rule: rexglue-sdk itself should not need to be edited

The lead rexglue developer's stated architecture: fixes/features belong in reNut's own
code, using rexglue's existing extension points — not edits to rexglue-sdk source. The
Linux audio fixes (`src/renut_engine/linuxfixes/{audio_backend,audio_client_guard}.cpp`)
are the reference pattern: `REX_HOOK` link-time symbol interposition, zero rexglue-sdk
edits. Before editing anything inside the SDK checkout, ask whether the same effect is
reachable via a hook, an env var, or a new file placed alongside existing SDK files
(additive, not modifying) instead. The two exceptions that *do* require real edits, kept
deliberately minimal and documented inline at their call sites, are: the one virtual hook
on `VulkanCommandProcessor::IssueDraw` (`TryNativeDraw`) that lets a plugin substitute a
native pipeline, and the Vulkan device-feature enablement (`bufferDeviceAddress`,
`descriptorIndexing`, etc.) that bindless native pipelines need — device creation predates
plugin loading, so there is no hook to attach to there. Everything else — pipeline cache,
shader debug panel, Phase 1 draw path, the trace-hook — lives in new files under
`src/graphics/nativevk/` and `src/graphics/vulkan/nativevk_*` inside the SDK tree, added
fresh rather than editing existing rexglue-sdk source.

**rexglue-sdk source is never committed to this repo.** The checkout lives entirely
outside reNut (`REXSDK_DIR`, see setup below) and reNut's git history contains zero SDK
source files — only the generated patch below, which is the sole bridge between what's
committed here and what the external checkout needs.

## Keeping the patch current

`cmake/patches/rexglue_sdk_native_renderer.patch` is a snapshot, not tracked live —
regenerate it (`git diff --binary > cmake/patches/rexglue_sdk_native_renderer.patch` from
inside the SDK checkout) any time you finish a change there, and commit the regenerated
patch in the same reNut commit as the reNut-side change it supports. Check
`git -C /path/to/rexglue-sdk status --short` before assuming the patch is current — an
uncommitted SDK-side change with no matching patch update means a fresh clone following
the setup steps below won't reproduce it.

The native renderer (`rexgpu-nativevk` plugin) builds against a checkout of
[rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) with one small patch applied,
plus this repo's own build wiring. rexglue-sdk itself needs almost no changes — see
`cmake/patches/rexglue_sdk_native_renderer.patch`'s own diff for exactly what's touched (a
single virtual hook on `VulkanCommandProcessor::IssueDraw` for plugins to substitute a native
pipeline, plus the Vulkan device-feature enablement bindless native pipelines need). Everything
else the native renderer actually needs — the pipeline cache, shader debug panel, Phase 1
draw path — lives entirely in this repo's own `src/graphics/nativevk/`-mirrored files inside the
SDK, added fresh rather than editing existing rexglue-sdk source.

## One-time setup

```sh
# 1. Clone rexglue-sdk at the commit this patch was built against.
git clone https://github.com/rexglue/rexglue-sdk.git /path/to/rexglue-sdk
cd /path/to/rexglue-sdk
git checkout 3eb9b511b4140d2769e27be63eae57d41bfa2afa

# 2. Apply the native-renderer patch (from this repo).
git apply /path/to/reNut/cmake/patches/rexglue_sdk_native_renderer.patch

# 3. Point the reNut build at it.
cmake --preset linux-amd64-relwithdebinfo -DREXSDK_DIR=/path/to/rexglue-sdk
```

## After that

A normal build only requires `rexgpu-xenos` (`GPU_PLUGINS xenos` in reNut's own
`CMakeLists.txt`) -- `rexgpu-configure_target()` hard-errors if a listed plugin's target
doesn't exist, and most builds (any plain/unpatched installed SDK package included) don't
have `rexgpu-nativevk` available, so it's deliberately not in the default list. If you've
done the one-time setup above and want to actually build `rexgpu-nativevk`, add `nativevk`
back to that `GPU_PLUGINS` line locally -- don't commit that change, since it breaks the
build for anyone without a patched SDK checkout.

If the patch stops applying cleanly after a rexglue-sdk update, regenerate it from a working
tree with the fix applied: `git diff --binary > cmake/patches/rexglue_sdk_native_renderer.patch`
from inside the rexglue-sdk checkout, and update the commit hash above.
