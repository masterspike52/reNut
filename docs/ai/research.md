# reNut Knowledge Bible

> **Scope**: narrative background — what reNut/rexglue/XenosRecomp actually are, the
> current architecture, and how comparable projects (UnleashedRecomp, DPRecomp, Xenia)
> solve the same problems. Read this for context before making an architectural call, or
> to explain the project to someone new. Skip it if you already know the shape of the
> project and just need the rules ([`START_HERE.md`](../START_HERE.md)) or a specific
> past event ([`history.md`](history.md)).

Living reference document: what this project is, where it stands, what's been tried,
what's left, and how comparable projects solve the same problems. This is a *narrative/
research* document for humans — for terse machine-oriented state, see `history.md`.
Update this when the project's stage, goals, or research picture changes; update
`history.md` for tactical discoveries/fixes/failures.

Last updated: 2026-08-17. All numeric claims re-verified against the live tree
and shader manifest on 2026-08-17 (§2a) — earlier drafts of this file carried
stale conversion counts.

---

## 1. What reNut Is

reNut is a native Linux/Windows port of **Banjo-Kazooie: Nuts & Bolts** (Xbox 360, 2008),
built on **rexglue** (`rexglue-sdk`), a static-recompilation toolkit in the spirit of
Xenia (emulator) and the hedge-dev `*Recomp` family (static recompilers), but structured
as a reusable SDK rather than a single-game bespoke renderer.

- **CPU side**: the Xbox 360's PowerPC code is statically recompiled ahead-of-time into
  C++ (one function per original subroutine, `sub_XXXXXXXX`, in `generated/`). This part
  is mature and shipping — the game boots, plays, and is generally stable, with a number
  of hand-written fixes for recompiler mis-translations (see `src/renut_engine/fixes/`).
- **GPU side**: two coexisting paths.
  1. **Stock/shipped path ("renut" plugin)**: runtime translation of the Xenos GPU command
     stream to Vulkan calls, interpreting the guest's shader microcode via emulation each
     frame. This is what every user actually plays on today. Correct, ~35-40fps in the
     city, GPU-bound.
  2. **Native-renderer effort (in progress, not shipped)**: statically recompile
     individual Xenos vertex/pixel shader microcode programs to real Vulkan pipelines
     ahead of time via **XenosRecomp** (hedge-dev's shader recompiler, PPC-GPU-microcode →
     HLSL → DXC → SPIR-V), then substitute them in at draw time instead of interpreting.
     Goal: near-native shader execution cost instead of per-instruction emulation.

reNut's specific complication (see §6 for how this differs from sibling projects): it
must recover shaders from **live captured Xbox 360 GPU microcode**, not from an offline
extraction of the game's original shipped shader containers with intact D3D9 reflection
data. Nuts & Bolts' shaders are frequently missing that reflection data, and a large
fraction of the game's most-used shaders never go through a capturable D3D9
`CreateVertexShader`/`CreatePixelShader` call at all (see §4).

## 2. Current Stage

| Subsystem | Stage |
|---|---|
| CPU recompilation / core gameplay | **Shipping.** Stable, playable, Linux + Windows. |
| Stock GPU path (`renut` plugin) | **Shipping.** Correct, in daily use. |
| Native shader substitution | **Experimental, off the shipped critical path.** Real pipelines render correctly for a subset of shaders; a real GTT-leak crash blocking long play sessions was found and fixed this week; the core "why do many native shaders visually corrupt" question is still open. |
| Shader debug tooling (panel, on-screen labels) | **Actively being built out**, in direct response to user needs for bisecting native-shader corruption. |
| Build pipeline (batch shader conversion) | **Mature and fast** (parallelized this week); conversion yield is the current bottleneck, not raw throughput. |

### 2a. Conversion yield — real measured numbers (2026-08-17)

Read straight from `generated/renut_xenos_shader_cache_manifest.txt`:

| | total | converted ok | failed |
|---|---|---|---|
| **Vertex shaders** | 516 | **280 (54.3%)** | 236 |
| **Pixel shaders** | 1537 | **1531 (99.6%)** | 6 |
| All | 2053 | 1811 (88.2%) | 242 |

Failure reasons: `skipped-by-xenosrecomp:SPIR-V` 224 vs + 6 ps;
`:memory` 10 vs; `:duplicate` 2 vs.

**This table sharpens the whole document.** Pixel-shader conversion is
effectively a solved problem at 99.6%. *All* remaining conversion-yield loss is
vertex shaders, and vertex shaders are precisely where the missing D3D9 vertex
declaration / usage-semantic data lives (§4, §6). This is direct quantitative
confirmation that vertex-declaration recovery (§5) is not just *a* lever — it is
the *only* remaining lever on shader coverage.

Older figures quoted in this project's history (12/1884 → 1492/1884 → 1706/2208,
"~500 unconvertible vertex shaders") are earlier snapshots of a differently-sized
shader dump and are superseded by the table above. On-disk raw ucode today:
525 `vs_*` + 1436 `ps_*` = 1961 files (the manifest carries slightly more entries
because container-synthesis inputs don't all have a standalone ucode file).

## 3. Goals

**Primary, currently-blocking goal**: fix native-shader visual corruption (stretched
textures, non-spawning items) so more of the ~2050 captured shaders can be safely
substituted in. The user's stated belief (2026-08-17): a large fraction of shaders
currently marked "bad" likely work correctly natively, and the corruption is
concentrated in a smaller set of genuinely-broken shaders rather than being pervasive —
this is the thing worth root-causing, not papering over with more exclusion heuristics.

**Secondary/longer-term goals**:
- A real, isolated, timer-based A/B performance measurement per shader (native vs
  stock-translated) — never done. Whole-scene comparisons so far show native
  substitution at partial coverage is **not** a clear win (see §4), so this matters before
  investing further in scaling shader count.
- Recover real vertex declarations (usage semantics) for IM_LOAD-loaded shaders, which
  currently have none — the single biggest known lever on how many vertex shaders can be
  safely converted at all (see §4, §5).
- Decide whether positional/index-based interpolator linkage (Xenia's real hardware
  model, see §6) is worth building as an alternative to XenosRecomp's semantic-name
  requirement — potentially unlocks most of the **236 currently-failing vertex
  shaders** (§2a), but is real engineering effort, not a quick patch.

## 4. What We've Done (chronological, by outcome)

### ✅ Done — shipped / verified working
- CPU recompiler crash fixes (`mullhwucrash.cpp` and friends) — game is stable.
- Linux port: XDG paths, mnk controls, audio backend/UAF fixes, POSIX file-access fix.
- Batch shader-conversion pipeline (XenosRecomp wrapper): synthesizes valid D3DX9
  constant-table blobs (float/bool constants) from `ucode_analyze`'s own analysis when the
  real captured container lacks one — raised real conversion yield from 12/1884 to
  1492/1884 at the time, and the pipeline now stands at 1811/2053 (88.2%) overall
  (§2a for the current, authoritative breakdown).
- **Two real upstream XenosRecomp bugs patched locally**
  (`cmake/patches/xenosrecomp_graceful_skip.patch`, do not revert):
  (1) any failed DXC compile hit a bare `assert()` with no null-check and
  segfaulted the whole process — now a logged skip carrying the shader hash and
  real reason; (2) XenosRecomp's own `std::execution::par_unseq` multi-shader loop
  **deterministically dropped ~450 of 2135 shaders on every run** with identical
  input (bisected; the same set converts 100% under `std::execution::seq` in ~50s).
  Root cause is DXC-internal `thread_local` state, not our code. Separately, 26/748
  Banjo shaders segfault XenosRecomp's directory-scan driver outright, which is why
  `tools/xenos_batch_convert.py` runs each shader in its own subprocess.
- `--reject-heuristic` filter: rejects vertex containers where any attribute's usage
  semantic had to be *guessed* (no real captured vertex declaration) — trades shader count
  for correctness; **confirmed via live screenshot** this eliminates the
  shattered-triangle corruption class it targets.
- **Vertex input LOCATION bug** (a real, structural correctness bug, present since the
  very first working native shader): Vulkan locations were assigned sequentially instead
  of matching XenosRecomp's fixed usage-based `USAGE_LOCATIONS` table. Fixed and verified
  end-to-end (binary + runtime inspection). Did **not** fix the visible corruption for the
  shaders exercised in a normal play session (their layouts were too simple to trigger it) —
  but is a real correctness fix regardless, and matters for future, more complex shaders.
- **Confirmed via direct A/B test** (force `HasNativePipeline()` false): the remaining
  visible corruption is caused by something inside specific live native shaders, **not**
  by mixing native and stock rendering in the same scene.
- Build parallelism: `xenos_batch_convert.py` per-shader loop was fully sequential in
  Python (unrelated to XenosRecomp's own internal `seq`-vs-`par_unseq` choice, which is
  intentionally `seq` — see §5). Parallelized via `ThreadPoolExecutor`.
- Shader debug panel QoL: stable numbered IDs, decoupled toggle-vs-move-to-disabled,
  bulk "Enable/Disable All Bad" buttons.
- **Real GTT memory leak found and fixed**: `constants_pool` in the native-pipeline
  bindless heap was never calling `Reclaim()`. Root-caused via live GPU-memory polling
  (not guessed), fixed with a once-per-frame reclaim call, verified via before/after
  telemetry (unbounded climb → flat plateau, crash point sailed past cleanly).
- Cleaned `marked_bad_shaders.txt`: removed 14/78 stale entries (shaders that no longer
  convert to native at all after the constant-table fix, so marking them "bad" was moot).
  **64 pairs remain, covering only 35 distinct vertex shaders** (verified 2026-08-17) —
  i.e. the corruption reports concentrate on a small vertex-shader set that gets paired
  with many different pixel shaders, which is itself evidence for the user's hypothesis
  that the problem is a narrow set of genuinely-broken shaders rather than pervasive.

### 🟡 Tried, real result, but not a net win — informative, not a dead end
- Full ucode-based synthesis (bypassing the D3D9-hook capture limitation, see below):
  raised converted count to 1590/2143 with 148 shader pairs live in real gameplay
  simultaneously — but introduced severe visible corruption, root-caused to the vertex
  usage-semantic heuristic guessing wrong for non-trivial attribute layouts. Led directly
  to `--reject-heuristic` above.
- Native substitution at ~150/1268 drawn pairs (partial coverage): **measured performance
  regression**, not improvement (main menu ~150fps → 20-80fps with native+heuristic on).
  Per-draw native overhead (pipeline lookup, bindless descriptor/constant bookkeeping) is
  evidently not cheap enough to net-win at low substitution percentages. This is why a
  real isolated A/B measurement (goal in §3) now matters more than raw shader-count scaling.
- On-screen shader position labels: two static "WVP is at register c0-c3" guesses both
  put labels above the player character instead of at each object. Converted into a live,
  panel-adjustable calibration slider instead of a third blind guess — **this is expected
  behavior given real-world findings in §6** (there is no fixed hardware WVP-register
  convention; it's whatever the original D3D9 shader compiler happened to allocate,
  per-shader).

### ❌ Reverted — real regression found
- A fix to `AnalyzeUcode()`'s control-flow-bound heuristic (raised offline shader-analysis
  success from 39 to 98) was reverted after it caused a full black-screen regression in
  the **shipped stock renderer**, which shares this exact function. Do not re-edit the
  shared runtime path for this; a separate offline-only analysis function would be needed.

