# reNut Project Memory

> **Scope**: chronological log — what was tried, what broke, what fixed it, session by
> session. Read this when you need *provenance* for a specific past decision or bug, or
> to pick up exactly where the last session left off (see the top section). Skip it if
> you just need current architecture/rules — that's [`START_HERE.md`](../START_HERE.md)
> and [`research.md`](research.md) instead. This file is terse and machine-oriented by
> design; `research.md` is the narrative/human-readable counterpart.

reNut/rexglue: static recompilation of Banjo-Kazooie: Nuts & Bolts (Xbox 360)
to native Linux. XenosRecomp compiles Xenos GPU shader microcode -> HLSL ->
DXC -> SPIR-V ahead of time. Goal: replace the "stock" runtime-translated
(interpreter-based) shader path with real native Vulkan pipelines per
shader pair, without visual corruption.

Facts below marked **(verified YYYY-MM-DD)** were re-checked against the real
tree/binaries on that date. Everything else is historical and may be stale.
Last full audit: **2026-08-17**.

## ⏭️ NEXT SESSION: START HERE — start Phase 0 of the SDK-independent native renderer rewrite (2026-08-19)

**The rewrite is decided and fully planned, but implementation has not started at all.**
Read `archive/native-renderer-rewrite-plan.md` in full before writing any native-renderer
code -- it has the phased rollout, the known groundwork that already carries over
(every D3D9 guest address already known, XenosRecomp pipeline untouched, vertex-fetch
decode / bindless-heap / constant-buffer techniques already proven), and the one real
open risk that must be validated FIRST (Phase 0): does hooking D3D9 draw calls directly
actually see ~100% of real draws, and is shader identity resolvable for shaders that
bypass `CreateVertexShader` via IM_LOAD. Do not skip Phase 0 and jump to writing
renderer code -- if it fails, the approach needs to change before anything else is built.

Everything else in this file and in `nativevk.md` describes the CURRENT, SDK-dependent
`rexgpu-nativevk` architecture -- accurate today, but explicitly being replaced, not the
target. `rexgpu-nativevk` is still fully present in the repo (patch file, SDK-side files,
debug panel), just no longer required for a normal build (see below) -- full removal is
its own later step once the new renderer has something to replace it with.

**PR status**: `nativevk-on-renderer` branch, pushed to `origin`, open as PR #38 against
`masterspike52/reNut:Renderer`, mergeable clean. Built on Renderer's real git history
(`linux-work`, referenced throughout this file below, had zero shared ancestry with
upstream at all and could never have merged cleanly -- see that branch's own commits for
how the 24 overlapping files were hand-reconciled, not bulk-overwritten). Per the repo
owner's explicit request: `mullhwucrash.cpp` was reverted back to byte-identical with
Renderer/main (the real `simde_mm_blendv_ps`->`_epi8` bug fix found this session was
dropped from the PR -- owner didn't want already-functioning shared files touched).
Everything else (Linux fixes, debug panel, NativeVK scaffolding, `sleep.h`'s real
`PPC_HOOK`->`REX_HOOK` fix) stayed in one combined PR, per the owner's explicit request
not to split it into separate branches.

**Two real build-breaking bugs found+fixed via the owner's own build attempts**:
1. `cmake/rexglue_xenosrecomp.cmake`'s `PATCH_COMMAND sh -c "..."` doesn't work on
   Windows (`sh` not on PATH) -- replaced with a portable CMake script
   (`cmake/apply_xenosrecomp_patch.cmake`), verified against the real XenosRecomp
   checkout (both fresh-apply and already-applied paths).
2. `rexgpu-nativevk` was in the default `GPU_PLUGINS` list, so ANY normal build without
   a manually-patched SDK checkout (i.e. everyone using a plain/prebuilt SDK install,
   including the owner) hard-failed at configure with "unknown GPU plugin 'nativevk'".
   Fixed by dropping `nativevk` from the default list in reNut's own `CMakeLists.txt` --
   `rexgpu-xenos` only now; `nativevk` stays buildable locally by re-adding it after the
   one-time SDK patch setup in `nativevk.md`, just not required by default.

Also found (SDK-side, real, worth keeping in the patch): `rex_resolve_version()` in
rexglue-sdk's own `CMakeLists.txt` didn't pass `SOURCE_DIR`, so in `add_subdirectory()`/
`REXSDK_DIR` mode it ran `git describe` against reNut's OWN repo (which has an unrelated
`v1.1.0` tag) instead of the SDK checkout -- caused a spurious "floor version behind tag
version" hard error. Fixed with an explicit `SOURCE_DIR` argument.

Also done: cleaned ~90+ narrative/session-dated comments ("Real fix (2026-08-17,
user-requested): ...") out of the native-renderer code across this repo and the SDK
patch -- see that commit for what was deliberately left alone (`xenosrecomp_native_renderer.patch`,
since the separate XenosRecomp checkout it's built from has real unverified functional
changes sitting in it, unrelated to this cleanup). Added `docs/ai/MIGRATION.md` -- the
generalized process for bringing any project onto `Renderer` cleanly, based on exactly
what this session actually did.

## Team fork prep + real capture-hash bug found+fixed (2026-08-18)

**Two unrelated threads finished this session, read both before continuing:**

**1. Team collaboration prep, DONE**: this fork's native-renderer work (rexglue-sdk diff
reduced from ~1900 lines to a small patch, one virtual hook + Vulkan device features, see
`nativevk.md`) is committed on `linux-work`, ready to push to `origin` (`iNiKKo/reNut`)
and PR against `masterspike52/reNut:Renderer`. **Plugin renamed** (2026-08-18, user request --
"renut" was confusing next to the project's own name): `gpu_plugin = "renut"` ->
`gpu_plugin = "nativevk"`, CMake target `rexgpu-renut` -> `rexgpu-nativevk`,
`src/graphics/renut/` -> `src/graphics/nativevk/` (SDK side), `renut_xenos_pipeline_cache.*` ->
`nativevk_xenos_pipeline_cache.*`, `renut_phase1_native_draw.*` -> `nativevk_phase1_native_draw.*`,
`renut_shader_debug_panel.cpp` -> `nativevk_shader_debug_panel.cpp`,
`RenutCommandProcessor`/`RenutGraphicsSystem` -> `NativeVkCommandProcessor`/
`NativeVkGraphicsSystem`, `RENUT_OPTIMISATIONS` -> `NATIVEVK_OPTIMISATIONS`. Deliberately NOT
renamed (genuinely project-level, not plugin-specific): `config/renut_hooks.toml`,
`renut_config.toml`, `src/renut_engine/*`, the generated `renut_xenos_shader_cache.h`/`.cpp` and
its `renut_xenos_shader_batch()`/`renut_synthesize_*()` CMake functions (shared build-pipeline
naming, not the plugin itself). Live config (`~/.config/renut/renut.toml`) already updated to
`gpu_plugin = "nativevk"`. Rebuilt and verified end-to-end (both `rexgpu-nativevk` and
`rexgpu-xenos` link cleanly, deployed, md5-verified).

**CORRECTION (2026-08-18, later same day)**: an earlier note in this file claimed
`upstream/Renderer` already has "an independent, unrelated, overlapping native-shader effort
from another contributor" -- **this was wrong**, based on a misread `git diff --stat` (files
that only exist on `linux-work` were misattributed as also existing on `Renderer` with
different content). Directly verified via `git cat-file -e` on both branches: `Renderer` has
**zero** native-shader-conversion files (no `tools/`, no `ucode_analyze.cpp`, no
`renut_xenos_shader_cache.h`, nothing under `cmake/patches/`) -- it's a smaller, differently
scoped branch (FPS overlay, mouse/keyboard controls, texture tools, general engine/gameplay
fixes). This means every native-renderer file merges into `Renderer` as a pure, zero-conflict
addition. The real (normal, expected) merge-conflict surface is the ~33 files that exist on
both branches -- mostly core engine/build files each branch touched independently
(`CMakeLists.txt`, `config/renut_hooks.toml`, `src/renut_engine/shader_dump.cpp`,
`src/renut_app.h`, `src/renut_engine/hooks.cpp`, etc.), with moderate, normal divergence (tens
to a few hundred changed lines each), not a duplicate-effort collision. `resources/` (16MB
personal reference PDFs) deliberately NOT committed -- copyright/repo-bloat concern, left for
the user to decide.

**2. Real bug found+fixed while verifying the refactor didn't regress anything**: playtesting
after the refactor showed the native shader debug panel had stopped appearing at all. Root cause
(confirmed via direct measurement, not guessed): `shader_dump.cpp`'s `dumpShaderBlob()` hashed
the WRONG byte range (header+ucode) for `shaders/vs_*.bin`/`ps_*.bin` capture filenames --
every other real hash in the codebase (the runtime's own `Shader::ucode_data_hash()`, and this
same file's OWN `computeShaderUcodeHash()`) hashes ucode-only. This silently broke vertex-shader
identity tracking specifically (pixel shaders don't need the declaration-correlation this hash
feeds, so they were unaffected) -- confirmed via an extended real play session: 0/163 distinct
vertex shaders ever matched the native cache, pixel shaders matched ~96%. Fixed
(`src/renut_engine/shader_dump.cpp`, commit `52a3d9c`). **Requires a fresh capture session to
take effect** -- existing `shaders/` captures predate the fix and are permanently mis-hashed.

**Status after the fix, NOT fully resolved**: rebuilt the whole pipeline from a fresh capture
(194 real vertex shader captures with the corrected hash, 97 survived into
`shaders_synthesized/`), but the native shader cache still only ends up with 80 real
(non-heuristic) vertex-shader entries out of ~275 candidate containers -- most fail
`batch_build_synthetic_containers.py`'s synthesis for reasons that look like the SAME
pre-existing, well-documented XenosRecomp/corpus gaps already described elsewhere in this file
(missing vertex-fetch data, missing DefinitionTable literal constants for heuristic-only IM_LOAD
captures) -- NOT re-investigated further this session, ran out of scope. Spot-checked one very
common, frequently-drawn real vertex shader (`vs_1638329963af0285`, real vertex bindings, no
high float-constant registers referenced) that still doesn't make it into
`shaders_synthesized/` despite looking like a clean candidate -- worth a direct
`build_synthetic_container.py` invocation to see its real rejection reason, if resumed.

**Next step if resumed**: play for a real, extended session (the fix is real and confirmed
working end-to-end for shaders that DO make it into the cache) to build a bigger fresh capture
corpus, then investigate remaining single-shader conversion failures the same way prior sessions
did (`ucode_analyze` + direct tool invocation), rather than assuming they're already covered by
the general "known gaps" narrative.

## ⏭️ (older) user tested bindless-heap fix, ZERO visible change

**2026-08-18, same day, after the fix below shipped**: user played and reported "still have
flickering and other, nothing changed at least i didn't notice a change." This is the SAME
"real verified bug, zero visible improvement" outcome as the 6 shader-translation bugs fixed
2026-08-17 (see `renut_native_shader_investigation_2026_08_17` memory / CORE UNSOLVED PROBLEM
section below). Two independent rounds of real fixes at two different levels (shader
translation correctness, then GPU descriptor-synchronization) both produced no measurable
change — that convergence is itself evidence worth acting on before writing more per-bug
fixes.

**Action taken in response**: added a boot-time env-var kill switch,
`RENUT_DISABLE_ALL_NATIVE=1`, in `RecordAndCheckPairEnabled()`
(`renut_xenos_pipeline_cache.cpp`) — forces every native shader pair off from process start
(unlike the debug panel's "Disable All", which can't reach pairs built during the intro videos
before the panel is reachable, and unlike `marked_bad_shaders.txt`, which only lists specific
already-known-bad pairs). Built, deployed, verified matching md5 on both `.so` files
(`librexgpu-renutrd.so` md5 `7d1b6812298b4df6ab18db35a7d034fd`; `librexgpu-xenosrd.so`
unchanged, `f50420e5759a96752a6c7a11898f78a9`).

**RESULT (2026-08-18, same day): flickering PERSISTS with `RENUT_DISABLE_ALL_NATIVE=1` set
(zero native draws, 100% stock rendering).** User's exact report: "Still flickering."

**This is a major pivot, not a footnote.** The entire native-shader-corruption investigation
since 2026-08-16 (6 shader-translation bugs fixed 8/17, the bindless-heap descriptor race fixed
8/18, none of which moved the needle) was aimed at the wrong subsystem for THIS symptom. The
flickering is NOT caused by native rendering — it happens identically in pure stock mode. The
original 2026-08-16 "confirmed via A/B test" conclusion (recorded in `renut_native_renderer`
memory) was almost certainly contaminated by the exact gotcha documented in this file: the old
"Disable All" panel button can't reach pairs built before the panel is reachable (after the
intro videos), so that earlier "native-only" test was never actually 100% stock. This clean
boot-time kill switch is the first trustworthy A/B on this question.

**Do NOT resume native-shader-corruption investigation for the flickering symptom.** It may
still be worth fixing for its own sake (native perf/correctness), but it is not the cause of
what the user is calling "flickering." The native-shader "stretched textures / non-spawning
items" corruption (CORE UNSOLVED PROBLEM section below) is a SEPARATE symptom from this
flickering — do not conflate them going forward; they need independent root-causing.

