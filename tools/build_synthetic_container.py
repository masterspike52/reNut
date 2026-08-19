#!/usr/bin/env python3
"""Builds a synthetic XenosRecomp ShaderContainer .bin for a shader that was
never captured by shader_dump.cpp's dump_guest_shaders hook (D3D9
CreateVertexShader/CreatePixelShader), because it was loaded directly into
GPU sequencer memory via IM_LOAD/IM_LOAD_IMMEDIATE PM4 packets -- confirmed
a structural gap, not a capture-timing bug (see docs/ai/research.md).

Real raw microcode for such shaders IS available via the SDK's
renut_dump_ucode_hash cvar (shaders_ucode_hash/<vs|ps>_<hash>.ucode), keyed
by the real ucode_data_hash(). This script wraps that real microcode in a
minimal, hand-built ShaderContainer.

Automated end to end: all per-shader vertex-fetch
and texture-fetch metadata (element count, data format, byte offset,
instruction address, sampler fetch-constant/register mapping, interpolator
count, color-output mask) is obtained by running tools/ucode_analyze -- a
standalone binary linking the SDK's own real Shader::AnalyzeUcode() -- against
the real .ucode file, NOT hardcoded per-shader tables. See
tools/ucode_analyze.cpp / tools/build_ucode_analyze.sh.

The one piece AnalyzeUcode cannot supply is D3D9 vertex-element Usage/
UsageIndex (POSITION vs TEXCOORD0 vs COLOR etc.) -- Xenos vfetch instructions
carry no D3D9 semantic name at all, confirmed by direct SDK source
inspection (no DeclUsage/D3DDECLUSAGE inference exists anywhere in this SDK
fork). This is now solved with a second REAL capture, not a guess: the
shader_dump.cpp SetVertexShader/SetVertexDeclaration hooks (new this
session) correlate the guest's own real D3DVERTEXELEMENT9 Usage/UsageIndex
bytes to the exact ucode_data_hash of whichever vertex shader was bound at
the time, writing shaders_vertdecl/vs_<hash>.vertdecl.txt keyed by real
per-element byte offset (matches vertex_bindings()'s offset_words*4 exactly,
no positional assumption). If that file isn't present yet (declaration never
observed bound to this shader in a real play session), this script falls
back to the same documented POSITION-first/TEXCOORD-follows heuristic used
for the one shader pair proven by hand earlier this session -- flagged
clearly in its output, not silently treated as equally trustworthy.

Usage: build_synthetic_container.py <ucode_dir> <vs|ps> <hash> <output.bin>
         [--vertdecl-dir DIR] [--ucode-analyze PATH]
  <ucode_dir>: directory containing <vs|ps>_<hash>.ucode files
  <vs|ps>: which shader type to build (vertex or pixel container layout differ)
  <hash>: the target shader's real ucode_data_hash, hex, no 0x prefix
  <output.bin>: where to write the synthetic container
  --vertdecl-dir: directory containing vs_<hash>.vertdecl.txt files
                  (default: <ucode_dir>/../shaders_vertdecl)
  --ucode-analyze: path to the built ucode_analyze binary
                    (default: alongside this script's repo, /tmp/ucode_analyze,
                    or built on demand via build_ucode_analyze.sh)
"""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

# Real XenosRecomp DeclUsage enum values (XenosRecomp/shader.h).
DECL_USAGE_POSITION = 0
DECL_USAGE_NORMAL = 3
DECL_USAGE_TEXCOORD = 5
DECL_USAGE_TANGENT = 6
DECL_USAGE_BINORMAL = 7
DECL_USAGE_COLOR = 10

# Real XenosRecomp PixelShaderOutputs bits (shader.h) -- COLOR0-3 come from
# ucode_analyze's writes_color_targets bitmask directly (same bit
# positions), DEPTH does not (it's a separate boolean, writes_depth).
PIXEL_SHADER_OUTPUT_DEPTH = 0x10

# Real Xenos xenos::VertexFormat numeric IDs (include/rex/graphics/xenos.h),
# not just the ones this script otherwise names -- used ONLY for the
# format-aware heuristic below, matching the raw values ucode_analyze.cpp's
# "data_format" JSON field already reports.
_FORMAT_10_11_11 = 16
_FORMAT_11_11_10 = 17
_FORMAT_8_8_8_8 = 6
# NOT one of XenosRecomp's own documented R11G11B10-only special-unpack
# formats (README.md's Vertex Fetch section is explicit: only 10_11_11/
# 11_11_10 get manual unpacking) -- but a real, consistent pattern was found
# by sampling this game's actual captured shaders: 2-3 consecutive
# 2_10_10_10 attributes immediately after POSITION, matching the exact
# normal/tangent/binormal SHAPE, recurs across many real shaders (sampled:
# vs_0112bc639335c58a, vs_054f86bb3d84dbb9, vs_07462764505df374, etc all
# show this). Treating it the same as the documented normal formats
# is a REAL INFERENCE from observed data, not a documented XenosRecomp
# convention -- it gets the Vulkan input LOCATION right (location 1, per
# USAGE_LOCATIONS) even though XenosRecomp won't apply special R11G11B10
# unpacking (this format isn't eligible for that regardless of usage
# labeling); the vertex shader's own real ucode is responsible for
# interpreting the raw packed bits either way, same as our own
# VertexFormatToVkFormat already does via VK_FORMAT_A2B10G10R10_*_PACK32.
_FORMAT_2_10_10_10 = 7

REGISTER_SET_BOOL = 0
REGISTER_SET_FLOAT4 = 2
REGISTER_SET_SAMPLER = 3