### ⛔ Unsolved / blocked (root problem, not yet cracked)
- **Vertex declaration recovery for IM_LOAD-loaded shaders.** The game's most-used
  shaders are loaded directly into GPU sequencer memory via `IM_LOAD`/`IM_LOAD_IMMEDIATE`
  PM4 packets, never through a capturable D3D9 `CreateVertexShader`/`SetVertexDeclaration`
  call — so there is no real captured usage-semantic data for them, structurally, via any
  existing hook. This is the direct cause of needing the heuristic (and therefore
  `--reject-heuristic`'s shader-count cost). No public prior art exists for recovering
  this (see §6) — Xenia sidesteps the whole category by not needing D3D9-level semantics
  in the first place.
- **Root cause of visible corruption in the *specific* shaders currently live** — narrowed
  to "something inside specific native shaders" (not mixing, not the location bug for the
  simple shaders tested), but the exact mechanism is still unidentified. See §5 checklist.

## 5. Checklist of Ideas / Potential Next Steps

Ideas below are unranked except within groups; check off / annotate as they're tried.
Cross-reference `history.md` before starting any of these — some overlap with
documented failed approaches.

> **2026-08-17 update — the vertex-shader half of this list is now root-caused.**
> Every failing shader was re-run through the real compiler and its diagnostic captured.
> 198 of 236 vertex failures (84%) are a single bug: XenosRecomp's Unleashed-specific
> `USAGE_LOCATIONS` table omits the usages our shaders use, and a table miss emits a
> vertex parameter with *no* `[[vk::location]]`, which DXC rejects outright. Two smaller
> bugs (undeclared `r63` = unimplemented point-size export, 15 shaders; interpolator
> table missing non-TEXCOORD usages, 13 shaders) account for most of the rest.
> Full analysis with measured tables, ruled-out hypotheses and a ranked fix order:
> https://claude.ai/code/artifact/b760eff3-3850-4f0f-9676-29c06a4d1463
> See `history.md` for the terse version with file:line references.
> **Note the trap**: 0 of those 198 have a real captured vertex declaration, so simply
> widening the table ships 198 shaders wired from *guessed* semantics. The fix is to stop
> routing vertex data through D3D9 usage names at all (item below, now evidence-backed).

**Root-causing the current corruption (highest priority):**
- [ ] Clean re-test pass now that the GTT-leak crash and stale marked-bad entries are
      fixed — get an up-to-date, uninterrupted signal on which of the 64 remaining
      marked-bad pairs are actually still broken.
- [ ] **NEW, highest-value corruption lead (2026-08-17).** Audit XenosRecomp's two silent
      "Tier 2" assumptions, both of which produce *wrong data with no error* — the exact
      shape of the reported symptoms. (1) `g_SwappedTexcoords` corrects the 16-bit YXWZ
      endian-swizzle **only for TEXCOORD** semantics; a 16-bit NORMAL/COLOR/etc. attribute
      is silently swizzled wrong. (2) `R11G11B10` unpacking is applied **only** to
      NORMAL/TANGENT/BINORMAL; the same packed format on any other usage is read as raw
      float garbage. Upstream's README explicitly flags both as Unleashed-specific
      assumptions other games may need to extend. Both are decidable **offline** from
      `ucode_analyze`'s already-decoded per-attribute `data_format` — this is a query over
      existing data, not a rewrite, and it directly targets stretched-texture/bad-lighting
      symptoms rather than compile failures.
- [ ] Per-attribute data-correctness audit (stride/offset/format) for a specific corrupted
      shader once identified, rather than structural (interpolator count, location)
      checks — the known symptom shapes (stretched textures = UV-ish, odd lighting =
      normal-ish) point at per-attribute *data* correctness, not topology/linkage.
- [ ] Use the on-screen-label calibration slider (once a working register value for at
      least one shader is found) to help visually correlate a specific broken shader with
      a specific broken object in-game.

**Vertex declaration recovery (biggest lever on shader count):**
- [ ] Investigate inspecting live vertex buffer memory/strides at draw time as an
      alternative signal to captured D3D9 declarations (not explored yet).
- [ ] Evaluate whether `data_format`/component-count patterns already known per-attribute
      (from ucode analysis alone) could produce a *smarter* heuristic than blind
      POSITION-then-TEXCOORD, without needing real captured declarations (not explored
      yet — different from the two heuristics already tried and reverted/kept).
- [ ] Test the "paired pixel shader's own interpolator-usage container" cross-reference
      idea from §6's research — real PS containers may survive capture more often than VS
      ones; on real hardware, VS export (usage, usageIndex) must match what the PS
      declares for the pair to function, so a real PS-side declaration could backfill the
      VS side. Untried, quick to falsify.
- [ ] **PROMOTED TO TOP PRIORITY (2026-08-17, now evidence-backed).** Positional/
      index-based linkage as a structural replacement for XenosRecomp's semantic-name
      requirement, modeled on Xenia's real hardware behavior (§6): assign Vulkan location
      *i* to the *i*-th vertex element of each shader, on both the container side and
      `ExtractVertexLocations()`/`GetOrCreatePipeline()`. Measured payoff: unblocks the
      198 shaders failing on partial-location assignment, and does so *without* trusting
      the heuristic's guessed semantic names, because the name stops affecting wiring.
      Also dissolves the latent `(Position,1)`/`(TexCoord,7)` → location-15 collision by
      construction. Confirmed feasible: largest shader needs 28 attributes, this GPU
      exposes 32 (gate on a runtime check — the portable minimum is 16). XenosRecomp's own
      README recommends precisely this ("a generic solution would assign unique locations
      per vertex shader"), so it is the sanctioned direction, not a fork.
      Justification for the primary reason this was previously deferred as "the most work":
      it is now clear the alternative is *more* work, because every other path requires
      recovering vertex declarations that structurally do not exist.

**Robustness / diagnostics:**
- [x] ~~Patch XenosRecomp locally to skip-and-log (not crash) on any failed DXC compile.~~
      **DONE** — shipped as `cmake/patches/xenosrecomp_graceful_skip.patch` (verified
      present and applied 2026-08-17). This is why the manifest now carries informative
      `skipped-by-xenosrecomp:<reason>` entries instead of silent segfaults.
- [ ] Root-cause the one still-unexplained XenosRecomp crash (`ps_9c016d19c75c8b76`,
      only reproduces in the real batch pipeline, not in isolation) — low priority.
      **Confirmed still broken 2026-08-17, and in an informative way**: its `.ucode` file
      exists on disk, yet the shader has **no manifest line at all** — not even a
      `skipped-by-xenosrecomp:` entry. Every other failure produces a logged skip thanks
      to the graceful-skip patch, so this one is dropping out *before* the patched
      error path, i.e. it's a different failure mode than an ordinary DXC reject.
      That makes it a cheap, well-isolated diagnostic target if picked up.

**Performance:**
- [ ] Build the real isolated single-shader GPU-timer A/B measurement (native vs
      stock-translated), still never done despite being flagged as important twice.
      Blocked on having at least one *visually clean* native shader to measure fairly.
- [ ] If per-draw native overhead is confirmed to be the bottleneck (not shader execution
      cost itself), investigate reducing it: pipeline cache hit-rate, descriptor write
      reduction, bindless heap bookkeeping cost per draw — none investigated yet.

**Geometry coverage:**
- [ ] Decide whether to implement real geometry-shader expansion for
      `kRectangleList`/`kQuadList` primitives (currently hard-fallback to stock,
      permanently) — probably fine to leave as-is (likely cheap UI/2D draws), but not
      formally decided.

## 6. Research: How Comparable Projects Handle This

All of the following (except Xenia, an emulator) are members of the **hedge-dev
`*Recomp` family** — `XenonRecomp` (PowerPC CPU recompiler) + `XenosRecomp` (Xenos
shader recompiler) are the actual shared upstream tools; reNut, UnleashedRecomp, and
Fable2Recomp are three independent *consumers* of that toolchain (reNut and
Fable2Recomp additionally share the `rexglue` SDK layer on top; UnleashedRecomp does not
use rexglue — it has its own bespoke renderer).

### UnleashedRecomp (Sonic Unleashed, hedge-dev — the most mature sibling)
- Uses XenosRecomp for shader translation, same as reNut.
- **Structurally different GPU pipeline**: a renderer **written from scratch**, not a
  retrofit onto an existing runtime-translated GPU backend. Every shader it compiles is
  fed by an **offline extraction of the game's real shipped shader containers** — real
  D3DXSHADER_CONSTANTTABLE reflection data intact, because Sonic Unleashed's shaders were
  built with that data present (unlike Nuts & Bolts, which frequently stripped it).
  Confirmed via direct GitHub code search: zero hits for "IM_LOAD" anywhere in their
  repo — they structurally never encounter the missing-declaration problem reNut has,
  because their capture method is a different (offline, complete) one that never depends
  on runtime hooks catching every shader as it's created.
- Modern rendering techniques: **bindless textures** and **shader specialization**
  (reNut's own native path also uses a bindless heap — same real technique, independently
  arrived at).
- **Async/predictive pipeline compilation**: traverses the game's rendering structures
  ahead of time during asset loading to determine what pipelines will be needed, compiling
  them in the background instead of hitching at first-use. reNut's native path currently
  does none of this (lazy, first-use `GetOrCreatePipeline`) — a real potential technique
  to adopt if/when native coverage grows large enough for compile stutter to matter.
- Skips real Xbox 360 GPU hardware quirks entirely where not needed for a PC port
  (no eDRAM emulation, etc.) — same category of simplification reNut benefits from by
  not being a cycle-accurate emulator.

### Fable2Recomp (early-stage, actively developed, shares reNut's exact SDK)
- Explicitly built on **rexglue**, same SDK as reNut ("static recompilation approach
  based on the ReXGlue project"). The closest sibling project architecturally.
- Public documentation does not yet describe its GPU/shader approach in any technical
  detail — appears to be earlier-stage than reNut on this specific front (no evidence
  found of a working native-shader substitution effort; likely still on/targeting a
  runtime-translated GPU path, same starting point reNut had before this effort began).
  A linked upstream issue (`hedge-dev/XenonRecomp#77`, "Fable 2 Recomp and possible issues
  and help needed") shows they're still working through **CPU-side** recompilation issues
  (missing-instruction errors), i.e. earlier in the pipeline than reNut's GPU-stage work.
- **Worth periodically re-checking** as it matures — being on the same SDK means any
  native-shader infrastructure they build (or blockers they hit) is maximally relevant to
  reNut, more so than UnleashedRecomp's bespoke-renderer approach.

### Viva Piñata Recomp / TiP-Recomp (both Rare, both rexglue, both very early-stage)

Two separate rexglue-based projects for Rare's Viva Piñata games — the closest *studio*
sibling to reNut (Rare made both Banjo-Kazooie: Nuts & Bolts and the Viva Piñata games on
the same generation of in-house tech, so shared engine-level quirks/patterns are plausible,
even though these are separate codebases, not a shared game engine repo):

- **`VivaPinataRecomp/VivaPinataRecomp`** — the original 2006 Viva Piñata (Xbox 360).
  Extremely early stage (2 commits as of this check) — no README/technical documentation
  published yet, nothing to compare against on the GPU/shader front at this time.
- **`SolarCookies/TiP-Recomp`** — *Viva Piñata: Trouble in Paradise* (2008 sequel).
  Also rexglue-based, "very early" development, targeting Windows first with Linux
  planned. Stated goal is explicitly to move off Xenia emulation and enable modding the
  way a native PC port would, rather than a performance argument specifically — good
  context for why these Rare titles keep getting picked up by the rexglue community
  (moddability + platform support), same motivation as reNut's own userbase likely cares
  about, distinct from (though compatible with) the native-shader performance goal.
  No GPU/shader-specific technical documentation published yet either.
- **Why worth tracking**: both are pre-CPU-recompilation-maturity stage (earlier than
  Fable2Recomp even), so there's nothing to learn from their GPU handling *yet* — but
  being Rare titles on rexglue, any Rare-engine-specific CPU recompiler quirks or fixes
  they document (analogous to reNut's own `mullhwucrash.cpp`-class fixes) could be
  directly relevant later, and it's plausible (unconfirmed) their shader capture will hit
  the same IM_LOAD/missing-reflection-data problems reNut hit, given the games share a
  console generation and likely a similar in-house Rare toolchain lineage. Re-check
  periodically as they mature past early CPU-recompile stage.

### Xenia (Xbox 360 emulator — not a recompiler, but shares the exact same GPU hardware target)
- **The single most relevant real architectural lead found this project has** (see
  `history.md` and `renut_shader_conversion_blockers.md` for full detail): on real
  Xenos hardware, interpolator linkage between vertex shader exports and pixel shader
  imports is **purely positional (by register index), not by semantic name**. Export
  register `o0` is always position; generic interpolators `o1`-`o15` link to the pixel
  shader's inputs by index alone. Xenia's `shader_translator.cc`/`dxbc_shader_translator.cc`
  never emit named HLSL semantics for interpolators at all — it links VS export N to PS
  import N directly in the compiled bytecode.
  - **Why this matters for reNut**: XenosRecomp's approach (emit real HLSL with named
    D3D9 semantics, resolved via a captured `Interpolator` usage/usageIndex array) is
    real, but not the *only* valid approach, and is the actual source of reNut's ~500
    currently-unconvertible vertex shaders (no captured declaration → no semantic name →
    assert/crash). A positional-linkage alternative is a real, scoped (if substantial)
    engineering idea — see §5 checklist.
- eDRAM (10MB embedded framebuffer memory, unique to Xenos) is emulated in system memory
  by Xenia via manual render-target management — not directly relevant to reNut's shader
  problem, but useful background if any eDRAM-dependent rendering path is ever hit.
- Xenia emulates at the hardware/microcode level generally, never reconstructing D3D9 API
  objects (vertex declarations, shader objects) — this is *why* it never needed the kind
  of object-layout reverse-engineering reNut once attempted and confirmed is a dead end
  (no public documentation of the real Xbox 360 D3D9 object memory layout exists anywhere
  — NDA'd XDK material, not search-indexed).

### Xbox 360 / Xenos hardware documentation (Free60, Beyond3D, general)
- Free60's Xenos GPU page and Beyond3D's architecture writeup describe the hardware
  (unified shader architecture, R500-family lineage, 10MB eDRAM, SM3.0-plus) but **do
  not** document shader constant register *allocation conventions* (e.g. "WVP is always
  at c0") — because there isn't one. Constant register assignment is a decision made by
  the original title's D3D9 HLSL compiler at build time, per shader, not a fixed hardware
  or OS convention.
  - **This directly explains** why reNut's two static "WVP is at c0-c3" guesses for the
    on-screen shader label feature both failed (§4) — there was never a reason to expect
    a single fixed register to work across different shaders in the first place. The
    live-adjustable calibration slider (current approach) is the *correct* structural
    response to this fact, not a stopgap — a truly general fix would need the real
    per-shader constant table (name → register mapping), which is exactly the same
    reflection data problem blocking full shader conversion in the first place (§4).
- No source anywhere (Xenia, Free60, Beyond3D, XenosRecomp's own issue tracker, general
  shader-decompilation literature) describes an ALU-pattern-based technique for inferring
  vertex-declaration semantics from raw microcode alone. If reNut ever builds one, it
  would be genuinely novel, not a known/documented technique being reimplemented.

### Summary: where reNut is similar vs different

| | reNut | UnleashedRecomp | Fable2Recomp | Viva Piñata Recomp / TiP-Recomp | Xenia |
|---|---|---|---|---|---|
| Developer of original game | Rare | Sonic Team | Lionhead | Rare | N/A |
| CPU recompiler | XenonRecomp family (via rexglue) | XenonRecomp | XenonRecomp (via rexglue) | XenonRecomp (via rexglue) | N/A (emulated, not recompiled) |
| Shader recompiler | XenosRecomp | XenosRecomp | Unknown/not yet built | Unknown/not yet built | N/A (own translator, positional linkage) |
| SDK layer | rexglue | Bespoke | rexglue | rexglue | N/A |
| Shader source data | Live runtime capture (D3D9 hooks + raw ucode capture) — **frequently incomplete** | Offline extraction of real shipped containers — **complete** | Unknown | Unknown | N/A — reads real hardware command stream live, no declaration recovery needed at all |
| Native shader coverage | Partial, experimental, corruption not fully root-caused | Full (their whole renderer *is* native) | Unknown/earlier stage | Very early stage, no GPU work published yet | N/A (always "native" in the sense of always-translated-at-runtime) |
| Bindless textures/shader specialization | Yes (native path) | Yes | Unknown | Unknown | N/A |
| Predictive/async pipeline compile | No (lazy, first-use) | Yes | Unknown | Unknown | N/A |

## 7. Where things actually live (orientation)

Full path/symbol detail is in `history.md` — this is the two-minute version,
because the single most common way to waste time on this project is editing or
testing the wrong copy of something.

- **Two separate trees.** The game repo is `/home/nick/Desktop/reNut/` (git, branch
  `linux-work`). The rexglue SDK — where all GPU/renderer work happens — is a
  *different, non-repo* tree at
  `/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src/`. Renderer edits go there.
- **In the SDK, headers and sources are split**: sources under `src/graphics/…`,
  public headers under `include/rex/graphics/…`. There is also an install-time header
  copy under `out/install/linux-amd64/include/` — **never edit that one.**
- **The native shader effort is essentially two files**:
  `src/graphics/vulkan/renut_xenos_pipeline_cache.{h,cpp}` (pipeline cache, bindless
  heap, `HasNativePipeline()`, `IssueNativeDraw()`) plus
  `src/graphics/vulkan/renut_shader_debug_panel.cpp` (ImGui panel — note it has **no**
  matching `.h`; its API is declared in the pipeline-cache header).
- **The `IM_LOAD` problem lives in** `src/graphics/command_processor.cpp`
  (`ExecutePacketType3_IM_LOAD` / `_IM_LOAD_IMMEDIATE`) — the shared PM4 dispatch,
  not the Vulkan backend. That's the code path where a shader arrives with no D3D9
  declaration attached, which is the structural origin of §4's blocker.
- **Building deploys nothing.** `ninja -C out/build/linux-amd64-relwithdebinfo renut`
  builds the SDK plugins into the *SDK's* `out/linux-amd64/`. They must then be
  `cp -f`'d into the game's build dir, and the copies `md5sum`-checked. A stale plugin
  copy runs old code silently, with no error — this has invalidated test results here
  before, and is the single most important gotcha on the project.
- **The shader build is CMake-driven**: `cmake/rexglue_xenosrecomp.cmake` fetches and
  patches XenosRecomp and defines `renut_xenos_shader_batch()`; the actual conversion
  work is done by `tools/xenos_batch_convert.py` (parallel, per-shader subprocess
  isolation) fed by `tools/batch_build_synthetic_containers.py` (which is where
  `--reject-heuristic` lives) and `tools/xenos_synthesize_constant_table.py`.
- **Ground truth for shader status** is always
  `out/build/linux-amd64-relwithdebinfo/generated/renut_xenos_shader_cache_manifest.txt`
  (`<tag> <status>` per line). §2a is just this file, counted.

---

The throughline: reNut's core blocker (§4, §5) is fundamentally a **data-availability**
problem specific to how Nuts & Bolts' shaders were built and captured, not a problem with
the shared XenosRecomp/XenonRecomp tooling itself, and not (per Xenia's real architecture)
even a fundamental requirement of correctly running Xenos shaders at all — a
positional-linkage approach could sidestep it, at real engineering cost.