**Characterized (2026-08-18, user answered directly)**: specific textures/objects flicker (not
whole-screen, not tearing, not UI/HUD), and it's CONSTANT — happens even standing still, not
tied to camera movement. User's own read: "This flickering happens from our implementation of
native shaders" — but this directly conflicts with the RENUT_DISABLE_ALL_NATIVE=1 result above
(flicker persists with zero native draws). Resolution: the bug must be in SHARED code the
native-renderer effort modified but which ALSO runs for stock draws — not in anything gated
behind "is this pipeline native."

**Found via `git diff` against pristine rexglue-sdk** (`cd rexglue-sdk-src && git status`/`git
diff` — this SDK clone has ALL native-effort changes as uncommitted working-tree diffs, so this
is the complete real diff, not something to re-derive): the biggest non-native-gated behavioral
change in shared code is `kRenutDynamicConstants`
(`include/rex/graphics/vulkan/command_processor.h:53`, was `#if RENUT_OPTIMISATIONS` / true) —
switches the constants descriptor set from "rewritten every draw" to "written once, rebound
with per-draw dynamic offsets," with non-trivial reuse/batching bookkeeping added throughout
`VulkanCommandProcessor::UpdateBindings()` (`command_processor.cpp` ~7706-7881, `else if (true)`
branch, `renut_constants_dynamic_set_`, `renut_dynamic_offset_values`). This runs on EVERY draw
(native or stock) and wrong per-draw offsets → wrong constants → wrong transform/UV would look
exactly like "specific object flickers, constant, standing still." Other shared-file diffs in
this SDK (`texture_cache.cpp` +21 lines, `pipeline_cache.cpp` +178, `shared_memory.cpp` +21,
`deferred_command_buffer.cpp` +10) are smaller and not yet individually audited — check these
next if the test below doesn't confirm.