def build_real_constant_table(float_regs: list[int], sampler_regs: list[int],
                              bool_regs: list[int] | None = None,
                              is_pixel_shader: bool = False) -> bytes:
    """Real D3DXSHADER_CONSTANTTABLE bytes (whole ConstantTableContainer,
    including its own leading `size` field) listing one ConstantInfo entry
    per float4/sampler/bool register a shader actually reads (from
    ucode_analyze's float_constants/texture_bindings/bool_constants).
    XenosRecomp doesn't care about real constant NAMES (only registerSet/
    registerIndex/registerCount matter for the #defines it emits, see
    shader_recompiler.cpp's recompile()), so synthetic names are fine --
    same real technique as tools/xenos_synthesize_constant_table.py, shared
    here since a synthetic container with an EMPTY constant table (this
    script's original behavior) fails XenosRecomp identically to a real
    capture with constantTableOffset == 0: "use of undeclared identifier
    'c45'"/'b243' etc for every float/sampler/bool register the shader
    references. Without a real RegisterSet::Bool entry, XenosRecomp falls
    back to emitting a bare, never-declared `b<N>` identifier for boolean
    control-flow conditions (shader_recompiler.cpp's ControlFlowOpcode::
    kCondJmp handling) instead of the correct `g_Booleans & (1 << N)` form
    -- confirmed via direct source reading, not guessed.

    the float4 entries used to be one ConstantInfo
    PER REGISTER (registerCount=1 each). shader_recompiler.cpp's recompile()
    (the ALU operand-formatting code) only emits the safe, bounds-clamped
    indexed accessor `constantName(index [+ a0|+ aL])` -- the ONLY form that
    can express Xenos's real dynamic/relative constant addressing (`c[a0+N]`,
    `c[aL+N]`, used pervasively for skeletal-animation/skinning bone-matrix
    lookups) -- when the MATCHED ConstantInfo has registerCount > 1
    (shader_recompiler.cpp:1205 branches on exactly this). With
    registerCount always 1, every such shader instead hit the plain-name
    fallback branch (shader_recompiler.cpp:479, guarded by
    `assert(!instr.const0Relative && !instr.const1Relative)` -- compiles to
    nothing in a release build), which silently DROPS the +a0/+aL offset
    entirely and always reads the same static register regardless of the
    real runtime index -- a confirmed, real, structural bug: any shader
    doing dynamic constant addressing (skinning is the classic real-world
    case) reads garbage/wrong bone data for every vertex, a strong match for
    reported shattered/scattered-looking animated geometry. Fixed by
    emitting ONE grouped float4 entry spanning the full real register range
    (0..255 for vertex shaders, 0..223 for pixel shaders -- matching
    shader_recompiler.cpp:1207's own `(isPixelShader ? 224 : 256)` tail-count
    ceiling exactly) instead of one entry per individually-referenced
    register -- XenosRecomp's own emitted macro already safely clamps/zero-
    fills any index outside what's actually live, so this is not a
    correctness risk for shaders that never use dynamic addressing, only a
    (harmless) widening of the declared range.
    """
    # shader_recompiler.cpp's own
    # `tailCount = (isPixelShader ? 224 : 256) - registerIndex` is baked
    # into the grouped macro it emits REGARDLESS of what registerCount this
    # script declares -- so a grouped entry covering a pixel-shader register
    # >= 224 wouldn't fail to compile, it would silently ALWAYS return 0.0
    # for that register (even for a plain static reference), since the
    # clamp is unconditional. 224 also matches this hardware's real,
    # documented pixel-shader float-constant register-file size (vertex
    # shaders get 256), so route any actually-used register >= that ceiling
    # through an individual, real registerCount=1 entry instead (the
    # ORIGINAL, proven-correct per-register scheme -- no dynamic-addressing
    # support for it, but also no incorrect zero-clamp). Confirmed real
    # regression this fixes: a heuristic-captured pixel shader statically
    # referencing register 255 previously got excluded from BOTH the
    # grouped range AND any per-register fallback, so XenosRecomp fell
    # through to its "not found in float4Constants" path and emitted a
    # bare, never-#define'd `c255` identifier -- a real DXC "undeclared
    # identifier" compile failure that didn't exist before this
    # constant-table rework.
    float4_count = 224 if is_pixel_shader else 256
    entries = [("c", REGISTER_SET_FLOAT4, 0, float4_count)] if any(r < float4_count for r in float_regs) else []
    entries += [(f"c{r}", REGISTER_SET_FLOAT4, r, 1) for r in sorted(float_regs) if r >= float4_count]
    entries += [(f"s{r}", REGISTER_SET_SAMPLER, r, 1) for r in sampler_regs]
    entries += [(f"b{r}", REGISTER_SET_BOOL, r, 1) for r in (bool_regs or [])]

    CONSTANT_TABLE_HEADER_SIZE = 28
    CONSTANT_INFO_SIZE = 20
    constant_info_offset = CONSTANT_TABLE_HEADER_SIZE
    names_offset = constant_info_offset + len(entries) * CONSTANT_INFO_SIZE

    name_bytes = b""
    name_offsets = []
    for name, _, _, _ in entries:
        name_offsets.append(names_offset + len(name_bytes))
        name_bytes += name.encode("ascii") + b"\x00"

    constant_table_size = names_offset + len(name_bytes)

    out = bytearray()
    out += struct.pack(">I", 4 + constant_table_size)
    out += struct.pack(">IIIIIII", constant_table_size, 0, 0, len(entries),
                        constant_info_offset, 0, 0)
    for i, (name, regset, regidx, regcount) in enumerate(entries):
        out += struct.pack(">IHHHHII", name_offsets[i], regset, regidx, regcount, 0, 0, 0)
    out += name_bytes
    return bytes(out)