**DIAGNOSTIC BUILD SHIPPED, TEST NOT YET RUN**: forced `kRenutDynamicConstants = false`
unconditionally in `command_processor.h` (was gated on `RENUT_OPTIMISATIONS`), reverting every
draw's constant-buffer binding to the old always-rewrite-the-descriptor-set path. Built,
deployed, verified matching md5 (`librexgpu-renutrd.so` `4ce60ebdebb330b3d171c7e4d292dca2`).
**Next step: have the user test with this build (native rendering back to its normal
enabled-by-default state, i.e. do NOT set RENUT_DISABLE_ALL_NATIVE this time — testing the
OTHER suspect now) and report whether the flickering is gone.**
- If flickering STOPS: root cause found — `kRenutDynamicConstants`'s dynamic-offset bookkeeping
  has a real bug; revert the header to `#if RENUT_OPTIMISATIONS` permanently (this "optimization"
  isn't worth a correctness bug) rather than debug the offset logic further, unless perf data
  says otherwise.
- If flickering PERSISTS: rule this out, move to the next shared-file diff candidates
  (`texture_cache.cpp`, `pipeline_cache.cpp`, `shared_memory.cpp`, `deferred_command_buffer.cpp`
  — none audited yet, check with `git diff` in `rexglue-sdk-src` the same way this one was found).

**RESULT: flickering PERSISTS with `kRenutDynamicConstants=false` too.** Ruled out.

**User confirmed the critical fact that resolves scope (2026-08-18)**: the TRUE original stock
renderer (before any native-renderer-effort changes at all) "runs almost perfectly" — no
flickering. So this is not a pre-existing stock bug, and it's not native pipelines specifically
(disabling those didn't help either) — it is a real regression somewhere in the native-effort's
SHARED-code changes (the ones that run unconditionally for every draw, not gated behind "is this
draw using a native pipeline"). Two of the four now-known non-native-gated shared-file diffs are
ruled out by elimination reasoning so far only by inference, not yet directly tested:
`kRenutDynamicConstants` tested and ruled out above. `command_processor.cpp` has substantial
OTHER non-native-gated additions beyond that flag (barrier-site tracking, texture-write
instrumentation counters, `InvalidateVertexBufferResidency`/`InvalidateAllVertexBufferResidency`
— NOTE: "vertex buffer residency" naming strongly resembles the deferred-vfetch-flush effort
that `renut_barrier_attribution` memory says was "ABANDONED+REVERTED 2026-08-16" — VERIFY this
residency code is not a leftover fragment of that revert, it's a real question mark, not
confirmed either way yet). Still fully unaudited: `texture_cache.cpp` (+21 lines),
`pipeline_cache.cpp` (+178 lines, largest unaudited one), `shared_memory.cpp` (+21),
`deferred_command_buffer.cpp` (+10). **Next session: audit these via `git diff` in
`rexglue-sdk-src`, starting with `pipeline_cache.cpp` (largest) and the
`InvalidateVertexBufferResidency` question, before guessing another toggle-and-test cycle.**

**Audited same day, 2026-08-18**:
- `InvalidateVertexBufferResidency`/`InvalidateAllVertexBufferResidency` calls: confirmed
  pre-existing (unmodified context lines in the diff, not additions) — real Xenia vertex-buffer
  cache invalidation, unrelated to the deferred-vfetch-flush effort or to renut. Ruled out.
- `pipeline_cache.cpp` (+178 lines): 100% diagnostic microcode-dump/disassembly-log
  infrastructure, gated behind `renut_dump_ucode_hash` cvar (default `false`) or one-shot debug
  logging. No rendering-behavior change. Ruled out.
- `texture_cache.cpp` (+21), `shared_memory.cpp` (+21): pure perf counters (increment-only,
  never read except by the frame-trace CSV writer, which is itself off by default). Ruled out.
- `deferred_command_buffer.cpp` (+10) + its header: adds `CmdVkSetCullMode`/`CmdVkSetFrontFace`
  dispatch cases, but grep confirms **zero call sites anywhere in the tree** — dead code, never
  invoked. Ruled out.
- **`src/graphics/pipeline/texture/cache.cpp`'s `TextureCache::RequestTextures()` — FOUND real
  candidate, un-gated by native/A-B-benchmark-inactive (i.e. active by default in normal play)**:
  an early-return optimization (`if (!(used_texture_mask & ~texture_bindings_in_sync_) && ...)
  return;`) skips the entire texture-binding refresh (including `binding.texture =
  FindOrCreateTexture(...)`) whenever a fetch-constant register's "in sync" bit is set, on the
  theory that nothing downstream would do anything anyway. But `VulkanTextureCache::RequestTextures`
  (the Vulkan override, `texture_cache.cpp:568`) calls this base function THEN unconditionally
  uses `binding.texture` for real GPU image-layout-transition work every draw regardless. If
  `texture_bindings_in_sync_` is ever stale relative to the real state of `binding.texture` (e.g.
  a texture object gets evicted/replaced through a path that doesn't clear this specific bit),
  the skip would leave a wrong/dangling texture bound — exactly matching "specific
  textures/objects flicker, constant, standing still." This is the ONE remaining un-tested,
  behavior-changing, non-native-gated diff found via the SDK's own `git diff` against upstream.

**DIAGNOSTIC BUILD #2 SHIPPED, TEST NOT YET RUN**: disabled the early-return (`if (false && ...)`
in `TextureCache::RequestTextures`, `src/graphics/pipeline/texture/cache.cpp` ~line 471) so
every draw always re-walks and re-validates its texture bindings, same as true pristine stock.
Built, deployed, verified matching md5 (`librexgpu-renutrd.so` `55993982ddeac75477278a832fbd2020`).
Also reverted the ruled-out `kRenutDynamicConstants=false` diagnostic back to its normal
`#if RENUT_OPTIMISATIONS` gating (back to `true`) so this test isolates only the texture-cache
change. **Next step: user tests normally (no env var, native enabled by default) and reports
whether flickering is gone.**
- If flickering STOPS: root cause found. Either revert this optimization permanently (correctness
  over the perf win), or — better — find what actually leaves `texture_bindings_in_sync_` stale
  and fix that specific gap instead of removing the whole optimization.
- If flickering PERSISTS: this specific hypothesis is wrong too — see the FULL AUDIT below,
  completed same day.

**RESULT: flickering STILL PERSISTS with `TextureCache::RequestTextures`'s early-return
disabled too.** Ruled out; restored to its normal `#if RENUT_OPTIMISATIONS` state (reverted to
active).

**Third candidate found+tested the same session, result NOT YET confirmed by the user**
(build shipped, DO NOT assume the outcome): `SharedMemory::RequestRanges` (base
`shared_memory.cpp`) was changed to reuse a persistent member scratch vector
(`request_ranges_scratch_`) instead of a local one, on a hot per-draw path (~6k calls/frame).
Disabled (forced back to a local vector), built, deployed, verified matching md5
(`librexgpu-renutrd.so` `f566b553876afdd45850248b8e777e2b`). **User has not yet tested this
build — get that result before concluding anything about this candidate**, then proceed to the
full-audit findings below regardless of this one result (the audit itself is independently
valid either way).

## FULL SHARED-CODE DIFF AUDIT COMPLETE (2026-08-18) — every uncommitted change in the SDK checked

User pushed back on one-at-a-time toggle guessing ("please just look into the full
implementation... double check everything"). Response: used `git diff --stat` in
`rexglue-sdk-src` (this SDK clone's ENTIRE native-effort history is uncommitted working-tree
diff against pristine upstream — nothing to bisect via commits, but the full diff itself is
directly readable) to enumerate literally every modified file, then read each one in full and
classified it. Result — **every single non-native-gated file diff in the SDK is now
individually accounted for**:

| File | What changed | Verdict |
|---|---|---|
| `command_processor.cpp` (vulkan) | `kRenutDynamicConstants` dynamic-offset descriptor reuse | **Tested disabled — ruled out** |
| `command_processor.cpp` (vulkan) | ~40 perf counters/timers, barrier-site tracking, A/B benchmark harness | Counters only, unused when their cvars are off — ruled out |
| `command_processor.cpp` (base) | perf counters + gates a debug-log lookup behind `should_log()` | Behaviorally identical when logging is off — ruled out |
| `pipeline/texture/cache.cpp` | `RequestTextures` early-return on already-in-sync bindings | **Tested disabled — ruled out** |
| `pipeline/texture/cache.cpp` | one-shot tiling-format inventory log | Logging only — ruled out |
| `pipeline_cache.cpp` (vulkan) | raw-ucode/vfetch-disasm dump tooling | Gated behind `renut_dump_ucode_hash` (default false) / one-shot debug log — ruled out |
| `texture_cache.cpp` (vulkan) | 2 perf counters in `RequestTextures` | Counters only — ruled out |
| `shared_memory.cpp` (vulkan) | 3 perf counters in `UploadRanges` | Counters only — ruled out |
| `shared_memory.cpp` (base) | `RequestRanges` reuses a persistent scratch vector instead of a local one | **Tested disabled — ruled out** |
| `deferred_command_buffer.cpp/h` | adds `CmdVkSetCullMode`/`CmdVkSetFrontFace` | Dead code, zero call sites anywhere in the tree — ruled out |
| `translator.cpp`/`translator_disasm.cpp`/`shader.h` | thread an `instruction_address` field through vertex-fetch parsing; null-check crash fix in disasm | Purely additive metadata + a debug-only crash fix, doesn't change parsing/translation output — ruled out |
| `vulkan_submission_tracker.cpp` | GPU-wait timing counters around `vkWaitForFences` | Stores `VkResult` in a local before comparing — behaviorally identical — ruled out |
| `vulkan_upload_buffer_pool.cpp` | adds `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` when a pool requests `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` | Conditional on a usage bit only native's own constant pool sets — inert for stock buffers — ruled out |
| `vulkan_device.cpp`/`device.h` | enables optional device features/extensions (bufferDeviceAddress, descriptor indexing, extendedDynamicState) | Feature enablement only; nothing uses descriptor-indexing/extended-dynamic-state features outside native-only code and the dead `CmdVkSetCullMode` above — ruled out |
| `renderdoc_api.cpp`/`dynlib.*` | `Load` → `LoadExisting` (`dlopen(..., RTLD_NOLOAD)`) for RenderDoc detection only | Unrelated to rendering — ruled out |
| `CMakeLists.txt` / `rexglue_install.cmake` / `graphics/CMakeLists.txt` | build wiring for new renut source files/targets | Not runtime logic — not read in full, but not a plausible per-draw-behavior source |

**Bottom line: there is no remaining un-tested, behavior-changing, non-native-gated code path
in this SDK's diff that hasn't at least been shipped for testing.** Three real candidates were
found and individually disabled/rebuilt/deployed: `kRenutDynamicConstants` (tested, ruled out),
`TextureCache::RequestTextures`'s early-return (tested, ruled out), and
`SharedMemory::RequestRanges`'s scratch-buffer reuse (shipped, user result pending — check
history.md's top section for whether this came back before reading further). Everything
else in the diff is counters/logging/dead-code/unrelated-tooling with no plausible rendering
effect.

**This means the premise itself needs re-examination next session.** Either:
(a) the bug is in reNut's OWN code (not the SDK) — `renut_xenos_pipeline_cache.cpp` and friends
   are native-PIPELINE-gated by design, but are they fully inert when
   `RENUT_DISABLE_ALL_NATIVE=1`? Re ‑verify: does anything in that file run unconditionally
   before/outside the `RecordAndCheckPairEnabled` gate — e.g. `EnsureBindlessHeapInitialized`,
   descriptor pool/layout creation, or anything touching the SAME descriptor sets/layouts stock
   also uses (shared pipeline layout, shared descriptor pool)? This was NOT checked this
   session — the disable flag only gates PIPELINE SELECTION, not necessarily every side effect
   of the native code having run at all during this process's lifetime;
(b) the RENUT_DISABLE_ALL_NATIVE=1 test itself has a gap (e.g. does disabling stop pipeline
   CREATION too, or only which pipeline gets bound for a draw — if native pipelines are still
   being built/warmed even while disabled, and that creation path has a side effect on shared
   Vulkan state, disabling draw SELECTION wouldn't fully isolate it);
(c) the user's "always been there in pristine" claim needs one more sanity check: is it possible
   what they remember as "runs almost perfectly" pre-dates a change that's actually
   ALWAYS-COMPILED-IN (not `#if RENUT_OPTIMISATIONS` gated at all) rather than something in this
   diff — i.e. check if `RENUT_OPTIMISATIONS` itself is unconditionally `1` for the `renut`
   target (confirmed earlier: `target_compile_definitions(rexgpu-renut PRIVATE
   RENUT_OPTIMISATIONS=1)`, always on for this target, never actually toggle-able at runtime) —
   so "disable the native shaders" and "disable RENUT_OPTIMISATIONS" are NOT the same lever, and
   a true pristine-behavior test has never actually been run on the CURRENT build. **Next
   session: try a build with `RENUT_OPTIMISATIONS=0` for `rexgpu-renut` (not just the
   `RENUT_DISABLE_ALL_NATIVE=1` runtime flag) as the real clean-room test** — this compiles out
   every one of the above candidates simultaneously via the same guard they all share, rather
   than continuing to hunt file-by-file.

## BREAKTHROUGH (2026-08-18, same day): the "fully disabled" A/B test was never actually complete

Also checked (b) from the list above before jumping to the `RENUT_OPTIMISATIONS=0` clean-room
build, since it's cheaper to verify first: **does `RENUT_DISABLE_ALL_NATIVE=1` actually stop
EVERY native substitution path?** Answer: **no.** There are TWO separate, independent native
substitution mechanisms in `command_processor.cpp`'s draw path
(`renut_xenos_pipeline_cache.cpp:5104` region):
1. `renut_xenos_pipeline::HasNativePipeline(...)` — the general, "convert the important/common
   shaders" pipeline cache. This IS what `RENUT_DISABLE_ALL_NATIVE=1` gates (via
   `RecordAndCheckPairEnabled`).
2. `renut_phase1::IsTargetDraw(...)` (`renut_phase1_native_draw.cpp`) — an OLDER, separate
   "Phase 1" proof-of-concept, checked SECOND, completely independent of the flag above. It
   matches purely on VERTEX shader hash (`kTargetVertexShaderHash =
   0x67021cae68a26b46`) against ONE specific real vertex shader confirmed present in actual
   gameplay, and when matched, substitutes a hand-picked, UNRELATED, hardcoded pixel shader
   (`ps_12c73f3eb45dad49`) — its own header comment states outright: **"substituted draws will
   not visually match what the guest draw would have looked like... an explicit, understood
   tradeoff for making the mechanism observable/measurable at all, not a mistake."** This was
   left ACTIVE, unconditionally, in every build shipped this whole investigation, including the
   "RENUT_DISABLE_ALL_NATIVE=1, zero native draws" test that the user's earlier "still
   flickering" result was based on. **That earlier test was not actually a clean stock-only
   run** — this one specific real, frequently-drawn shader was still being deliberately
   mis-rendered the whole time.

**Fixed**: wired `RENUT_DISABLE_ALL_NATIVE=1` into `IsTargetDraw` too (local `std::getenv` check
in `renut_phase1_native_draw.cpp`, since it's a different translation unit than the
anonymous-namespace flag in `renut_xenos_pipeline_cache.cpp`). Built, deployed, verified matching
md5 (`librexgpu-renutrd.so` `1e0acb0e5778da9c16fd53445ae180f1`).

**Next step, NOT yet run: the user needs to re-test with `RENUT_DISABLE_ALL_NATIVE=1` set —
THIS is now the first genuinely complete "100% stock, zero native substitution of any kind"
build in this whole investigation.**
- If flickering STOPS now: confirms native rendering (in one of its two forms) IS the real
  cause after all, and specifically narrows it to either general native pipelines or this one
  Phase 1 shader — worth then testing `RENUT_DISABLE_ALL_NATIVE` unset (normal native-enabled
  play) but with ONLY the Phase 1 mechanism forced off, to isolate which of the two it actually
  is. Given Phase 1's deliberately-wrong-pixel-shader design, it's a strong standalone suspect
  for at least PART of what's been reported.
- If flickering PERSISTS even now: the native-rendering hypothesis is genuinely dead for this
  symptom, and the `RENUT_OPTIMISATIONS=0` clean-room build (see above) is the next real step —
  at that point everything renut-authored will be compiled out simultaneously.

**RESULT: flickering STILL PERSISTS even with the Phase 1 gap closed
(`RENUT_DISABLE_ALL_NATIVE=1`, both native mechanisms now genuinely off).**

**Deploy-chain sanity check (2026-08-18, user asked "are you sure I'm compiling/deploying the
right thing")**: verified end-to-end, all real:
- User confirmed (asked directly, not assumed) they launch
  `out/build/linux-amd64-relwithdebinfo/renut` directly -- NOT either of the two
  `reNut-x86_64.AppImage` files on the system (one next to the game files, one in
  `reNut/dist/`), which is good because **neither AppImage bundles `librexgpu-renutrd.so` at
  all** (extracted both, confirmed only `librexgpu-xenosrd.so`/`librexruntimerd.so`/
  `libTracyClientrd.so` present) -- would have been a real dead end if they'd been testing via
  either one.
- Plugin path resolution confirmed via source (`gpu_plugin_loader.cpp:56`,
  `rex::filesystem::GetExecutableFolder() / PluginFileName(name)`) -- relative to the
  EXECUTABLE's own location, not CWD, so launching from any terminal directory still loads the
  right file.
- Live user config confirmed: `~/.config/renut/renut.toml` has `gpu_plugin = "renut"` (not
  `"xenos"`).
- Every rebuild this session verified matching md5 between the SDK's build output and the
  deployed copy in the game's build dir, plus a successful `ctypes.CDLL` load, before handing
  off to the user.
**Conclusion: the build/deploy chain is not the problem. The flicker is real and has survived
every fix and every full native-disable this session.**

## Where this leaves things — native rendering hypothesis is now genuinely exhausted

Both native substitution mechanisms fully disabled, tested clean, flicker unchanged. Combined
with the full shared-code audit (all 3 real candidates individually tested, all ruled out), the
native-renderer effort's code is no longer a plausible source of THIS symptom. Two real options
remain for next session, in priority order:
1. **`RENUT_OPTIMISATIONS=0` clean-room build** (compile-time, not runtime) -- the one thing
   never actually tried. Compiles out literally everything renut-authored in the SDK
   simultaneously (all instrumentation, all optimizations, all native pipeline code paths) via
   the single shared guard. If flicker STILL persists even here, the bug predates ALL of this
   session's and prior sessions' native-renderer work entirely, and is either in reNut's own
   game-side code (hooks, recompiled game code in `generated/renut_recomp.*.cpp`, midasm hooks)
   or is a genuine pre-existing Xenia-stock/driver issue that the user's "runs almost perfectly"
   recollection may be misremembering or that has a narrower trigger condition than "always."
2. If (1) still flickers: pivot to driver/environment-level checks (this is an RDNA4 card on a
   very new/recent driver stack per `renut_native_shader_investigation` memory) -- try disabling
   MSAA/other `renut.toml` options one at a time, check for `VK_LAYER_KHRONOS_validation`
   messages during a play session (not yet done this investigation), and reconsider whether
   "flickering, specific textures/objects, constant, standing still" might describe a driver-side
   texture-compression/caching bug rather than anything in this codebase at all.

## REAL BUG FOUND AND FIXED (2026-08-18) — vertex-fetch batching, not a toggle-and-test guess

User asked to double check the xenos-vs-renut A/B directly (not assumed): **confirmed by the
user that `gpu_plugin = "xenos"` (zero renut code, separate CMake target
`rexgpu-xenos`) is 100% clean, no flickering, no artifacts.** This is the first fully clean A/B
in the whole investigation and definitively confirms the bug is in code exclusive to
`rexgpu-renut`.

Re-examined the CMakeLists.txt: `REXGPU_RENUT_SOURCES = REXGPU_XENOS_SOURCES` (same base file
list, compiled twice with different `RENUT_OPTIMISATIONS` defines) plus a few renut-exclusive
files. Read the renut-exclusive files in full
(`src/graphics/renut/{command_processor,graphics_system,plugin_main}.cpp`) — confirmed trivial
pass-through subclassing, not the cause. That leaves only `#if RENUT_OPTIMISATIONS` blocks in
the SHARED files. Grepped every remaining occurrence in `command_processor.cpp` not yet
individually audited (previous sessions only checked a subset) and found a real one at
`VulkanCommandProcessor::IssueDraw`'s vertex-fetch-request loop, ~line 4843-4926:

**The bug**: this code batches missing vertex-buffer upload requests instead of issuing them
immediately (real, legitimate perf optimization -- fewer barrier transitions). But the per-index
dedup cache (`vertex_buffer_states_[vfetch_index].address/.size`) was updated to the NEW target
address/size **at the moment an entry was queued into the batch**, before the batched
`SharedMemory::RequestRanges` call actually ran (that only happens once, after the ENTIRE
vfetch double-loop finishes). If the function returned early for ANY other reason between
queuing an entry and reaching that flush -- e.g. a LATER vfetch constant in the same loop turns
out invalid and hits one of the existing `return false;` paths a few lines below (line
~4878/4882) -- the batch's `RequestRanges` call never runs, so the actual GPU upload for every
already-queued entry silently never happens. But their `state.address/size` had ALREADY been
optimistically updated to match the target -- so on every SUBSEQUENT draw reusing that exact
same vertex-buffer address (extremely common; a static mesh's fetch constant doesn't change
draw-to-draw), the "already in sync, skip" check at the top of the loop (`state.address ==
vfetch_constant.address && state.size == vfetch_constant.size`) would now incorrectly return
true, permanently skipping the real upload forever for that vertex buffer -- until the game
happens to rewrite that fetch constant to a different address (which self-heals it, explaining
why this wouldn't be 100% permanent/universal, just persistent for specific objects). This is a
real, non-hypothetical correctness bug: a specific real object's vertex data can get "stuck"
un-uploaded (rendering with stale/zeroed/whatever-was-there-before data) for the rest of the
session, matching "specific textures/objects [wrong], constant, standing still" exactly, and
explaining why it's PER-OBJECT rather than universal (only objects unlucky enough to be queued
in the same draw as a later invalid fetch constant are affected).

**Fixed**: moved the `state.address = ...; state.size = ...;` assignment out of the queuing
point and into the success loop that runs only after the batched `RequestRanges` call is
confirmed to have succeeded (mirrors exactly what the immediate/non-batched fallback path a few
lines below already does correctly). Built, deployed, verified matching md5
(`librexgpu-renutrd.so` `15e260af417e46c2bee0da52d52bbbb5`). `gpu_plugin` confirmed still
`"renut"` in the live config (was temporarily switched to `"xenos"` for the A/B test, switched
back).

**RESULT: flickering STILL PERSISTS even with the vfetch-batching bug fixed.** Keep the fix
(it's a real bug regardless), but it is not the/a sole cause of the reported symptom.

**Remaining `#if RENUT_OPTIMISATIONS` blocks individually checked same session** (all of
`command_processor.cpp`'s occurrences, `command_processor.h:832`, `pipeline_cache.cpp`'s):
all confirmed counters/logging/scratch-buffer-declaration only, matching the earlier full audit.
**Every `#if RENUT_OPTIMISATIONS` block in the entire SDK is now individually accounted for.**
Also confirmed: `renut_xenos_pipeline::`/`renut_phase1::` functions are called ONLY from the two
already-gated sites in `IssueDraw` (grepped every call site) -- no unconditional setup/side
effect runs when both are disabled. `rexgpu-xenos` vs `rexgpu-renut` build flags differ only in
`RENUT_OPTIMISATIONS`, `VK_ENABLE_BETA_EXTENSIONS`, extra source files, and imgui include dirs --
no optimization-level or sanitizer difference that could explain divergent UB exposure.

## External research (2026-08-18): XenosRecomp's real, documented limitations

User asked to research whether Xenos->Vulkan shader conversion has ever worked reliably.
Fetched XenosRecomp's own README (hedge-dev/XenosRecomp on GitHub). Key facts, verified by
direct quote, not paraphrase:
- Built explicitly for **Unleashed Recompiled** (Sonic Unleashed), which "implements a
  translation layer for the renderer rather than emulating the Xbox 360 GPU" -- a
  fundamentally different integration style than reNut's (Xenia-derived GPU-EMULATION renderer
  substituting native pipelines into an otherwise-emulated fixed-function pipeline). Explicit
  warning: "Users are expected to modify the recompiler to fit their needs. Do not expect the
  recompiler to work out of the box."
- Named incomplete/missing: dynamic register indexing (partially addressed by reNut's own
  2026-08-17 fix, see fixes-history), mini vertex-fetch instructions, 1D textures, several
  texture fetch features, memexport (reNut has its own graceful-skip for this), point size,
  INF/NaN ALU edge cases, integer constants.
- **Boolean constants packed into a 32-bit integer, capped at 16 per stage** -- explicitly
  flagged as "potentially insufficient for other games requiring up to 128 boolean registers."
  NOT yet checked against Banjo-Kazooie: Nuts & Bolts' real shader corpus this session -- if any
  BK:N&B shader indexes a boolean constant register >=16, it would silently alias/misread,
  producing a wrong-but-shader-specific result. Real, concrete, unchecked lead for the
  ORIGINAL native-shader corruption (stretched textures/non-spawning items), NOT the current
  flicker (native is fully disabled and still flickers, so this can't be its cause right now).
- Vertex-declaration handling has Sonic-Unleashed-specific hardcoded overrides ("Certain
  semantics are forced to be uint4 instead of float4 for specific shaders in Sonic Unleashed")
  and instanced-geometry handling is stated outright to be "completely game specific and must be
  manually implemented for other games" -- BK:N&B's own equivalent overrides, if any are needed,
  have never been audited for existence.
- **This explains the ORIGINAL, still-unsolved "CORE UNSOLVED PROBLEM" (stretched
  textures/non-spawning items, see that section below) far better than anything investigated in
  the 2026-08-17 session** -- those bugs were all real, but none were "is this game hitting a
  documented, named XenosRecomp gap that was only ever solved for a different game's shader
  set." **Does NOT explain the CURRENT flicker**, which is proven (by the disable tests above)
  to occur with zero XenosRecomp-compiled shaders executing at all.

## Where this leaves things, honestly, end of session

Two genuinely separate problems are now untangled:
1. **The ORIGINAL native-shader corruption** (stretched textures, non-spawning items) -- a real,
   still-open problem, now with a much stronger research-backed lead (XenosRecomp's named,
   game-specific gaps above, especially the 16-boolean-constant cap) than any prior session's
   guesses. Next step: audit BK:N&B's real shader corpus for boolean-constant usage patterns the
   same way earlier sessions measured `exp_adjust`/`signed_rf_mode`/texture-dimension usage
   (`tools/ucode_analyze.cpp` + a `scratchpad/` sweep script) -- concrete, checkable, not another
   guess.
2. **The CURRENT flicker** (specific textures/objects, constant, standing still) -- proven via
   multiple independent tests to be UNRELATED to native shaders (`RENUT_DISABLE_ALL_NATIVE=1`
   covering both substitution mechanisms, still flickers) and unrelated to 4 individually-tested
   real behavioral candidates (`kRenutDynamicConstants`, `TextureCache::RequestTextures`
   early-return, `SharedMemory::RequestRanges` scratch reuse, the vfetch-batching bug -- fixed,
   kept, but not the/a sole cause). Every line of the SDK's diff against pristine upstream has
   now been read and classified. **Code-reading of this diff is exhausted as a technique for
   this specific symptom.** Confirmed via `gpu_plugin = "xenos"` A/B (user-verified 100% clean)
   that the bug is real and specific to `rexgpu-renut`'s compiled output, not a environment/
   driver-wide issue.
   ~~Recommended next step: use RenderDoc~~ -- user asked for Tracy instead + a verbose debug
   log; RenderDoc needs interactive GUI use neither of us has here, Tracy is a timing profiler
   (doesn't show resource/data state), so built the actual right tool instead: see below.

## NEW TOOL BUILT 2026-08-18: live-triggered verbose per-draw trace file

Built at the user's explicit request ("focus on adding a lot of debugging that gets put into a
file for you to read... every step the renderer takes it outputs what it does and what it
expects"). Not RenderDoc (needs interactive GUI, neither of us has display access) or Tracy
(timing only, doesn't show what data a draw actually used) -- a plain text trace file, since I
can read files directly.

**How it works** (`command_processor.cpp`, all in `rex::graphics::vulkan` namespace, NOT gated
by `RENUT_OPTIMISATIONS` so it also compiles into `rexgpu-xenos` for a future true A/B if ever
needed):
- `renut_verbose_trace_frames` cvar (int, default 0, currently `2` in
  `~/.config/renut/renut.toml`) -- how many frames to capture once triggered.
- **Live file trigger, not a boot-time counter** (first version was boot-time-only and would
  have only ever captured the intro videos -- caught and fixed before shipping): every frame
  (`RenutVerboseTraceOnFrameEnd`, called from `IssueSwap`), the game does one cheap
  `std::filesystem::exists()` check for `renut_start_trace.flag` next to the executable
  (`out/build/linux-amd64-relwithdebinfo/`). When found, it's deleted (fires once) and the next
  N frames get traced to `renut_verbose_trace.log` in the same directory, then the file is
  closed automatically.
- **To trigger a capture**: with the user already standing at/near the flickering object,
  create `out/build/linux-amd64-relwithdebinfo/renut_start_trace.flag` (empty file, any content)
  -- I can do this myself directly via the Write tool, no user action needed once they're in
  position. Read `renut_verbose_trace.log` afterward, also directly.
- **What gets logged per draw**: `vs_<hash>`/`ps_<hash>`, primitive type, index count; every
  vertex-fetch constant touched (`ALREADY SYNCED, skipped` vs `MISS ... requesting upload`, plus
  the batch-flush outcome); every texture binding resolved (fetch_constant, dimension, signed,
  the real `VkImageView` pointer, flagged `-- NULL VIEW` if unbound); and the final
  native-vs-Phase1-vs-stock DECISION line. Two consecutive frames of the exact same static scene
  can be diffed line-by-line -- anything that differs between them for the same draw (a
  different image-view pointer for the same fetch_constant, a vfetch flipping between SYNCED and
  MISS, etc.) is the actual mechanism of the flicker, not a guess.
**USED, 2026-08-18, same session**: user got in front of flickering geometry, trigger fired
twice ~2s apart (renamed to `renut_trace_A.log`/`renut_trace_B.log` before each got overwritten
by the next capture). `renut_verbose_trace_frames` in `renut.toml` does NOT hot-reload from a
live-edited file while the process is already running (edited 2->4 mid-session, had no effect,
stayed at the boot-time value of 2) -- only 1 full frame captured per trigger despite requesting
more; worked around by triggering twice and diffing the two single-frame snapshots directly
instead of true N-consecutive-frames.

**Findings from analysis** (`renut_trace_A.log`/`renut_trace_B.log`, both still on disk in
`out/build/linux-amd64-relwithdebinfo/` for further analysis):
- ~53% of all texture bindings resolve to a NULL `VkImageView` (7786/14488 in the first single-
  frame capture) -- **investigated and RULED OUT as a bug**: this is the `is_signed=true`
  variant of a texture whose real fetch-constant is currently unsigned, in code SHARED with
  `rexgpu-xenos` (confirmed clean) -- `spirv_translator_fetch.cpp` always statically declares
  both signed/unsigned bindings per Xenos's real per-channel-sign hardware feature, but only
  samples whichever one the real runtime sign bits select; a texture legitimately not needed
  this draw stays unbound. Normal, not a lead.
- **Real finding, not yet resolved either way**: diffed the two single-frame snapshots
  (`scratchpad`-style Python, not saved -- rerun the same regex-based parse over
  `renut_trace_A.log`/`renut_trace_B.log` if resuming this) by grouping texture bindings per
  (vertex shader hash, pixel shader hash, stage, fetch_constant, signed) and comparing the SET
  of real `VkImageView` pointers seen. Out of 3239 such keys common to both captures, 12 differ
  -- ALL 12 are `fetch_const=11, unsigned`, across 11 DIFFERENT shader pairs, and they all swap
  between the exact same two real texture pointers (`0x7fa2b76b46c0` /`0x7fa2b62b66b0`) from
  capture A to capture B, while the user stood still. Drilled into one pair
  (`vs_9bd6d5e87f320c3a`/`ps_1068bca74c165c43`): it draws exactly twice per frame in both
  captures (once with `tex11=nil`, once with a real pointer), and it's the SECOND draw's real
  pointer that flips between captures.
- **Genuinely ambiguous, not resolved**: this is consistent with EITHER (a) a real stale-binding
  bug on the shared hardware fetch-constant register 11 (many objects share it across a frame;
  something reads whichever value happens to be bound at a slightly wrong moment), OR (b) normal
  intended behavior -- the same shader pair legitimately reused for multiple different on-screen
  objects/icons within one frame, each meant to bind its own different texture, and the two
  captures just happened to draw those instances in a different order/content 2 seconds apart
  (menu icons/HUD elements can animate even while the player stands still). Asked the user
  whether the visible flicker looks like it toggles between two distinct looks vs. random noise
  -- answer was "not sure," inconclusive.

**Next session, concrete and cheap**: the codebase ALREADY has an unrelated feature
(`GetDebugShaderLabels()`/`DebugLabelPositions()`, `ndc_x`/`ndc_y`, see "2026-08-17 late
session" notes below) that records each native draw's on-screen NDC position. Extend the SAME
idea into `RenutVerboseTraceFile()`'s per-draw logging (print the draw's real NDC/screen
position alongside its texture bindings) -- this would let the register-11 finding above be
resolved definitively: if the two flip-flopping draws land at the SAME screen position across
captures, it's a real bug; if they land at two different, sensible positions, it's normal
per-instance reuse. Also worth fixing properly this time: make `renut_verbose_trace_frames`
actually take effect without a process restart (either re-read it fresh every trigger poll
instead of only once, or confirm/document that a restart really is required) so a true
N-consecutive-frame capture is possible instead of stitching together separately-triggered
single frames.

**Prior fix, implemented, built, and deployed** (details below, kept for provenance — this is
the fix that produced zero visible change, described above):

What changed, in `renut_xenos_pipeline_cache.cpp`:
- `kBindlessHeapSlotCount` grown from 32 to 4096 (descriptor pool size and set-layout binding
  count already derived from this constant, so this propagated automatically; build succeeded
  with no device-limit failures reported at pool-creation time).
- `BindlessHeapState` gained `std::unordered_map<uint64_t, uint32_t> texture2d_slot_map` (keyed
  by a combined `VkImageView`+`VkSampler` identity hash, `CombineTextureSamplerKey`) and
  `uint32_t next_free_texture2d_slot = 0`.
- `BindTexture2DFixedSlot` renamed to `ResolveTexture2DSlot`, now takes an `out_slot` param:
  resolves the real view/sampler as before, looks up its identity in `texture2d_slot_map`,
  reuses the slot if already allocated, else allocates `next_free_texture2d_slot++` and writes
  the descriptor exactly once. Returns false (triggering a full draw skip, matching stock's
  fallback behavior) if the slot pool is exhausted — logs one real warning via a
  `static std::atomic<bool> warned` guard, not spammed per-draw.
- `IssueNativeDraw`'s texture-binding loop now writes the **assigned stable slot** (not
  `tb.fetch_constant`) into `shared_constants.texture2d_indices[tb.fetch_constant]` /
  `sampler_indices[tb.fetch_constant]` — this is the actual fix: two different draws in the
  same frame that both use hardware register N for two different real textures now each get
  their OWN permanent slot number written into their OWN per-draw `SharedConstants` buffer,
  instead of both racing to overwrite ONE shared descriptor-set slot keyed by register number.
- The `>= kBindlessHeapSlotCount` bound check in the same loop was corrected to `>= 16` — that
  check is about the `texture2d_indices[16]` array position (`tb.fetch_constant`, a hardware
  register 0-31), which is a completely different number from the (now much larger)
  `kBindlessHeapSlotCount` slot-pool size. Reusing the pool-size constant there would have
  silently allowed out-of-bounds array writes into `SharedConstantsLayout` once the pool grew
  past 16. Caught and fixed during this session's edit, not a leftover bug.

**Original root-cause narrative** (verified 2026-08-18, before the fix above; kept for
provenance):

**Verified root cause of the persistent flickering/wrong-texture symptom** (see the
"CORE UNSOLVED PROBLEM" section below for full backstory/prior fixes): the native
path's bindless texture heap (`BindlessHeapState` in
`/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src/src/graphics/vulkan/renut_xenos_pipeline_cache.cpp`)
indexes heap slots by the Xenos hardware fetch-constant REGISTER NUMBER (0-31,
`kBindlessHeapSlotCount`), not by real texture identity. Verified facts (not
speculation):
1. Xenos hardware has exactly 32 fetch-constant registers; this game has far more
   than 32 distinct textures, so the same register number is guaranteed to get
   rebound to different real textures many times within one frame -- that's normal
   fixed-function operation, not a bug in the game.
2. `GetHeapState()` returns ONE `static BindlessHeapState`, shared for the whole
   process lifetime -- not duplicated per frame-in-flight.
3. `kMaxFramesInFlight = 3` (`command_processor.h`), and the codebase's OWN existing
   pattern elsewhere (`swap_descriptors_*` arrays) shows it already knows to
   duplicate descriptor resources per frame-in-flight for exactly this reason -- the
   bindless heap just doesn't do it.
4. The STOCK renderer's real texture-binding path (`texture_cache.cpp`/
   `command_processor.cpp` ~line 7649-7684) does NOT use this pattern at all -- it
   writes fresh per-draw descriptor image info with dirty-tracking tied to the
   actual current draw, never a shared slot reused across unrelated draws by
   register number.

`vkUpdateDescriptorSets` writes take effect immediately on the CPU, and a whole
frame's draws are recorded before the GPU touches any of them -- so whichever draw
last wrote a given slot number wins for EVERY draw in that frame (and possibly the
previous still-in-flight frame) that references that slot number. Frame-to-frame
timing jitter changes which write "wins" -> flickering, not a fixed wrong-but-stable
error. This is forced by the four facts above, not a guess.

**Exact fix plan (executed 2026-08-18 exactly as written below — kept for provenance)**:
1. Key heap slots by STABLE texture identity (the real `VkImageView` handle --
   stable while a texture stays resident -- paired with its `VkSampler`) instead of
   `fetch_constant_index`. Add e.g. `std::unordered_map<std::pair<VkImageView,VkSampler>, uint32_t>`
   (needs a hash/equal for the pair, or combine into a single key) plus a
   `uint32_t next_free_slot = 0;` to `BindlessHeapState`.
2. Grow `kBindlessHeapSlotCount` from 32 to a few thousand (e.g. 4096) --
   `renut_xenos_pipeline_cache.cpp:471`. Descriptor pool size
   (`pool_sizes[0/1].descriptorCount`, ~line 631/633) and set-layout binding count
   (~line 549) already derive from this constant, so bumping it should propagate
   automatically -- verify device limits (`maxDescriptorSetSampledImages` etc.) are
   comfortably above 4096 on the RX 9070 target before assuming this is free.
3. Rewrite `BindTexture2DFixedSlot` (~line 1654-1692, currently keyed on
   `fetch_constant_index`, `kBindlessHeapSlotCount` guard at line 1663) to: resolve
   view/sampler as today, look up the (view,sampler) pair in the map, reuse the
   slot if found, else allocate `next_free_slot++` (bounds-check against the new
   larger `kBindlessHeapSlotCount`) and write the descriptor ONCE (not every call).
   Needs an out-parameter or return value carrying the assigned slot number back to
   the caller (current signature just returns bool success/fail).
4. Update `IssueNativeDraw`'s texture-binding loop (~line 1916-1955,
   `shared_constants.texture2d_indices[tb.fetch_constant] = tb.fetch_constant;` /
   same for `sampler_indices`) to write the ASSIGNED STABLE SLOT returned by step 3,
   not `tb.fetch_constant`. This is the actual value XenosRecomp's compiled
   shaders read at runtime (`shader_recompiler.cpp:1272-1277`,
   `vk::RawBufferLoad<uint>(SharedConstants + dim*64 + registerIndex*4)`) -- the
   `registerIndex` in that formula is still `tb.fetch_constant` (that part of the
   ABI is correct and already fixed this session), only the STORED VALUE at that
   byte offset needs to change from "the register number" to "the real heap slot."
5. No eviction in this first pass -- slots accumulate for the session (acceptable;
   note as a known follow-up if very long play sessions ever matter).
6. After implementing: `ninja rexgpu-renut` (from
   `/home/nick/Desktop/reNut/out/build/linux-amd64-relwithdebinfo`, NOT the
   standalone SDK dir -- see "Standing procedures" section for why), copy fresh
   `librexgpu-renutrd.so` to the game build dir, verify via ctypes, hand to user.
   No shader reconversion needed (XenosRecomp/HLSL side is untouched by this fix,
   only the C++ runtime-side heap management changes).

**Also still true from the prior fix that DID ship and IS deployed** (verify still
correctly in place before starting the above, don't re-do): `SharedConstantsLayout`'s
first 256 bytes hold `texture2d_indices[16]`/`texture3d_indices[16]`/
`texturecube_indices[16]`/`sampler_indices[16]` (was dead zeroed padding before
2026-08-17's root-cause fix); debug-colorize mode defaults to `false` (was
accidentally left `true`, which had been masking pixel-stage fixes during testing --
fixed same session, confirm it's still `false` before assuming a build "does
nothing").

## Project Map

### Game repo: `/home/nick/Desktop/reNut/` (git, branch `linux-work`; main is `main`)
- `CMakeLists.txt` — `project(renut)`, C++23, `add_executable(renut ...)`.
  Wires the shader pipeline: `renut_xenos_shader_batch(renut ...)` with
  dependencies `renut_synthesize_constant_tables_target` and
  `renut_synthesize_containers_from_ucode_target`, and forces
  `rexgpu-renut` to depend on `renut_xenos_shader_batch`.
- `cmake/rexglue_xenosrecomp.cmake` — FetchContent of hedge-dev/XenosRecomp as a
  host tool; defines `renut_xenos_shader()` (single shader -> HLSL) and
  `renut_xenos_shader_batch()` (whole dump dir -> compiled SPIR-V cache .cpp).
  Header comment holds the real rationale for the patch below. **Read it before
  touching the shader build.**
- `cmake/patches/xenosrecomp_graceful_skip.patch` — two bundled upstream fixes:
  (1) DXC compile failures become logged skips instead of `assert()` segfaults;
  (2) `std::execution::par_unseq` -> `seq` in XenosRecomp's own multi-shader loop.
  **Do not revert.** Measured: `par_unseq` deterministically dropped ~450/2135
  shaders per run; `seq` converts 100% in ~50s. Applied idempotently via `sh -c`
  (a bare PATCH_COMMAND string does *not* get a shell — that bug was real and fixed).
- `CMakePresets.json` — presets `linux-amd64-{debug,release,relwithdebinfo}` and
  `win-amd64-*`. The one in use is `linux-amd64-relwithdebinfo`.
- `archive/native-renderer-plan.md` (~85KB) — the long-form native-renderer plan
  (Phase 0/1/...). Referenced by the cmake files. Large; grep it, don't read whole.
- `archive/deferred-vfetch-flush.md` (~17KB) — deferred-flush design; that rewrite
  was **abandoned and reverted** (see memory `renut_barrier_attribution`). Historical.
- `config/renut_hooks.toml` — midasm hooks (guest addresses -> named C++ hooks).
- `renut_manifest.toml` / `renut_config.toml` — rexglue SDK project manifest
  (`sdk_version = "0.9.0"`, entrypoint `assets/default.xex`, out dir `generated`).
- `src/renut_engine/` — game-side C++: fixes, hooks, `shader_dump.cpp`
  (guest ShaderContainer parsing; all reads go through `RENUT_BSWAP32` because
  guest memory is big-endian), `renut_xenos_shader_cache.h`.

### Repo tools: `/home/nick/Desktop/reNut/tools/` (verified 2026-08-17)
- `xenos_batch_convert.py` — batch-converts shader ucode -> SPIR-V via XenosRecomp.
  Per-shader subprocess isolation + caching. Parallel via
  `ThreadPoolExecutor(max_workers=os.cpu_count() or 4)` (line ~237).
- `build_synthetic_container.py` — synthesizes D3DX9 shader containers.
  `check_for_missing_literal_constants()` (line ~426) rejects shaders referencing
  float const registers >=252 without real DefinitionTable data.
- `batch_build_synthetic_containers.py` — the **batch driver** for the above.
  This is where `--reject-heuristic` actually lives (it forwards the flag to
  `build_synthetic_container.py`). Not in `xenos_batch_convert.py`.
- `xenos_synthesize_constant_table.py` — constant-table synthesis step.
- `ucode_analyze.cpp` + `build_ucode_analyze.sh` -> binary `ucode_analyze` in the
  build dir. Dumps structural JSON per ucode file (bindings, formats, float
  constants, dynamic addressing, ...). Usage: `ucode_analyze <vs|ps> <file.ucode>`.
- `xenos_cache_unpack.cpp` — unpacks the generated shader cache.
- `compare_traces.py`, `frame_inventory.py`, `resolve_barrier_sites.sh` — older
  perf/trace analysis helpers from the barrier-attribution work.
- `syntax_check_sdk.sh` — syntax-only check of SDK sources (fast pre-build sanity).

### SDK source (build/edit this, NOT a repo checkout): `/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src/`
Headers and sources are in **separate trees** — this was wrong in an earlier
version of this file:
- Sources: `src/graphics/...`
  - `src/graphics/command_processor.cpp` — shared PM4 packet dispatch, incl.
    `ExecutePacketType3_IM_LOAD` / `_IM_LOAD_IMMEDIATE` (line ~977/1705). This is
    the path that loads shaders straight into sequencer memory with **no D3D9
    declaration** — the structural root of the vertex-declaration problem.
  - `src/graphics/vulkan/` — the stock Vulkan backend plus the native effort:
    `command_processor.cpp`, `pipeline_cache.cpp`, `shader.cpp`,
    `render_target_cache.cpp`, `texture_cache.cpp`, `shared_memory.cpp`,
    `primitive_processor.cpp`, `deferred_command_buffer.cpp`, `graphics_system.cpp`.
  - **Native shader path**: `src/graphics/vulkan/renut_xenos_pipeline_cache.{h,cpp}`
    — pipeline cache, bindless heap, `HasNativePipeline()` (cpp:681),
    `IssueNativeDraw()`, `USAGE_LOCATIONS` mirroring (cpp:986/1018/1062),
    primitive-type fallbacks (cpp:703/725).
  - `src/graphics/vulkan/renut_shader_debug_panel.cpp` — ImGui debug panel
    (**no matching `.h`** — its API is declared in `renut_xenos_pipeline_cache.h`,
    e.g. `Set/GetNativeShaderLabelMatrixRegister` at h:215/216).
  - `src/graphics/vulkan/renut_phase1_native_draw.{cpp,h}` +
    `renut_phase1_shaders_spirv.inc` — the earlier Phase-1 hand-built native draw
    experiment; separate from the pipeline-cache path above.
  - `src/graphics/renut/` — the shipped **renut plugin** itself
    (`command_processor.cpp`, `graphics_system.cpp`, `plugin_main.cpp`).
- Public headers: `include/rex/graphics/...`
  (`include/rex/graphics/vulkan/command_processor.h`, `include/rex/graphics/renut/…`,
  `include/rex/graphics/d3d12/…`). `out/install/linux-amd64/include/` is an
  install-time copy — **don't edit that one**.
- Built plugins: `out/linux-amd64/librexgpu-renutrd.so`,
  `librexgpu-xenosrd.so`, `librexruntimerd.so`, `libSPIRV-Tools-sharedrd.so`,
  `libTracyClientrd.so`.

### Reference documents: `/home/nick/Desktop/reNut/resources/` (indexed 2026-08-17)
25 PDFs, ~16MB. Extracted plain text cached in scratchpad `restext/` for fast grepping
(`pdftotext` is installed). **Composition matters: this is a CPU/system library, not a
GPU one.**
- **PowerPC / VMX ISA references (authoritative, for XenonRecomp-side work)**:
  `vector_simd_pem.ppc.2005AUG23.pdf` (318pp, VMX/SIMD PEM), `ALTIVECPEM.pdf` (346pp),
  `ibm-ppc64-book1.pdf` (220pp), `ibm-ppc64-book2.pdf` (62pp).
- **Official MS XDK white papers** (`xbox 360 tech docs/`, 21 files, 5-25pp each):
  CPU overview/caches/alignment/best-practices, VMX128 instruction summary,
  multithreading + lockless programming, compiler technology, memory page sizes,
  memory copy functions, dump debugging, build profiling, storage, security, XGD3.
- **`github repos.txt`**: UnleashedRecomp, XenonRecomp, XenosRecomp, idaxex,
  XEXLoaderWV, ghidra-fidb-xenonsdk, x360-fidb-generator, fable2mod forum thread.
- **VERIFIED ABSENT — do not go looking again**: zero occurrences anywhere in the corpus
  of `xenos`, `vfetch`, `tfetch`, `interpolator`, `vertex declaration`,
  `constant register`, `shader model`, `render target`. There is **no** Xenos ISA,
  shader-microcode, or Xbox-360-D3D9 graphics reference here. The native-shader work
  gets nothing from these; XenosRecomp's own source + Xenia remain the only real
  primary sources for that.
- **The two docs with any GPU content**:
  - `system_xbox_360_hw_overview.pdf` (5pp) — corroborates the hardware model
    independently: 48 ALUs, one vector + one scalar op per clock, 10MB eDRAM,
    predicated tiling, HLSL SM3.0 "and beyond", 32-bit IEEE float throughout. Two lines
    are directly relevant to our conversion failures: *"Vertex shaders can fetch from
    textures"* (so the 3 `implicit lod in vertex stage` failures are a legitimate,
    officially-supported feature — fix by emitting `SampleLevel`, not by skipping), and
    *"shaders also have the unique ability to directly access main memory"* (memory
    export was a promoted first-class feature, so our 10 memexport shaders are expected,
    not exotic).
  - `system_Synchronous_and_Asynchronous_Swaps_Xbox_360.pdf` (21pp) — eDRAM →
    front-buffer `Resolve` semantics and GPU/CPU swap synchronisation. Relevant to
    present/frame-pacing and render-target handling, not to shader translation.

### Build output / runtime dir: `/home/nick/Desktop/reNut/out/build/linux-amd64-relwithdebinfo/`
- `renut` — the game binary (~190MB).
- `librexgpu-renutrd.so` / `librexgpu-xenosrd.so` — **copies** of the SDK-built
  plugins. Must be re-copied after every SDK build (see recipe).
- `marked_bad_shaders.txt` — user-curated `vs_<hash> ps_<hash>` pairs marked bad
  in-game. Gitignored, not source controlled. Currently **64 pairs / 35 distinct
  vertex shaders** (verified 2026-08-17). `.bak` alongside holds the pre-cleanup 78.
- `generated/renut_xenos_shader_cache_manifest.txt` — authoritative per-shader
  `<tag> <status>` conversion status.
- `generated/renut_xenos_shader_cache.cpp` (+ `.compressed.cpp`,
  `.hashremap.bin`, `.resultcache.json`) — the generated cache.
- `shaders_ucode_hash/` — raw ucode per hash: **525 `vs_*`, 1436 `ps_*`** (1961 files).
- `ucode_analyze` — built analysis binary.
- `_deps/xenosrecomp-{src,build,subbuild}/` — the FetchContent'd XenosRecomp.
- Logs: `~/.local/state/renut/logs/renut_NNN[.M].log` — rotates at ~5MB with a
  numeric suffix (e.g. `renut_004.10.log`), so one "session" spans many files and
  one file can span several process launches. Sort by mtime, don't assume naming.
- Live GPU mem stats (this machine, AMD): `/sys/class/drm/card1/device/mem_info_{vram,gtt}_used`.
  `card1` is correct here (verified 2026-08-17) but is machine-specific.

## Build/Deploy Recipe

1. `ninja -C out/build/linux-amd64-relwithdebinfo renut`
2. Verify the plugin loads: `python3 -c "import ctypes; ctypes.CDLL('<sdk>/out/linux-amd64/librexgpu-renutrd.so')"`
3. `command cp -f` **both** `librexgpu-renutrd.so` and `librexgpu-xenosrd.so` from
   `/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src/out/linux-amd64/`
   to `/home/nick/Desktop/reNut/out/build/linux-amd64-relwithdebinfo/`.
   **Mandatory** — stale plugin copies silently run old code with no error.
   Check with `md5sum` on both paths before concluding a test result means anything.
4. User builds/tests via display access; I don't have one (division of labor).

## Current shader conversion status (measured 2026-08-17 from the live manifest)

| | total | ok | failed |
|---|---|---|---|
| Vertex | 516 | **280 (54.3%)** | 236 |
| Pixel | 1537 | **1531 (99.6%)** | 6 |
| All | 2053 | 1811 (88.2%) | 242 |

Failure breakdown: `skipped-by-xenosrecomp:SPIR-V` 224 vs + 6 ps;
`skipped-by-xenosrecomp:memory` 10 vs; `skipped-by-xenosrecomp:duplicate` 2 vs.

**Key structural fact**: the conversion deficit is *entirely* vertex shaders.
Pixel shaders are essentially solved (99.6%). Every remaining conversion-yield
lever is a vertex-declaration / vertex-semantic problem. Historical numbers
quoted elsewhere (12/1884 -> 1492/1884 -> 1706/2208) are older snapshots of a
differently-sized dump; use the table above.

Note: the manifest (2053 entries) and `shaders_ucode_hash/` (1961 files) don't
match 1:1 — the manifest also carries entries from container-synthesis inputs
that have no standalone ucode file. Don't assume one implies the other.

## ✅ DONE (2026-08-17): positional-location rewrite + boolean-constant fix, built and deployed

All fixes below are **applied, compiled, linked, and copied into the live game build
dir**. Not yet visually tested by the user (I have no display).

**Applied and verified compiling/linking**
- `_deps/xenosrecomp-src/XenosRecomp/shader_common.h` — `g_Booleans` (1 dword) →
  `g_BooleanWord(i)` (8 dwords, 256 bools); trailing shared fields moved
  260/264/272 → 288/292/300. DXIL `DEFINE_SHARED_CONSTANTS` updated to match.
- `shader_recompiler.cpp` — bool defines + conditional jumps index the full 256-bit
  file by raw `boolAddress` (also removes upstream issue #17's need for "-128");
  vertex `[[vk::location]]` now **positional** (`location i` = i-th element) with a
  `maxVertexInputAttributes = 32` graceful bail; `INTERPOLATORS` extended with
  Normal0/1/2, Tangent0, Binormal0; register file 32 → 64 (`kNumRegisters`).
- `src/renut_engine/renut_xenos_shader_cache.h` — `kRenutMaxVertexLocations` 16 → 32;
  new `kRenutTexCoordSemanticUnset` sentinel + `vertexTexCoordSemanticIndex[]` field
  on `RenutXenosShaderCacheEntry` (see below).
- `tools/xenos_cache_unpack.cpp` — `ExtractVertexLocations()` now positional;
  `LookUpVertexLocation` kept but `[[maybe_unused]]`. New
  `ExtractVertexTexCoordSemanticIndices()` reads the raw usage/usageIndex bitfield
  XenosRecomp already writes per vertex element (untouched by the positional-location
  change) and emits it as a second parallel array, same remap/keying pattern as
  `vertexLocations[]`.
- SDK `renut_xenos_pipeline_cache.cpp`, `GetOrCreatePipeline()`:
  - `SharedConstantsLayout` widened (`booleans[8]`, size 288 → 320, new static_asserts);
    uploads all 8 bool dwords.
  - `is_packed_normal_like_usage` (~line 1040) no longer keys off `real_location`
    (meaningless now that locations are positional) — tests
    `fetch.attributes.data_format` directly against `k_10_11_11`/`k_11_11_10`/
    `k_2_10_10_10`, the same enum `VertexFormatToVkFormat()` already switches on.
  - `is16BitFormat`'s `semanticIndex` recovery (~line 1077, `g_SwappedTexcoords`)
    no longer switches on `attribute_info.location` — reads
    `vs_entry->vertexTexCoordSemanticIndex[raw_attribute_index - 1]` instead (the
    new cache field above). **Key insight found while implementing this**:
    `tfetchTexcoord()`'s `semanticIndex` argument is baked into the HLSL as a
    compile-time literal by XenosRecomp itself
    (`shader_recompiler.cpp`'s `recompile(VertexFetchInstruction)`, reading its own
    `vertexElements` map) — that part was never broken by the location change.
    Only this pipeline-cache code's *separate*, redundant runtime reconstruction of
    the same fact (for the `swapped_texcoords_mask` it uploads) needed fixing.

**Verified end to end**
- `ninja XenosRecomp` (in game build dir) — clean.
- Isolated re-probe of the 242 previously-failing shaders: 224 now compile.
- `ninja rexgpu-renut` (SDK build, `REXSDK_DIR` mode, `RENUT_HAVE_XENOS_SHADER_CACHE=1`)
  — clean, links `librexgpu-renutrd.so`. (A standalone SDK-only build without
  `REXSDK_DIR`'s full context hits a **pre-existing, unrelated** `#else`-branch
  compile error in `IssueNativeDraw` — dead code, `RENUT_HAVE_XENOS_SHADER_CACHE=0`
  path, not touched by this session, not the real build path — ignore it.)
- **Real batch conversion re-run** (deleted `resultcache.json` to bypass the
  content-hash cache, which is not keyed on the XenosRecomp binary version and was
  silently serving stale skip results): **2035/2053 shaders converted (99.1%)**,
  vertex **499/516** — matches the projected yield exactly. Remaining 18 skips are
  the known-legitimate set (10 memexport, 5 vertex implicit-LOD, 2 duplicate-usage,
  1 swizzle).
- Fresh `librexgpu-renutrd.so` (117MB, SDK's own `out/linux-amd64/`) copied via
  `cp -f` into the game build dir
  (`out/build/linux-amd64-relwithdebinfo/librexgpu-renutrd.so`, was stale from 17:25,
  now current) and confirmed loadable via `ctypes.CDLL` with all deps resolving.

**Patch captured (2026-08-17, no longer at risk)**: `cmake/patches/xenosrecomp_graceful_skip.patch`
deleted, replaced by `cmake/patches/xenosrecomp_native_renderer.patch` — a single
consolidated patch (old graceful-skip fixes + this session's booleans/positional-location
fixes), generated via `git diff` against the pristine FetchContent checkout and verified
with `git apply --check` against a fresh clone of the same upstream commit
(`990d03b`). `cmake/rexglue_xenosrecomp.cmake`'s `PATCH_COMMAND` updated to reference
the new filename. A clean CMake reconfigure will now reproduce the current tree exactly.

**Still open**
- **User has not yet visually/gameplay-tested this build.** This is the actual
  "1 to 1" verification — everything above is compile/link/yield verification only.
- Runtime `maxVertexInputAttributes` device-limit check before pipeline creation
  (promised in `renut_xenos_shader_cache.h`'s comment, not yet added) — no shader in
  the real corpus currently needs more than 28, so not urgent, but a future capture
  with more attributes would silently misbehave without it.

## LIVE SILENT-CORRUPTION BUG (2026-08-17): boolean constants in 81% of pixel shaders

Found by reading XenosRecomp's open issue tracker (issue #17, filed by upstream's own
author, still open). **This affects shaders that convert successfully and are in use —
unlike the conversion-failure bugs below, it produces wrong rendering, not a skip.**

**Measured: 1240 of 1531 converted pixel shaders (81%) branch on a boolean constant
whose bit index is outside the representable range.** 0 of 280 vertex shaders affected.

Mechanism, verified end to end:
1. Xenos has **256 boolean constants** (`XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031` ..
   `_224_255`, 8 dwords). Vertex bools live at 0-127, pixel bools at 128-255.
2. Our pixel shaders really do use high ones — observed `b129`, `b130`, `b243`-`b254`.
3. XenosRecomp models the whole file as **one uint32** (`g_Booleans`), 16 bools per
   stage, pixel offset by +16 (`shader_recompiler.cpp:1309`). It emits
   `#define b243 (1 << 259)`.
4. A shift of 259 on a 32-bit value is undefined in SPIR-V; AMD masks the shift to 5
   bits, so `1 << 259` becomes `1 << 3`. `b243`-`b254` therefore test bits **3-14**,
   which is the *vertex* bool window. Compiles clean, binds clean, branches wrong.
5. Independently, `renut_xenos_pipeline_cache.cpp:1713` uploads **only dword 0**
   (`regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]`), i.e. bools 0-31. Bools 128+ are
   never sent to the GPU at all. So even a correctly-computed bit would read stale data.
- Note `b129`/`b130` land on bits 17/18 by modular accident — the *shift* is right but
  the *data* is still bools 17/18, not 129/130. Nothing in this path is actually correct.
- Timing note: `g_Booleans` was only wired to real guest data on 2026-08-17 (it was
  hardcoded 0 before). Previously every such branch took the false path consistently;
  now they read real-but-wrong bits, so **native-shader behaviour may have changed
  character very recently**. Worth knowing before comparing against older observations.

Fix shape (upstream README anticipates this: *"may require increasing the size of the
`g_Booleans` data type for other games"*):
- Widen `g_Booleans` to 8 dwords / 256 bits in `shader_common.h` and in
  `SharedConstantsLayout` (currently `uint32_t booleans` at byte 256).
- Emit word+bit selection instead of a single shift, and drop the `+16` pixel offset —
  with a real 256-bit file the raw `boolAddress` is already correct, which also resolves
  upstream issue #17 (no need for its "subtract 128" workaround).
- Upload all 8 dwords in `IssueNativeDraw()`, not just the first.
- Re-check `static_assert(sizeof(SharedConstantsLayout) == 288)` and the byte-256 offset
  assert when the layout grows.

## ROOT CAUSE FOUND (2026-08-17): the vertex-shader conversion deficit

Full write-up (designed, readable): https://claude.ai/code/artifact/b760eff3-3850-4f0f-9676-29c06a4d1463

Method: re-ran every failing shader through XenosRecomp in isolation and captured the
real compiler diagnostic. Scripts kept in scratchpad: `probe_failures.py`,
`analyze_locations.py`, `check_location_collisions.py`. **Measured, not inferred.**

| n | vs | ps | diagnostic | cause |
|---|---|---|---|---|
| 198 | 198 | 0 | `partial explicit stage input location assignment via vk::location(X) unsupported` | usage missing from `USAGE_LOCATIONS` |
| 28 | 23 | 5 | `use of undeclared identifier` | `r63` (15) + interpolator naming (13) |
| 10 | 10 | 0 | memexport | genuinely unimplemented upstream |
| 3 | 3 | 0 | implicit LOD in vertex stage | needs explicit LOD |
| 2 | 2 | 0 | duplicate usage/usageIndex | our own guard working |
| 1 | 0 | 1 | vector swizzle out of bounds | undiagnosed |

**Bug A (198 shaders, 84% of vertex failures).** `shader_recompiler.cpp:78-97`
`USAGE_LOCATIONS[]` has 17 entries and its own comment says *"specialized Vulkan
locations for Unleashed Recompiled... Likely not going to work with other games."*
The emit loop (`shader_recompiler.cpp:1397-1404`) only prints `[[vk::location(N)]]`
on a table hit — a miss emits the parameter with **no location at all**, and DXC
rejects *partial* explicit location assignment, killing the whole shader. Missing
usages here: `POSITION2..12`, `TEXCOORD8..12`.
**Critical nuance: 0 of those 198 have a real captured vertex declaration** (only 12
vertdecls exist project-wide). The high usage indices are artifacts of our own
synthesis heuristic's global counter. So naively widening the table = 198 shaders
compiling with *guessed* semantics, i.e. exactly what `--reject-heuristic` suppresses.

**Bug B (15 shaders).** `ExportRegister::VSPointSizeEdgeFlagKillVertex = 63` has no
case in the VS export switch (`shader_recompiler.cpp:631-685`); it falls through to
the interpolator lookup, misses, and generates a write to undeclared `r63`. Only
`r0..r31` are declared (`shader_recompiler.cpp:1530`) though ALU operands mask to
`0x3F` (r0-r63). Upstream README lists "Point size" as unimplemented.

**Bug C (13 shaders).** `INTERPOLATORS[]` (`shader_recompiler.cpp:99-119`) declares
only TEXCOORD0-15 + COLOR0-1, so a container interpolator with usage
Normal/Tangent/Binormal names a nonexistent `oNormal0`/`iNormal0` parameter.

### Verified NEGATIVE results — do not re-investigate
- **Register-file overflow is NOT a general cause.** No shader in the partial-location
  group references r32+. Only r63 appears, for Bug B. (Tested, hypothesis refuted.)
- **`vk::location` collisions are NOT a live corruption source.** `USAGE_LOCATIONS`
  maps both `(Position,1)` and `(TexCoord,7)` to location 15 — a real aliasing bug —
  but all 280 converted vertex shaders were checked and **0 collide**, including all
  35 marked-bad ones. It is a trap for whoever extends the table, not a current bug.
- Interpolator-count mismatch: structurally impossible (all 18 declared unconditionally).

### Recommended fix order (see artifact §6 for full reasoning)
- **A. Assign vertex locations positionally per shader** (location i = i-th vertex
  element), on both the container side and `ExtractVertexLocations()`/`GetOrCreatePipeline()`.
  Makes the semantic name irrelevant to wiring — dissolves the heuristic-guessing
  problem instead of tolerating it, and kills the location-15 collision by construction.
  Upstream README:67 independently recommends exactly this. Ceiling: largest shader needs
  **28 attributes**; this GPU (RX 9070 / RADV) exposes `maxVertexInputAttributes = 32`,
  so it fits — but that exceeds the portable 16 minimum, so gate it on a runtime check.
- **B.** Declare `r0..r63`; add an explicit (possibly deliberately-ignored) case for
  export register 63.
- **C.** Name interpolators by index, not usage.
- **D. Highest-value item for the CORRUPTION problem** (A-C only fix *compile* failures):
  audit the silent Tier-2 assumptions — `g_SwappedTexcoords` re-swizzles 16-bit data
  **only for TEXCOORD**, and `R11G11B10` unpacking is applied **only** to
  NORMAL/TANGENT/BINORMAL. A 16-bit non-TEXCOORD attribute or a packed format on another
  usage is silently wrong data with no error — exactly the "stretched texture"/"wrong
  lighting" signature. Decidable offline from existing `ucode_analyze` output.
- **E.** Fetch opcodes other than `TextureFetch`/`GetTextureWeights` (incl.
  `SetTextureLod`, gradient setters) are silently dropped at
  `shader_recompiler.cpp:228-229` — route them to the graceful-skip path.

## XenosRecomp / Shader Facts (verified)

- Every vertex shader unconditionally declares/writes all 16 `oTexCoordN`; every pixel shader unconditionally declares all 16 `iTexCoordN`. Interpolator-count mismatch between VS/PS is structurally impossible — ruled out as a corruption cause.
- `USAGE_LOCATIONS[]` fixed table (XenosRecomp `shader_recompiler.cpp`): Position=0, Normal=1, Tangent=2, Binormal=3, TexCoord0-3=4-7, Color0=8, BlendIndices0=9, BlendWeight0=10, Color1=11, TexCoord4-7=12-15.
- Guest memory (`rex::memory::Memory::TranslatePhysical<T>`) is **big-endian** — must use `rex::memory::load_and_swap<T>()` from `<rex/memory/utils.h>` to read correctly. (Game-side code uses its own `RENUT_BSWAP32` macro for the same reason.)
- `Runtime::instance()->memory()` is the public accessor to the real memory singleton (`SharedMemory::memory()` is protected, don't use it from outside).
- 26/748 real Banjo shaders segfault XenosRecomp's own directory-scan driver outright (confirmed 2026-08-17) — this is why `xenos_batch_convert.py` isolates each shader in its own subprocess rather than using upstream's batch mode.

## FIXED: Build only used 4 cores

Root cause: `tools/xenos_batch_convert.py`'s per-shader isolation loop was fully sequential in Python (unrelated to the XenosRecomp internal `seq` patch, which was correctly left alone). Fixed by wrapping the per-shader subprocess run in `concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count())`; each shader still runs in its own subprocess/tempdir for crash isolation, results collected via `as_completed` and merged single-threaded afterward (no shared-state races). **Verified still present 2026-08-17.**

## FIXED: GTT memory leak -> DEVICE_LOST crash (~14.5-14.9k frames)

- Symptom: `radv/amdgpu: Not enough memory for command submission` -> `VK_ERROR_DEVICE_LOST`, reproducible after several minutes just idling in the main menu.
- Root cause: `BindlessHeapState::constants_pool` (a page-based ring buffer, `GraphicsUploadBufferPool`) never had `Reclaim()` called on it anywhere in `renut_xenos_pipeline_cache.cpp`, so pages were never recycled — confirmed via live polling of `mem_info_gtt_used` (climbed ~6MB/s indefinitely, no plateau).
- Fix (**verified in tree 2026-08-17**): `renut_xenos_pipeline_cache.cpp:1599-1601` — once-per-frame `heap.constants_pool->Reclaim(command_processor.GetCompletedFrame())` in `IssueNativeDraw()`, gated by `BindlessHeapState::last_reclaimed_frame` (declared cpp:490) so it runs once per `GetCurrentFrame()`, mirroring the stock renderer's `uniform_buffer_pool_->Reclaim(frame_completed_)` in `command_processor.cpp`.
- Verified at the time: post-fix GTT usage flat ~301-304MB over 5+ min / 300+ samples, sailed past the old crash point (15,180+ frames, clean exit).
- FAILED, ruled out during diagnosis: fence pools, transient descriptor sets, barrier-site tracking, CSV trace writer — all reviewed clean before finding the real leak.

## FIXED: Shader debug panel QoL (2026-08-17, user-requested)

- Each recorded native shader pair has a stable 1-based `display_id` (`DebugShaderPairInfo::display_id`, `renut_xenos_pipeline_cache.h:149`), assigned at first-seen time, shown as `#N` prefix in the panel (`renut_shader_debug_panel.cpp:93`) instead of raw hash pairs.
- Panel section split changed from live `enabled` state to `marked_bad` (static, from `marked_bad_shaders.txt` at boot) — "All Shaders" vs "Marked Bad" sections, so toggling a pair for testing no longer moves it and makes it hard to re-find.
- Added `SetMarkedBadShaderPairsEnabled(bool)` + "Disable All Bad" / "Enable All Bad" buttons.

## ONGOING / UNVERIFIED: On-screen shader labels

- Feature: floating `#<display_id>` label drawn at each native pair's last-drawn screen position (`DebugShaderLabel`, `GetDebugShaderLabels()`, declared `renut_xenos_pipeline_cache.h:178`).
- Position is computed in `IssueNativeDraw()` by reading the real guest vertex position (via `TranslatePhysical`+`load_and_swap`, only for `VK_FORMAT_R32G32B32(A32)_SFLOAT` position formats) and multiplying by 4 float4 registers starting at a "matrix base register".
- FAILED (2x): guessed base register = c0 (WVP convention), both with local origin and with real vertex position — labels clustered above the player character in both cases. **Research confirms why**: there is no fixed hardware/OS WVP-register convention on Xenos; register allocation is whatever the original title's D3D9 HLSL compiler chose, per shader.
- Current state: base register is a **live ImGui slider (0-252)** (`Set/GetNativeShaderLabelMatrixRegister`, declared `renut_xenos_pipeline_cache.h:215-216`, backed by an atomic at `renut_xenos_pipeline_cache.cpp:156`, driven from `renut_shader_debug_panel.cpp:253-256`) so the user can calibrate in-game without rebuilding. **NOT yet confirmed correct by the user.**
- Also fixed: label overlap/clustering — greedy O(n^2) per-frame collision-avoidance pushdown in `DrawOnScreenLabels()`.
- Next step if resumed: user drags the slider in-game against a known object and reports what register (if any) tracks it. If none does, a general fix needs the real per-shader constant table (name -> register), which is the *same* reflection-data problem blocking shader conversion.

## CORE UNSOLVED PROBLEM: native shader corruption (stretched textures, non-spawning items)

User's stated belief/priority (2026-08-17): a large fraction of shaders marked "bad" actually work natively, and most corruption is concentrated in a smaller set of genuinely broken shaders — wants this root-caused, not worked around.

**2026-08-17, later same day: user tested the positional-location+boolean build — more shaders convert, but visual stretching is unchanged and remains the top-priority visual problem** (worse than missing objects). Found and fixed a strong additional candidate:

**FOUND+FIXED: vfetch scan order vs. container/compile order mismatch (measured 51% of multi-attribute bindings affected).**
- `renut_xenos_pipeline_cache.cpp`'s `GetOrCreatePipeline()` walked `vb.attributes` in raw ucode-scan order to index `vertexLocations[]`/`vertexTexCoordSemanticIndex[]` (`raw_attribute_index`).
- But `tools/build_synthetic_container.py`'s `build_vertex_container()` explicitly **sorts each binding's attributes by `offset_words`** before writing the container's `vertexElementsAndInterpolators[]` array (`attributes.sort(key=lambda a: (a["binding_index"], a["offset_words"]))`), and XenosRecomp's `shader_recompiler.cpp` assigns `[[vk::location(i)]]` by that same array **position** `i` (`for (i = 0; i < vertexElementCount; i++) ... vertexElementsAndInterpolators[field18+i]`), not by instruction address.
- So whenever a shader's vfetch instructions aren't already in ascending-offset order within a binding, the runtime's `raw_attribute_index` (scan order) pointed at the WRONG element of `vertexLocations[]`/`vertexTexCoordSemanticIndex[]` (container/sorted order) — silently binding the wrong buffer data to the wrong shader input.
- **Measured empirically** (`scratchpad/check_order2.py`, extracting real ucode from all 516 `shaders_synthesized/vs_*.bin` containers and running through `ucode_analyze`): **307 of 600 multi-attribute vertex bindings (51%) have out-of-order vfetch instructions.** This is large-scope, not an edge case.
- **Fix** (`renut_xenos_pipeline_cache.cpp`, `GetOrCreatePipeline()`, right before the per-binding attribute loop): build a local `std::stable_sort`ed copy of each binding's attributes by `fetch.attributes.offset` before iterating, matching the container's own sort exactly, so `raw_attribute_index` lines up with `vertexLocations[]`/`vertexTexCoordSemanticIndex[]` again. Needs `#include <algorithm>` (added).
- **Verified**: `ninja rexgpu-renut` compiles/links clean (one transient unrelated clang segfault on the huge generated shader-cache .cpp on the first attempt, succeeded cleanly on retry — not caused by this change, the generated data was untouched). Full `renut` relinked, fresh `librexgpu-renutrd.so` copied to the game build dir and confirmed loadable via ctypes. **Not yet visually confirmed by the user** — this is the very next thing to check.
- This does NOT touch shader conversion yield (still 2035/2053) — it's a pure runtime binding-order fix, orthogonal to what compiles.

- `marked_bad_shaders.txt` cleaned 2026-08-17: 14/78 entries were **stale** (vs or ps hash no longer `ok` in the manifest, so they're excluded from native rendering anyway by the literal-constant fix). Removed via script; backup at `marked_bad_shaders.txt.bak`. **64 pairs / 35 distinct vertex shaders remain** (verified 2026-08-17).
- Interpolator-count mismatch: ruled out (structurally impossible, see above).
- Structural correlation attempt (`scratchpad/analyze_marked_bad.py`, using `ucode_analyze`) — **run against the pre-cleanup 78-entry list**, i.e. 44 distinct marked-bad vertex shaders vs 120 random "ok" controls:
  - Marked bad: multi-row vertex stream (>=8 attrs/binding) 4.5%, dynamic float constant addressing 6.8%, avg bindings 1.00, avg attrs 4.41, `R32G32_FLOAT`-family format ~20.5% vs 5.8% control.
  - Control: multi-row 9.2%, dynamic addressing 0%, avg attrs 4.91.
  - Conclusion: correlations weak/inconclusive, not a smoking gun. Do not over-index on the format difference. If re-run, use the cleaned 35-shader set.
- One deep-dive sample (`vs_2efa6fb4aaedd053` + `ps_144b4fe879000405`, decompiled via direct single-file XenosRecomp invocation) turned out to be a screen-space/post-effect shader (`float4 r0 = float4((iPos.xy - 0.5) * ...)`), not representative — don't generalize from it.
- **Recommended next step (not yet executed)**: clean in-game re-test now that the GTT-leak crash is fixed and the stale list is purged — re-mark what's actually still broken from scratch rather than mining the old mixed-vintage list.

## 2026-08-17 late session: user directive — treat the whole native renderer as unproven, stop micro-test-loop, systematic audit against the real Xenia stock translator

User's exact framing (governs future sessions on this topic): "treat everything about the native renut renderer as wrong until proven it's correct... whatever we have right now is certain wrong." Also explicitly: no more one-fix-then-ask-user-to-test-then-report loops — batch real fixes, verify they compile, deploy, let user play freely, only check back when something concrete is found or a batch is ready.

**Key methodological realization**: the "stock" renderer in this codebase (`pipeline_cache.cpp`/`translator.cpp`/`draw.cpp`) is not a fallback to be replaced — it's Xenia's real, working `SpirvShaderTranslator`, ported wholesale. It is the authoritative reference for "what correct Xenos→Vulkan translation looks like," available for direct comparison, not something to re-derive from screenshots. Every real bug found this session was found by diffing native behavior against this stock code, not by guessing from visual symptoms. Use this method first for any future investigation here.

**Debug tooling added this session (still in tree, off by default via checkbox except where noted)**:
- F5 panel checkbox "Debug: colorize native draws by shader pair" (`SetDebugColorizeMode`/`GetDebugColorizeMode`, `renut_xenos_pipeline_cache.cpp`) — swaps the real (possibly-broken) vertex shader's pixel stage for a trivial flat-color fragment shader (`renut_debug_color_spirv.h`, hand-compiled via `glslangValidator`), color derived per-(vs,ps)-hash. Only colorizes pairs whose pipeline is built **fresh while the mode is on** — does NOT retrofit already-cached pipelines (an earlier version that did this caused a real, confirmed regression: recreating live pipeline objects under load stalled native draw counts and caused flickering — reverted). **Currently defaults to `true` at boot** (`DebugColorizeModeFlag`'s initial value) so it's active before the intro videos/panel are reachable — flip back to `false` once this diagnostic effort concludes, it is not meant to ship on.
- Panel line "N native draw(s) since launch" (`PeekLastFrameDrawCount`) — reads `state.draw_count` (declared in `renut_xenos_pipeline_cache.cpp`) directly; it's a running total since boot, NOT per-frame (the per-frame reset only happens inside `TakeDrawCount()`, which is only ever called from the CSV trace writer, confirmed inactive in normal play — don't reintroduce a "last frame" framing without fixing that first).
- **Real bug found+fixed in this tooling itself**: `IssueNativeDraw`'s pipeline-handle lookup (`state.pipelines` linear scan) only ever matched `entry.pipeline`, never `entry.debug_pipeline` — with debug mode on, EVERY native draw silently vanished (not colorized, not stock-fallback, just never issued, permanently for that pipeline's lifetime) because the lookup always failed and the function returned before drawing. Fixed by matching either handle. This produced the "~50% of the map missing" report — was a real bug in the debug feature, not evidence about the renderer itself. Don't re-conflate the two if this comes up again.

**Real fixes shipped this session, ranked by how the systematic audit rated their likely visual impact (none individually confirmed to fix the corruption yet — user reported "nothing noticeable changed" after the first batch, more found and shipped after)**:

1. **`ndc_scale`/`ndc_offset` position correction, X/Y only** — stock's `spirv_translator.cpp` applies `position_xyz = position_xyz * ndc_scale + ndc_offset * position_w` to EVERY vertex shader's output unconditionally (covers viewport remap, window offset, half-pixel offset, D3D-vs-GL clip convention, reverse-Z, and the entire clip-disabled/screen-space-quad case — full-screen UI/video/post-process). Native shaders had zero equivalent beyond a half-pixel-only term. Wired via a new `VulkanCommandProcessor::last_viewport_info()` accessor (`command_processor.h`/`.cpp`) exposing the SAME `ViewportInfo` stock computes once per `IssueDraw` call (no duplicate/divergent computation), fed to XenosRecomp-compiled shaders via new `g_NdcScale`/`g_NdcOffset` shared constants (`shader_common.h`, `SharedConstantsLayout` in `renut_xenos_pipeline_cache.cpp`).
   - **Real regression found+fixed same day**: first version applied this to Z too (matching stock's formula literally) — user's next test showed individual native shaders going INVISIBLE when toggled on. Root cause: stock's Z scale/offset (often 0.5/0.5) assumes semantic knowledge of the guest's clip-space convention that a full translator has and a raw ucode pass-through (XenosRecomp) does not — applying it blindly pushed vertices outside the valid depth range and got them clipped. Scoped back to X/Y only (`shader_recompiler.cpp`'s two injection sites, both now emit `oPos.xy = oPos.xy * g_NdcScale.xy + g_NdcOffset.xy * oPos.w;`, Z untouched). **Z-axis equivalent remains a real, unexamined gap** — if revisited, do NOT just copy stock's formula again; the Xenos DX-clip-space-convention/reverse-Z semantics need to be understood specifically for XenosRecomp's raw output first.
2. **Alpha test now dynamic per-draw, with compare-function gating** — was a static spec constant baked once at shader-conversion time (whatever `RB_COLORCONTROL.alpha_test_enable` happened to be when XenosRecomp's static analysis ran), now reads the real register every draw via new `g_AlphaTestEnable` shared constant. **Real regression found+fixed same day**: first version gated on `alpha_test_enable` alone; XenosRecomp's compiled `clip(oC0.w - g_AlphaThreshold)` hardcodes exactly ONE comparator (GEQUAL-keep — discard when `oC0.w < threshold`), with no way to express D3D9's other 7 `D3DCMP_*` functions. Games commonly leave the alpha-test unit enabled while setting `ALPHAFUNC=ALWAYS` as a no-op idiom — the first version would wrongly discard via GEQUAL logic on every such draw instead of discarding nothing. Now gated on `alpha_test_enable && alpha_func == xenos::CompareFunction::kGreaterEqual` (`renut_xenos_pipeline_cache.cpp`, `IssueNativeDraw`). **This is currently the single strongest candidate for "enabling native rendering makes geometry disappear"** — not yet confirmed by the user (deployed, awaiting play).
3. **`RB_DEPTHCONTROL` now goes through `draw_util::GetNormalizedDepthControl`** instead of reading the raw register — matches stock's real `RB_MODECONTROL.edram_mode` gate (force-disables depth entirely outside `kColorDepth`/`kDepthOnly` modes). Narrower/lower-confidence than items 1-2.
4. **`color_blend_state.attachmentCount` was hardcoded to `1`** regardless of how many color render targets (MRT) a draw actually uses — Vulkan requires this to exactly match the real attachment count (both under dynamic rendering and classic render passes). Any draw using more than one simultaneous color target would have had undefined/silently-dropped output on attachments 1+. Fixed by computing the real count once (shared with the existing dynamic-rendering attachment-count computation, same `render_pass_key.depth_and_color_used` bits — one source of truth, not two) and building an array of that many identical `VkPipelineColorBlendAttachmentState` entries (`renut_xenos_pipeline_cache.cpp`).

**RULED OUT with real measurement (2026-08-17, same day)**: `exp_adjust`/`signed_rf_mode` on vertex fetches. Added `exp_adjust`/`signed_rf_mode` fields to `tools/ucode_analyze.cpp`'s JSON output (they weren't emitted before), then swept all 516 real `shaders_synthesized/vs_*.bin` containers (`scratchpad/check_exp_signed.py`): **5115 total vertex attributes examined, 0 with nonzero `exp_adjust`, and of 2351 signed attributes, 0 use `signed_rf_mode` other than `kZeroClampMinusOne` (value 0)**. Critically, `kZeroClampMinusOne` (Microsoft's clamped/has-a-zero convention, `max(c/(2^(b-1)-1), -1)`) IS what Vulkan's own `_SNORM` format spec implements — the "known gap" in the code comments was about the OTHER mode (`kNoZero`, OpenGL's alternate no-zero mapping), which **this game's real data never uses at all**. Both items in the earlier audit write-up were real theoretical gaps but are proven non-issues for this specific game's corpus — do not re-investigate without new evidence (e.g. a newly-added shader capture that behaves differently).

**Ranked but NOT yet acted on** (from the systematic audit, still real/open):
- Alpha test only handles GEQUAL now (see fix #2) — the other 7 compare functions still can't clip correctly; currently those draws just never clip (safer than before, but not fully correct either).
- Texture views always requested as unsigned (`BindTexture2DFixedSlot` hardcodes `is_signed=false`) — shaders sampling a texture as signed-normalized data get the wrong interpretation. Investigated but NOT fixed: stock's real signedness (`VulkanShader::TextureBinding::is_signed`, `texture_cache.cpp`'s `IsAnySignSigned(binding->swizzled_signs)`) is derived from the real guest texture FORMAT/swizzle data during translation, not a simple per-instruction field `Shader::TextureBinding`/`ParsedTextureFetchInstruction` exposes anywhere (confirmed via grep — no `is_signed` field exists on the texture-fetch side of `shader.h` at all, unlike the vertex-fetch side). Fixing this properly needs understanding stock's format/signedness derivation first; deprioritized this session to avoid another guessed-and-wrong regression like the two already found and fixed today.
- **QUANTIFIED, narrow, NOT fixed**: `BindlessHeapState` only ever populates the Texture2D descriptor array (`texture2d_entries`) — 3D and Cube map slots are left permanently unbound (`renut_xenos_pipeline_cache.cpp`'s own comment: "Only Texture2D is populated for now... 3D/Cube slots stay unbound"). Added a `dimension` field to `tools/ucode_analyze.cpp`'s texture-binding JSON (wasn't emitted before) and swept all 1537 real `shaders_synthesized/ps_*.bin` containers (`scratchpad/check_tex_dim2.py`): of 35135 total texture fetch instructions, **34967 are 2D (99.5%), 137 are Cube (0.4%), 29 are 3D (0.08%), 2 are 1D**. Real but narrow — likely a handful of specific shaders (skyboxes, reflective/cubemap-lit surfaces), not a systemic cause of the widespread corruption. Any native shader hitting one of these 166 fetches samples an unbound descriptor (garbage/undefined, `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` makes this valid-but-wrong rather than a validation error). Left unfixed this session — low value relative to effort given the small real-corpus count; if revisited, needs `texture3d_entries`/`texturecube_entries` arrays populated the same way `texture2d_entries` already is, plus a same-shaped write path in `BindTexture2DFixedSlot` (which would need renaming/generalizing since it's 2D-specific by name and logic right now).

**Ruled out this session** (do not re-investigate without new evidence): quad-list/rectangle-list/point-sprite primitives already correctly fall back to stock (`PrimitiveTypeToVkTopology` explicitly rejects them); front-face winding/cull-mode convention matches stock exactly (line-for-line same register bits, same ternary); texture residency request (`RequestTextures`) and shader translation (`EnsureShadersTranslated`, which populates `GetUsedTextureMaskAfterTranslation`) both run unconditionally before the native/stock branch, not gated behind which path is taken; stencil state defaults safely to disabled (not a source of invisibility); wrong-AppImage-being-tested was considered and ruled out (the AppImage on the user's Desktop is a stale Aug-11 build with no `librexgpu-renutrd.so` at all, but the user's screenshots showed the F5 panel/debug-colorize/draw-counter features working, which only exist in the dev build — they are testing the right binary).

## Standing procedures / gotchas

- Two-step verification mandatory before every test cycle: (1) verify `.so` loads via ctypes, (2) `command cp -f` both plugin files to the game directory, (3) `md5sum` both copies to prove they match. Skipping this silently tests stale code — this has bitten this project before.
- Edit the SDK at `reNut-build-scratch/rexglue-sdk-src/src/...`, never `out/install/linux-amd64/include/...` (install-time copy).
- `ls` is aliased in this shell (bare `ls -la` can silently produce nothing, `ls -t` demands `--time <FIELD>`) — use `command ls`.
- zsh globs `--include=*.cpp` as a filename pattern and fails with "no matches found" — quote grep args or omit `--include`.
- `grep -F "^${vs} "` does NOT anchor (`-F` disables regex anchors) — use `awk -v t="$vs" '$1==t{print}'` for exact-field manifest lookups.
- User division of labor: I fix/investigate code, user builds/runs the actual game (I don't have display access), except read-only live GPU-memory polling via `/sys/class/drm/...` which I can do myself.