def run_ucode_analyze(ucode_analyze_path: Path, shader_kind: str, ucode_path: Path) -> dict:
    result = subprocess.run(
        [str(ucode_analyze_path), shader_kind, str(ucode_path)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"build_synthetic_container: ucode_analyze failed for {ucode_path}:\n{result.stderr}",
              file=sys.stderr)
        sys.exit(1)
    return json.loads(result.stdout)


def find_or_build_ucode_analyze(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    candidates = [
        Path("/tmp/ucode_analyze"),
        Path(__file__).parent / "ucode_analyze_bin",
    ]
    for c in candidates:
        if c.exists():
            return c
    # Build it on demand -- real, not a stub: invokes the same script a human
    # would run, so a fresh checkout works without a separate manual step.
    build_script = Path(__file__).parent / "build_ucode_analyze.sh"
    out_path = Path("/tmp/ucode_analyze")
    print(f"build_synthetic_container: building ucode_analyze via {build_script}...", file=sys.stderr)
    result = subprocess.run(["sh", str(build_script), str(out_path)], capture_output=True, text=True)
    if result.returncode != 0 or not out_path.exists():
        print(f"build_synthetic_container: failed to build ucode_analyze:\n{result.stdout}\n{result.stderr}",
              file=sys.stderr)
        sys.exit(1)
    return out_path


def parse_vertdecl_file(path: Path) -> list[dict]:
    """Parses a real shaders_vertdecl/vs_<hash>.vertdecl.txt file (written by
    shader_dump.cpp's dumpVertexDeclUsageForBoundShader), one real captured
    D3D9 element per line: 'offset=<n> type=<hex> usage=<n> usageIndex=<n>'.
    """
    elements = []
    for line in path.read_text().splitlines():
        parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
        if "offset" not in parts:
            continue
        elements.append({
            "offset": int(parts["offset"]),
            "usage": int(parts["usage"]),
            "usage_index": int(parts["usageIndex"]),
        })
    return elements


def assign_usage_from_vertdecl(attributes: list[dict], vertdecl_elements: list[dict]) -> bool:
    """Matches real captured D3D9 elements to AnalyzeUcode's attributes by
    real byte offset (attribute's offset_words*4 == captured element's
    offset) -- both describe the same underlying vertex buffer layout, offset
    is the natural join key, no assumption that array order matches. Returns
    True if every attribute got a real match.
    """
    by_offset = {e["offset"]: e for e in vertdecl_elements}
    matched_all = True
    for attr in attributes:
        real = by_offset.get(attr["offset_words"] * 4)
        if real is None:
            matched_all = False
            continue
        attr["usage"] = real["usage"]
        attr["usage_index"] = real["usage_index"]
        attr["usage_source"] = "captured"
    return matched_all


def assign_usage_heuristic(attributes: list[dict]) -> None:
    """Fallback when no real captured D3D9 declaration is available yet.

    REAL FIX (second pass): the original version of this
    heuristic (POSITION-first, everything else TEXCOORD0/1/2/...) was only
    ever verified against the ONE shader this whole native-renderer effort
    started with. Deployed broadly (1500+ shaders) it produced confirmed,
    real in-game geometry corruption -- root-caused to XenosRecomp's own
    FIXED usage->Vulkan-location table (shader_recompiler.cpp's
    USAGE_LOCATIONS: Position=0, Normal=1, Tangent=2, Binormal=3,
    TexCoord0=4, ..., Color0=8): mislabeling a real NORMAL attribute as
    TexCoord0 doesn't just get the wrong NAME, it binds that attribute to
    Vulkan input location 4 instead of the correct location 1 -- a real,
    structural vertex-wiring mismatch, not a cosmetic one.

    This version adds a second, real signal beyond ordering: the vertex
    fetch's own `data_format` (already decoded by ucode_analyze from real
    ucode, no guessing) strongly implies usage on this hardware, per
    XenosRecomp's own documented conventions (README.md's Vertex Fetch
    section): packed 10_11_11/11_11_10 formats are used almost exclusively
    for NORMAL/TANGENT/BINORMAL, and 8_8_8_8 is the classic vertex-color
    format. This is STILL an inference, not a capture -- correct for common
    cases, not guaranteed for every shader -- but strictly more informed
    than pure positional ordering. Attributes are still marked
    usage_source="heuristic" either way so callers (e.g.
    --reject-heuristic) can still treat this as unverified.
    """
    # offset_words == 0 (not list position) is the real, reliable POSITION
    # signal -- it's always the first attribute in the buffer regardless of
    # whether this list is the full attribute set or a partial-match
    # remainder (assign_usage_from_vertdecl may have already claimed real
    # usage for some attributes before this function ever sees the rest).
    # RE-ENABLED (same session): consuming a 2nd/3rd
    # consecutive packed-normal-shaped attribute as TANGENT/BINORMAL was
    # tried, made things measurably WORSE, and was reverted -- but the real
    # root cause was NOT this heuristic itself, it was a separate,
    # pre-existing bug in renut_xenos_pipeline_cache.cpp's
    # GetOrCreatePipeline(): Vulkan vertex input LOCATIONS were assigned
    # purely sequentially, but XenosRecomp compiles the shader expecting
    # FIXED locations per D3D9 usage (shader_recompiler.cpp's
    # USAGE_LOCATIONS: Position=0, Normal=1, Tangent=2, Binormal=3,
    # TexCoord0=4, ...) -- confirmed via real Vulkan validation output
    # ("pVertexAttributeDescriptions does not have a Location 4, but
    # [VK_SHADER_STAGE_VERTEX_BIT] has [Input variable, Location 4] at that
    # Location"). That bug is now FIXED: tools/xenos_cache_unpack.cpp's
    # ExtractVertexLocations() computes the real per-attribute location
    # (using the SAME USAGE_LOCATIONS table) from this container's own
    # real vertex-element data at build time, stores it in
    # RenutXenosShaderCacheEntry.vertexLocations[], and
    # renut_xenos_pipeline_cache.cpp's GetOrCreatePipeline() now reads it
    # instead of guessing sequentially. With that fixed, mislabeling
    # TANGENT/BINORMAL as TEXCOORD (this heuristic's ORIGINAL bug) is safe
    # to fix again -- it gets the correct location either way now, the only
    # remaining risk is the USAGE NAME itself being wrong for shaders where
    # a 2nd/3rd packed attribute isn't really tangent/binormal.
    _NORMAL_LIKE_USAGES = [DECL_USAGE_NORMAL, DECL_USAGE_TANGENT, DECL_USAGE_BINORMAL]
    _NORMAL_LIKE_FORMATS = (_FORMAT_10_11_11, _FORMAT_11_11_10, _FORMAT_2_10_10_10)
    # See build_vertex_container's own comment on binding_index:
    # XenosRecomp deduplicates/names vertex-shader input
    # parameters purely by (usage, usageIndex) -- shader_recompiler.cpp's
    # `semantic = usage<<4 | usageIndex` -- GLOBALLY across the whole
    # shader, not per real vertex-fetch binding. So every usage_index
    # counter here (position, texcoord, and each of the 3 normal-like
    # slots) must keep incrementing across binding boundaries too, or a
    # second binding's own "first/second/third attribute" collides with
    # the same (usage, usageIndex) pair binding 0 already used -- the
    # exact real bug this fix addresses (confirmed crash: XenosRecomp's
    # generated HLSL declared duplicate `iPosition0` parameters for a
    # shader with 2 real bindings, DXC's compile failed, and XenosRecomp's
    # own zero error-handling on that turned it into a silent SIGSEGV).
    # position_usage_index in particular gives a second binding's own
    # offset-0 attribute POSITION1 instead of a colliding POSITION0 -- a
    # real, XenosRecomp-recognized case (shader_recompiler.cpp's
    # isMetaInstancer check), used for real instancing/skinning-style
    # secondary vertex streams, exactly this fix's real repro shape (a
    # second binding with many repeating float4 attributes, real
    # bone-matrix-shaped data).
    # Same class of bug as the position_usage_index comment above:
    # COLOR was still
    # hardcoded to usage_index=0 unconditionally, unlike TEXCOORD (which
    # already incremented). Any shader with 2+ real 8_8_8_8-shaped
    # attributes (confirmed real crash: "redefinition of parameter
    # 'iColor0'") hit the identical duplicate-parameter/DXC-compile-
    # failure/XenosRecomp-SIGSEGV chain. XenosRecomp supports a real
    # Color1 (USAGE_LOCATIONS has { DeclUsage::Color, 1, 11 }), so this
    # mirrors position_usage_index's fix exactly.
    position_usage_index = 0
    texcoord_index = 0
    color_index = 0
    normal_like_run_index = 0
    for attr in attributes:
        if attr["offset_words"] == 0:
            attr["usage"] = DECL_USAGE_POSITION
            attr["usage_index"] = position_usage_index
            attr["usage_source"] = "heuristic"
            position_usage_index += 1
            continue
        fmt = attr.get("data_format")
        if fmt in _NORMAL_LIKE_FORMATS and normal_like_run_index < len(_NORMAL_LIKE_USAGES):
            attr["usage"] = _NORMAL_LIKE_USAGES[normal_like_run_index]
            attr["usage_index"] = 0
            normal_like_run_index += 1
        elif fmt == _FORMAT_8_8_8_8:
            attr["usage"] = DECL_USAGE_COLOR
            attr["usage_index"] = color_index
            color_index += 1
        else:
            attr["usage"] = DECL_USAGE_TEXCOORD
            attr["usage_index"] = texcoord_index
            texcoord_index += 1
        attr["usage_source"] = "heuristic"


class HeuristicRejected(Exception):
    """Raised when reject_heuristic=True and any vertex attribute needed the
    fallback usage heuristic -- an opt-in filter (the heuristic corrupts
    real geometry for shaders whose layout isn't POSITION-then-TEXCOORDs,
    confirmed via in-game visual corruption once ~150 heuristic-based
    shaders went live simultaneously; vertex-declaration capture is
    structurally unavailable for these shaders, same IM_LOAD root cause as
    shader creation itself -- see docs/ai/research.md), so callers that
    only want verified-correct
    containers can skip the rest instead of emitting a wrong one."""


class MissingLiteralConstantData(Exception):
    """raised when this shader's real,
    ucode_analyze-detected float_constants includes a register in the real,
    empirically-confirmed "compiler-embedded literal constant" range
    (>= 252) -- checked directly against REAL captured shaders (shaders/*.bin
    that DO have a real DefinitionTable): every one found covers exactly
    registers 252-255, and the real embedded VALUES there are genuinely
    shader-specific authored numbers (e.g. one real shader's real data was
    (1.0, -1428.57, 0.0, 0.0), another's was (0.5, 0.25, 1.6, 0.0) -- no
    universal default exists to fall back to). A game shader compiled this
    way expects these registers' values to come from its own bytecode's
    DefinitionTable (a real, separate mechanism XenosRecomp's own
    shader_recompiler.cpp emits as a LOCAL VARIABLE, not the runtime
    constant buffer this script's synthesized ConstantTable feeds), and a
    shader that was only ever captured via raw IM_LOAD microcode (this
    script's whole reason to exist) has NO DefinitionTable at all --
    structurally, information-theoretically unrecoverable, not a bug to fix
    by guessing. Confirmed real, in-the-wild symptom this explains: several
    user-marked-broken shaders (objects that silently fail to render/
    "don't spawn") reference exactly this register range with no real
    declaration data available. Gracefully skip instead of shipping a
    shader fed a garbage literal it was never designed to receive at
    runtime -- falls back to the stock renderer, which DOES have the real
    bytecode (and therefore the real DefinitionTable) via the normal D3D9
    runtime-translation path."""


# Real, empirically-confirmed threshold (see MissingLiteralConstantData's own
# docstring) -- every real DefinitionTable checked across this game's
# captured shaders covers exactly registers 252-255, never lower.
_LITERAL_CONSTANT_REGISTER_THRESHOLD = 252


def check_for_missing_literal_constants(float_regs: list[int]) -> None:
    bad = [r for r in float_regs if r >= _LITERAL_CONSTANT_REGISTER_THRESHOLD]
    if bad:
        raise MissingLiteralConstantData(
            f"references float constant register(s) {bad} with no real DefinitionTable "
            "available to supply their real, shader-specific literal values")


_SHADER_STRUCT = struct.Struct(">IIIII")  # physicalOffset,size,field8,fieldC,field10 (interpolatorInfo read separately)


def read_ps_interpolator_usages(ps_container_path: Path) -> dict[int, tuple[int, int]]:
    """Reads a REAL, D3D9-captured pixel shader container's own
    interpolators[] array (PixelShader::interpolators, shader.h) and
    returns {register: (usage, usage_index)} for each entry.

    Real idea this exists for (from external research into
    Xenia's architecture -- see renut_shader_conversion_blockers.md
    memory): a vertex shader export register N and the paired pixel
    shader's import register N must carry the SAME real usage/usageIndex
    for the pair to function at all on real hardware -- confirmed via
    XenosRecomp's own shader_recompiler.cpp (PS side reads
    `PixelShader::interpolators[i]`, decodes the SAME Interpolator bitfield
    union `usageIndex:4, usage:4, reg:4` this file's own
    pack_interpolator() writes for VS). So a REAL captured PS container
    (shaders/ps_<hash>.bin, from the D3D9 hook path -- NOT one WE
    synthesized, since build_pixel_container() invents its own numeric
    interpolator convention rather than real usage data) can supply real
    ground truth for what a paired, uncaptured VS's export registers
    should be named, when the VS's own real data is missing.

    Only useful when the target PS genuinely has a real container with
    constantTableOffset/interpolator data intact -- returns {} (not an
    error) if the container looks synthesized/empty, so callers can fall
    back to the existing heuristic cleanly.
    """
    if not ps_container_path.exists():
        return {}
    data = ps_container_path.read_bytes()
    if len(data) < 36:
        return {}
    flags, virtual_size, physical_size, field_c, ctbl_off, dtbl_off, shader_off, f1c, f20 = \
        struct.unpack(">9I", data[0:36])
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return {}
    if shader_off + 24 > len(data):
        return {}
    # Shader base struct (24B): physicalOffset,size,field8,fieldC,field10,interpolatorInfo
    interpolator_info = struct.unpack(">I", data[shader_off + 20:shader_off + 24])[0]
    interpolator_count = (interpolator_info >> 5) & 0x1F
    if interpolator_count == 0:
        return {}
    # PixelShader adds field18(4B), outputs(4B) before interpolators[] begins.
    interpolators_offset = shader_off + 24 + 8
    if interpolators_offset + interpolator_count * 4 > len(data):
        return {}
    result: dict[int, tuple[int, int]] = {}
    for i in range(interpolator_count):
        word = struct.unpack_from(">I", data, interpolators_offset + i * 4)[0]
        usage_index = word & 0xF
        usage = (word >> 4) & 0xF
        reg = (word >> 8) & 0xF
        result[reg] = (usage, usage_index)
    return result


def build_vertex_container(ucode_bytes: bytes, analysis: dict, vertdecl_elements: list[dict] | None,
                           reject_heuristic: bool = False,
                           ps_interpolator_usages: dict[int, tuple[int, int]] | None = None) -> bytes:
    if not analysis["vertex_bindings"]:
        print("build_synthetic_container: no vertex_bindings found by ucode_analyze -- "
              "this shader has no real vfetch instructions, cannot build a vertex container",
              file=sys.stderr)
        sys.exit(1)
    # A shader normally has exactly one real vertex-fetch binding group in
    # this game (confirmed for the one proven shader; a shader that legitimately
    # uses multiple bindings would need every attribute across all of them,
    # which this flattens rather than dropping).
    #
    # Real bug found+fixed: binding_index is now tracked and
    # carried through -- it did NOT exist before, and attributes were
    # sorted by offset_words GLOBALLY across every binding combined. Since
    # offset_words is relative to EACH BINDING's own stream (not a single
    # shared buffer), a second binding's first attribute also has
    # offset_words==0, so it sorted next to binding 0's real position
    # attribute and the heuristic (which keys "is this POSITION" purely off
    # offset_words==0) labeled BOTH as POSITION0 -- a real, confirmed
    # crash: XenosRecomp's generated HLSL declared the same `iPosition0`
    # parameter twice ("redefinition of parameter 'iPosition0'"), DXC's
    # compile failed, and XenosRecomp's own zero error-handling on that
    # turned it into a silent SIGSEGV. Sorting is now done PER BINDING
    # (stable sort preserves binding order, each binding's own attributes
    # stay sorted by their own offset), and assign_usage_heuristic below
    # uses binding_index to only ever treat ONE attribute per binding as a
    # "position-like" (offset 0) attribute, assigning DECL_USAGE_POSITION
    # with usage_index 0 for the first binding and usage_index 1 for a
    # second (XenosRecomp has a real, recognized Position1 case -- see
    # shader_recompiler.cpp's isMetaInstancer check -- used for real
    # instancing/skinning-style secondary vertex streams, exactly the
    # shape this fix was found against: a second binding with many
    # repeating float4 attributes, real bone-matrix-shaped data).
    attributes = []
    for binding_index, vb in enumerate(analysis["vertex_bindings"]):
        for a in vb["attributes"]:
            attributes.append({
                "instr_addr": a["instruction_address"],
                "data_format": a["data_format"],
                "offset_words": a["offset_words"],
                "binding_index": binding_index,
            })
    attributes.sort(key=lambda a: (a["binding_index"], a["offset_words"]))

    matched_all = False
    if vertdecl_elements:
        matched_all = assign_usage_from_vertdecl(attributes, vertdecl_elements)
        if not matched_all:
            print("build_synthetic_container: WARNING vertdecl file present but did not "
                  "cover every real vfetch attribute by offset -- filling gaps with heuristic",
                  file=sys.stderr)
    if not vertdecl_elements or not matched_all:
        # Only heuristic-fill attributes still missing usage (partial real
        # coverage is trusted where it exists).
        missing = [a for a in attributes if "usage" not in a]
        if missing:
            assign_usage_heuristic(missing)
    for a in attributes:
        a.setdefault("usage_source", "captured")

    if reject_heuristic and any(a["usage_source"] == "heuristic" for a in attributes):
        raise HeuristicRejected()

    interpolator_count = bin(analysis["writes_interpolators"]).count("1")

    # VertexElement and Interpolator have DIFFERENT real on-disk bit layouts
    # -- found EMPIRICALLY using a debug build of the real
    # XenosRecomp binary (fprintf added at each read site in
    # shader_recompiler.cpp), tested against a real container with single-
    # nibble/field-set test values swept across every bit position. This
    # matters because `be<uint32_t>` (XenosRecomp/pch.h) unconditionally
    # byte-swaps on every read via its `get()`/implicit-conversion operator
    # with no "already native" guard, so naively packing bitfields in their
    # STRUCT-DECLARED order and writing them as a plain big-endian integer
    # does not universally work the same way for every struct -- verify
    # against a real debug build before changing either formula below.
    def pack_vertex_element(address, usage, usage_index):
        return (address & 0xFFF) | ((usage & 0xF) << 12) | ((usage_index & 0xF) << 16)

    def pack_interpolator(usage, usage_index, reg=0):
        return (usage_index & 0xF) | ((usage & 0xF) << 4) | ((reg & 0xF) << 8)

    vertex_elements = [
        pack_vertex_element(a["instr_addr"], a["usage"], a["usage_index"]) for a in attributes
    ]

    # Interpolator OUTPUTS are a completely
    # different concept from vertex INPUTS (confirmed via direct
    # shader_recompiler.cpp reading: XenosRecomp's VS function signature
    # declares its interpolator outputs from a FIXED, global INTERPOLATORS[]
    # table -- Normal0-15/Tangent0/Binormal0/TexCoord0-15/Color0-1 ONLY,
    # Position is not a valid interpolator usage at all -- and VS/PS
    # interpolator arrays are wired together PURELY BY ARRAY INDEX, not by
    # any register-number field; usage/usageIndex only control the
    # generated HLSL variable NAME on each side, matched positionally).
    # Building interpolator entries from vertex INPUT attributes (the old
    # behavior below) fabricates entries with no relationship to what the
    # shader's ALU code actually exports, and real shaders often need far
    # fewer/more/different interpolators than they have vertex inputs
    # (confirmed real crash: a shader with 14 non-position inputs but only
    # 5 real interpolator writes).
    #
    # If a REAL captured pixel shader's own interpolator array is
    # available (ps_interpolator_usages, see read_ps_interpolator_usages()
    # -- only populated for shaders with a genuine D3D9-captured PS
    # container, not one we synthesized), copy IT verbatim as this VS's
    # own interpolator array instead of guessing from inputs. This makes
    # the VS's array structurally IDENTICAL (same length, same per-index
    # usage/usageIndex) to what the PS expects at each index -- since
    # XenosRecomp wires them together by index, this guarantees correct
    # linkage regardless of whether the usage NAME is "semantically real"
    # for this specific shader.
    # Real, correct algorithm (replaces the vertex-input-based
    # guess above and the popcount-based interpolator_count from earlier):
    # confirmed via direct source reading (shader_code.h's ExportRegister
    # enum: VSInterpolator0..15 == literal values 0..15) that a vertex
    # shader's real ALU export register number IS the interpolator array
    # INDEX XenosRecomp looks the export up by (shader_recompiler.cpp:650's
    # `interpolators.find(instr.vectorDest)`, where the map was built by
    # `interpolators.emplace(i, ...)` for array index i, line ~1508) -- and
    # the SDK's own writes_interpolators() is a REAL BITMASK (confirmed:
    # translator.cpp's GatherAluResultInformation sets
    # `writes_interpolators_ |= 1 << result.storage_index` for
    # kInterpolator writes), bit N meaning "this shader's ALU code writes
    # export register N", not just a count. Popcount (the old code)
    # collapses this real per-slot information into a single number,
    # losing exactly the data needed to build a correct, non-sparse
    # 0..count-1 array (XenosRecomp reads the array in a dense loop, no
    # gaps possible -- shader_recompiler.cpp:1490).
    #
    # Correct construction: interpolator_count is (highest set bit + 1),
    # and EVERY array position 0..count-1 needs a real, distinct
    # (usage, usageIndex) pair from the fixed INTERPOLATORS[] table
    # (Normal0-15/Tangent0/Binormal0/TexCoord0-15/Color0-1, 34 real slots,
    # Position is NOT valid here) -- including "gap" positions the shader
    # never actually writes to, since XenosRecomp default-initializes
    # those to 0.0 rather than needing them omitted. The usage NAME
    # assigned to each position doesn't need to be "semantically real" (it
    # only becomes the generated HLSL variable name, used consistently
    # within this one shader's own code) -- it just needs to be present
    # and distinct, so this assigns them in INTERPOLATORS[] table order,
    # which is always valid regardless of what the shader actually does
    # with each slot.
    # Must match XenosRecomp's OWN real INTERPOLATORS[] table exactly
    # (shader_recompiler.cpp, upstream commit 990d03b -- verified directly
    # against the actual _deps/xenosrecomp-src checkout CMake fetches, not
    # assumed): only TexCoord0-15 + Color0-1, 18 entries total. There is NO
    # Normal/Tangent/Binormal entry -- those are valid VERTEX INPUT usages
    # (a completely separate table) but NOT valid VS output/interpolator
    # usages upstream. An earlier version of this table wrongly included
    # them (confirmed by cross-referencing a local experimental checkout
    # that turned out to have unrelated, non-upstream, non-shipped patches
    # rewriting the whole constant-buffer layout) which produced containers
    # referencing e.g. "oNormal0" as a VS output -- undeclared in the real
    # generated HLSL, causing DXC to emit compile errors and, worse,
    # segfault outright on the resulting malformed source.
    _INTERPOLATOR_USAGE_TABLE = (
        [(DECL_USAGE_TEXCOORD, i) for i in range(16)] +
        [(DECL_USAGE_COLOR, 0), (DECL_USAGE_COLOR, 1)]
    )

    if ps_interpolator_usages:
        ordered = [ps_interpolator_usages[reg] for reg in sorted(ps_interpolator_usages)]
        interpolators = [pack_interpolator(usage, usage_index) for usage, usage_index in ordered]
        interpolator_count = len(interpolators)
        print(f"build_synthetic_container: NOTE using {len(interpolators)} real interpolator "
              "usage(s) from a captured pixel shader container instead of guessing from vertex "
              "inputs", file=sys.stderr)
    else:
        writes_mask = analysis["writes_interpolators"]
        interpolator_count = (writes_mask.bit_length()) if writes_mask else 0
        if interpolator_count > len(_INTERPOLATOR_USAGE_TABLE):
            print(f"build_synthetic_container: WARNING interpolator_count={interpolator_count} "
                  f"exceeds the {len(_INTERPOLATOR_USAGE_TABLE)} real usage slots XenosRecomp's "
                  "own INTERPOLATORS[] table supports -- clamping so the container's declared "
                  "count and its physically-written array stay consistent (an uncapped count "
                  "here previously caused XenosRecomp to read past the end of the array).",
                  file=sys.stderr)
            interpolator_count = len(_INTERPOLATOR_USAGE_TABLE)
        interpolators = [
            pack_interpolator(usage, usage_index)
            for usage, usage_index in _INTERPOLATOR_USAGE_TABLE[:interpolator_count]
        ]

    field18 = 0
    vertex_element_count = len(vertex_elements)
    field20 = 0
    interpolator_info = (interpolator_count & 0x1F) << 5

    vertex_shader_header = struct.pack(
        ">IIIIIIII",
        0, len(ucode_bytes), 0, 0, 0, interpolator_info, field18, vertex_element_count,
    ) + struct.pack(">I", field20)
    # REAL BUG FOUND AND FIXED (see git history / memory for full writeup):
    # vertexElementsAndInterpolators is be<uint32_t>[], whose get()
    # unconditionally byte-swaps -- pack as big-endian here so the runtime's
    # own swap produces the intended value.
    vertex_shader_header += struct.pack(f">{len(vertex_elements)}I", *vertex_elements)
    vertex_shader_header += struct.pack(f">{len(interpolators)}I", *interpolators)

    # A shader using ONLY dynamic (a0/aL-
    # relative) float constant addressing has NO entries in float_constants
    # at all (AnalyzeUcode only records STATIC register references there --
    # see translator.cpp's GatherOperandInformation, which sets
    # float_dynamic_addressing instead of touching float_bitmap for a
    # relative-addressed operand) -- an empty float_regs list means
    # build_real_constant_table's grouped-entry gate (`any(r < float4_count
    # for r in float_regs)`) never fires, so `c(N + a0)` has no `#define
    # c(...)` to resolve to. Matches xenos_synthesize_constant_table.py's
    # own real fix for the identical gap.
    float_constants = analysis.get("float_constants", [])
    # check BEFORE the dynamic-addressing expansion
    # below -- that expansion legitimately claims the full 0-255 range for
    # an unrelated reason (the runtime can't know which specific registers
    # a dynamic a0/aL-relative access might touch), which would otherwise
    # make EVERY dynamically-addressed shader a false positive here. Only a
    # real, STATIC reference to a literal-constant-range register is
    # meaningful evidence this shader needs data we don't have.
    check_for_missing_literal_constants(float_constants)
    if analysis.get("float_dynamic_addressing"):
        float_constants = list(range(256))
    constant_table_container = build_real_constant_table(
        float_constants, [], analysis.get("bool_constants", []),
        is_pixel_shader=False)

    header_size = 36
    constant_table_offset = header_size
    shader_offset = constant_table_offset + len(constant_table_container)
    virtual_size = shader_offset + len(vertex_shader_header)
    padding = (-virtual_size) % 4
    virtual_size += padding
    physical_size = len(ucode_bytes)

    shader_container = struct.pack(
        ">IIIIIIIII",
        0x102A1101, virtual_size, physical_size, 0, constant_table_offset, 0, shader_offset, 0, 0,
    )

    usage_sources = {a["usage_source"] for a in attributes}
    if "heuristic" in usage_sources:
        print(f"build_synthetic_container: NOTE {sum(1 for a in attributes if a['usage_source']=='heuristic')}"
              f"/{len(attributes)} vertex element(s) used the fallback usage heuristic, not a real capture "
              "-- verify shaders_vertdecl/*.vertdecl.txt coverage if this shader renders incorrectly",
              file=sys.stderr)

    return (shader_container + constant_table_container + vertex_shader_header +
            b"\x00" * padding + ucode_bytes)


def build_pixel_container(ucode_bytes: bytes, analysis: dict) -> bytes:
    interpolator_count = bin(analysis["writes_interpolators"]).count("1")
    # Fixed convention (confirmed via direct SDK source inspection, not
    # per-shader disassembly): interpolator index N is always read from ALU
    # register N by the compiled pixel shader (spirv_translator.cpp's
    # input_output_interpolators_[interpolator_index]) -- so this needs no
    # per-texture src_register lookup at all, unlike the version of this
    # script that hand-transcribed "reg" from real disassembly text earlier
    # this session.
    interpolators = [
        {"usage": DECL_USAGE_TEXCOORD, "usage_index": i, "reg": i}
        for i in range(interpolator_count)
    ]

    def pack_interpolator(usage, usage_index, reg):
        return (usage_index & 0xF) | ((usage & 0xF) << 4) | ((reg & 0xF) << 8)

    interpolator_words = [pack_interpolator(it["usage"], it["usage_index"], it["reg"]) for it in interpolators]

    field18 = 0
    interpolator_info = (len(interpolators) & 0x1F) << 5
    # Real bug found+fixed: only writes_color_targets bits were
    # ever set here -- PIXEL_SHADER_OUTPUT_DEPTH was never included even
    # for shaders that genuinely write oDepth (real manual-depth-write
    # shaders, confirmed via real DXC "use of undeclared identifier
    # 'oDepth'" compile failures -- XenosRecomp's shader_recompiler.cpp
    # only DECLARES the oDepth output parameter when this bit is set,
    # regardless of whether the shader's own instructions reference it).
    outputs_mask = analysis["writes_color_targets"]
    if analysis.get("writes_depth"):
        outputs_mask |= PIXEL_SHADER_OUTPUT_DEPTH

    pixel_shader_header = struct.pack(
        ">IIIIIIII",
        0, len(ucode_bytes), 0, 0, 0, interpolator_info, field18, outputs_mask,
    )
    pixel_shader_header += struct.pack(f">{len(interpolator_words)}I", *interpolator_words)

    sampler_regs = sorted({tb["fetch_constant"] for tb in analysis["texture_bindings"]})
    # See the vertex-shader call site's own comment -- same gap,
    # pixel-shader side.
    float_constants = analysis.get("float_constants", [])
    check_for_missing_literal_constants(float_constants)
    if analysis.get("float_dynamic_addressing"):
        float_constants = list(range(256))
    constant_table_container = build_real_constant_table(
        float_constants, sampler_regs, analysis.get("bool_constants", []),
        is_pixel_shader=True)

    header_size = 36
    constant_table_offset = header_size
    shader_offset = constant_table_offset + len(constant_table_container)
    virtual_size = shader_offset + len(pixel_shader_header)
    padding = (-virtual_size) % 4
    virtual_size += padding
    physical_size = len(ucode_bytes)

    # REAL BUG FOUND AND FIXED: XenosRecomp determines vertex-vs-pixel purely
    # from `flags & 0x1` (bit 0 CLEAR means pixel shader). Real confirmed
    # flags value for a pixel shader: 0x102A1100.
    shader_container = struct.pack(
        ">IIIIIIIII", 0x102A1100, virtual_size, physical_size, 0,
        constant_table_offset, 0, shader_offset, 0, 0,
    )

    return (shader_container + constant_table_container + pixel_shader_header +
            b"\x00" * padding + ucode_bytes)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ucode_dir", type=Path)
    parser.add_argument("shader_kind", choices=["vs", "ps"])
    parser.add_argument("hash", help="hex, no 0x prefix")
    parser.add_argument("output", type=Path)
    parser.add_argument("--vertdecl-dir", type=Path, default=None)
    parser.add_argument("--ucode-analyze", default=None)
    parser.add_argument("--reject-heuristic", action="store_true",
                        help="Exit with status 2 instead of emitting a container if any vertex "
                             "attribute needed the fallback usage heuristic (no real captured "
                             "D3D9 vertex declaration) -- for callers that only want "
                             "verified-correct containers, not best-effort guesses.")
    parser.add_argument("--pair-with-ps", type=Path, default=None,
                        help="(vs only) Path to a REAL, D3D9-captured pixel shader container "
                             "(shaders/ps_<hash>.bin) that is genuinely drawn paired with this "
                             "vertex shader in real gameplay. If given and it has real "
                             "interpolator data, that data is copied verbatim as this vertex "
                             "shader's own interpolator array instead of guessing from vertex "
                             "inputs -- see read_ps_interpolator_usages()'s docstring for why "
                             "this is structurally correct (XenosRecomp wires VS/PS "
                             "interpolators together by array index, not by name).")
    args = parser.parse_args()

    target_hash = int(args.hash, 16)
    ucode_path = args.ucode_dir / f"{args.shader_kind}_{target_hash:016x}.ucode"
    if not ucode_path.exists():
        print(f"build_synthetic_container: '{ucode_path}' not found", file=sys.stderr)
        return 1

    ucode_analyze_path = find_or_build_ucode_analyze(args.ucode_analyze)
    analysis = run_ucode_analyze(ucode_analyze_path, args.shader_kind, ucode_path)

    ucode_bytes = ucode_path.read_bytes()

    if args.shader_kind == "vs":
        vertdecl_dir = args.vertdecl_dir or (args.ucode_dir.parent / "shaders_vertdecl")
        vertdecl_path = vertdecl_dir / f"vs_{target_hash:016x}.vertdecl.txt"
        vertdecl_elements = parse_vertdecl_file(vertdecl_path) if vertdecl_path.exists() else None
        if vertdecl_elements is None:
            print(f"build_synthetic_container: NOTE no real vertex-declaration capture at "
                  f"'{vertdecl_path}' -- falling back to usage heuristic for this shader", file=sys.stderr)
        ps_interpolator_usages = None
        if args.pair_with_ps is not None:
            ps_interpolator_usages = read_ps_interpolator_usages(args.pair_with_ps)
            if not ps_interpolator_usages:
                print(f"build_synthetic_container: NOTE --pair-with-ps '{args.pair_with_ps}' has "
                      "no real interpolator data (not a real D3D9-captured container, or "
                      "genuinely writes zero interpolators) -- falling back to guessing from "
                      "vertex inputs", file=sys.stderr)
        try:
            container = build_vertex_container(ucode_bytes, analysis, vertdecl_elements,
                                               reject_heuristic=args.reject_heuristic,
                                               ps_interpolator_usages=ps_interpolator_usages)
        except HeuristicRejected:
            print(f"build_synthetic_container: REJECTED {ucode_path.stem} -- no real vertex "
                  "declaration and --reject-heuristic was given", file=sys.stderr)
            return 2
        except MissingLiteralConstantData as e:
            print(f"build_synthetic_container: REJECTED {ucode_path.stem} -- {e}", file=sys.stderr)
            return 2
    else:
        try:
            container = build_pixel_container(ucode_bytes, analysis)
        except MissingLiteralConstantData as e:
            print(f"build_synthetic_container: REJECTED {ucode_path.stem} -- {e}", file=sys.stderr)
            return 2

    args.output.write_bytes(container)
    print(f"build_synthetic_container: wrote {len(container)} bytes to {args.output} "
          f"({len(ucode_bytes)} bytes real microcode)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
