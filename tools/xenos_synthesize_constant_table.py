#!/usr/bin/env python3
"""Patches real captured Xbox 360 shader containers (shader_dump.cpp output,
shaders/<vs|ps>_<hash>.bin) that have constantTableOffset == 0 -- meaning the
game's build stripped the D3DX9 constant-reflection table XenosRecomp needs
(see XenosRecomp's own README: "Constant buffer registers are populated
using reflection data embedded in the shader binaries. If this data is
missing, the recompiler will not function.") -- by synthesizing a real,
valid D3DXSHADER_CONSTANTTABLE-format blob from what our OWN ucode analysis
(tools/ucode_analyze.cpp, using the same Shader::AnalyzeUcode the runtime
pipeline cache already relies on) already knows about the shader: which
float4 constant registers (c0-c255) it reads, and which sampler registers
(s0-s31, from real texture_bindings()) it uses. XenosRecomp doesn't care
about the constant NAMES (only registerSet/registerIndex/registerCount
matter for the #defines it emits), so synthetic names ("c0", "s0", ...) are
fine -- it never needs to match the game's real original names.

Real D3DXSHADER_CONSTANTTABLE layout confirmed by reading XenosRecomp's own
XenosRecomp/constant_table.h and XenosRecomp/shader_recompiler.cpp
(recompile()) directly, not guessed:
    ConstantTableContainer { uint32 size; ConstantTable constantTable; }
    ConstantTable { uint32 size, creator, version, constants, constantInfo,
                     flags, target; }
    ConstantInfo { uint32 name; uint16 registerSet; uint16 registerIndex;
                    uint16 registerCount; uint16 reserved; uint32 typeInfo;
                    uint32 defaultValue; }
All big-endian (be<T> in XenosRecomp's own headers, matching the real Xbox
360 shader bytecode's own endianness). `constantTableOffset` (in
ShaderContainer, at container-relative byte 16) points at the
ConstantTableContainer; `constantTable.constantInfo`/`constantInfo->name`
are offsets relative to the ConstantTable struct's OWN start (i.e.
constantTableOffset + 4), not the container or file start -- confirmed via
direct reading of shader_recompiler.cpp's recompile() pointer arithmetic.

`typeInfo`/`defaultValue` are read by XenosRecomp for Float4/Sampler
registerSets? -- confirmed NO (see recompile()'s Float4/Sampler cases: only
name/registerIndex/registerCount are dereferenced) so they're left zeroed;
real TypeInfo synthesis is unnecessary for this recompiler's actual needs.

Usage:
    xenos_synthesize_constant_table.py <ucode_analyze_binary> <shader_dir>
        [<output_manifest>]

Scans <shader_dir> for every *.bin whose real ShaderContainer header has
constantTableOffset == 0, runs <ucode_analyze_binary> against the matching
*.ucode file, and if the shader references NO float constants and NO
samplers, leaves it alone (the missing table doesn't matter, matches this
project's "16 ok shaders with ctbl_off==0" real finding). Otherwise,
synthesizes a real constant table, appends it to the container's PHYSICAL
section (extending physicalSize accordingly, matching how real XenosRecomp
containers grow), and overwrites constantTableOffset in-place. Rewrites the
.bin in place; a .orig backup is kept alongside it the first time only.
"""

import json
import re
import struct
import subprocess
import sys
from pathlib import Path

_CONTAINER_STRUCT = struct.Struct(">IIIIIIIII")  # flags,vsize,psize,fieldC,ctbl,dtbl,shader,f1c,f20
_MAGIC_MASK = 0xFFFFFF00
_MAGIC_VALUE = 0x102A1100

_REGSET_BOOL = 0
_REGSET_FLOAT4 = 2
_REGSET_SAMPLER = 3


def build_constant_table(float_regs, sampler_regs, bool_regs=(), is_pixel_shader=False,
                          definition_table_regs=frozenset()):
    """Returns real, big-endian D3DXSHADER_CONSTANTTABLE bytes (the whole
    ConstantTableContainer, ready to append to a container's physical
    section and point constantTableOffset at).

    float4 registers used to get one ConstantInfo
    EACH (registerCount=1), which makes XenosRecomp emit a plain #define per
    register -- shader_recompiler.cpp's recompile() can only express real
    Xenos dynamic/relative constant addressing (`c[a0+N]`/`c[aL+N]`, used
    pervasively for skeletal-animation/skinning bone-matrix lookups) via the
    safe, bounds-clamped INDEXED macro it emits for a registerCount > 1
    entry (shader_recompiler.cpp:1205-1219) -- with registerCount always 1,
    any shader doing dynamic addressing silently fell through to a
    plain-name fallback that DROPS the +a0/+aL offset entirely (guarded
    only by an assert() that compiles to nothing in a release build) and
    always reads the same static register regardless of the real runtime
    index. Confirmed real, structural bug -- a strong match for reported
    shattered/scattered-looking animated geometry. Fixed by emitting ONE
    grouped float4 entry spanning contiguous runs of registers, instead of
    one per individually-referenced register, so XenosRecomp takes its own
    safe indexed-macro path.

    definition_table_regs must still be excluded (a real, already-fixed
    crash: XenosRecomp emits a LOCAL VARIABLE `float4 c<N> = ...` for every
    register a container's real DefinitionTable covers -- including one of
    those registers in this constant table too would double-define it and
    fail DXC's compile). A single contiguous registerIndex/registerCount
    span can't "skip" specific registers, so this walks float_regs and
    emits one grouped entry per maximal contiguous run of registers that
    are present and NOT in definition_table_regs -- still safe/indexed for
    every real run, just split at any excluded register.
    """
    # shader_recompiler.cpp's own
    # `tailCount = (isPixelShader ? 224 : 256) - registerIndex` is baked
    # into the grouped macro it emits (`select((INDEX) < tailCount, ...,
    # 0.0)`) REGARDLESS of what registerCount this script declares -- so a
    # grouped entry covering a pixel-shader register >= 224 wouldn't fail to
    # compile, it would silently ALWAYS return 0.0 for that register (even
    # for a plain static reference, not just a dynamic one), since the
    # clamp is unconditional. 224 also matches this hardware's real,
    # documented pixel-shader float-constant register-file size (vertex
    # shaders get 256) -- so registers at/above it are a real edge case, not
    # something to force into the grouped/safe scheme. Route those through
    # the ORIGINAL, proven-correct per-register #define scheme instead
    # (registerCount=1, no dynamic-addressing support, but also no incorrect
    # zero-clamp) -- confirmed real regression this fixes: a heuristic-
    # captured pixel shader statically referencing register 255 previously
    # got excluded from BOTH schemes entirely (capped out of the grouped
    # range with no per-register fallback), causing XenosRecomp to fall
    # through to its "not found in float4Constants" path
    # (shader_recompiler.cpp:483-487) and emit a bare, never-#define'd
    # `c255` identifier -- a real DXC "undeclared identifier" compile
    # failure that didn't exist before this constant-table rework.
    max_float4_registers = 224 if is_pixel_shader else 256
    entries = []
    grouped = sorted(r for r in float_regs
                      if r not in definition_table_regs and r < max_float4_registers)
    for r in sorted(float_regs):
        if r not in definition_table_regs and r >= max_float4_registers:
            entries.append((f"c{r}", _REGSET_FLOAT4, r, 1))
    run_start = None
    prev = None
    for reg in grouped + [None]:  # sentinel to flush the final run
        if reg is not None and prev is not None and reg == prev + 1:
            prev = reg
            continue
        if run_start is not None:
            # Each run needs its OWN macro name -- XenosRecomp
            # emits one `#define {name}(INDEX) ...` per ConstantInfo entry
            # processed, so two entries sharing a name would silently
            # redefine/collide instead of each covering its own real
            # register range.
            entries.append((f"c{run_start}", _REGSET_FLOAT4, run_start, prev - run_start + 1))
        run_start = reg
        prev = reg
    for reg in sampler_regs:
        entries.append((f"s{reg}", _REGSET_SAMPLER, reg, 1))
    for reg in bool_regs:
        entries.append((f"b{reg}", _REGSET_BOOL, reg, 1))

    # Layout (relative to constantTable's own start, i.e. right after the
    # ConstantTableContainer.size field):
    #   ConstantTable header (28 bytes: size,creator,version,constants,
    #     constantInfo,flags,target)
    #   ConstantInfo[len(entries)] (20 bytes each)
    #   name strings, back to back, NUL-terminated
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
    # ConstantTableContainer.size (whole container size incl. this field)
    out += struct.pack(">I", 4 + constant_table_size)
    # ConstantTable: size, creator, version, constants, constantInfo, flags, target
    out += struct.pack(">IIIIIII", constant_table_size, 0, 0, len(entries),
                        constant_info_offset, 0, 0)
    assert len(out) == 4 + CONSTANT_TABLE_HEADER_SIZE
    for i, (name, regset, regidx, regcount) in enumerate(entries):
        out += struct.pack(">IHHHHII", name_offsets[i], regset, regidx, regcount,
                            0, 0, 0)
    assert len(out) == 4 + names_offset
    out += name_bytes
    assert len(out) == 4 + constant_table_size
    return bytes(out)


def patch_container(data: bytes, table_bytes: bytes) -> bytes:
    flags, vsize, psize, fieldc, ctbl_off, dtbl_off, shader_off, f1c, f20 = \
        _CONTAINER_STRUCT.unpack_from(data, 0)
    assert ctbl_off == 0, "container already has a constant table -- refusing to overwrite"

    # Real containers place the constant table inside the PHYSICAL section
    # (appended after virtualSize bytes) -- mirror that by appending the
    # synthesized table right after the existing physical section and
    # growing physicalSize/constantTableOffset accordingly. The offset is
    # relative to the container start (shaderData in recompile()'s own
    # pointer arithmetic), so it's simply the container's current total size.
    new_ctbl_off = len(data)
    new_psize = psize + len(table_bytes)
    new_header = _CONTAINER_STRUCT.pack(flags, vsize, new_psize, fieldc,
                                         new_ctbl_off, dtbl_off, shader_off, f1c, f20)
    return new_header + data[36:] + table_bytes


_DEFINITION_TABLE_HEADER = struct.Struct(">IIIII")  # field0,field4,field8,fieldC,size


def parse_definition_table_float_registers(data: bytes, dtbl_off: int, is_pixel_shader: bool) -> set[int]:
    """Returns the set of unified float4 register indices (c0-c511, pixel
    shader registers already offset by +256 to match ucode_analyze's own
    numbering) that this container's REAL definition table already gives a
    literal, hardcoded value to.

    Real bug this exists to prevent: XenosRecomp's own
    shader_recompiler.cpp (recompile(), ~line 1447) emits a LOCAL variable
    `float4 c<N> = asfloat(uint4(...))` for every register the definition
    table covers -- a real, distinct mechanism from the constant table this
    script builds (RegisterSet::Float4 emits a `vk::RawBufferLoad<float4>`
    MACRO named `c<N>` instead). If the SAME register N appears in both,
    the generated HLSL has two conflicting `c<N>` definitions (one local
    var, one macro) and DXC's compile fails outright -- confirmed via a
    real crash: ps_09d00ca638df4271 has a real captured definitionTable
    entry for register 255 (a real game-authored default constant), and
    this script was ALSO adding register 255 to the synthesized constant
    table (register 255 is genuinely read by the shader per ucode_analyze,
    but ucode_analyze has no way to know it's ALREADY supplied by the
    definition table, not something that needs runtime supply) --
    `#define c255 vk::RawBufferLoad<...>` then collided with the literal
    `float4 c255 = ...` XenosRecomp itself emits, and XenosRecomp's own
    zero error-handling on a failed DXC compile turned that into a real,
    silent SIGSEGV (main.cpp:139 dereferences a null IDxcBlob*).

    Mirrors shader_recompiler.cpp's real parse exactly: DefinitionTable's
    `definitions` field is a be<uint32_t> stream, read as repeating
    (Float4Definition{registerIndex:u16, count:u16, physicalOffset:u32},
    i.e. 8 bytes / 2 uint32s per entry) until a 0 terminator, then a
    SEPARATE Int4Definition stream follows (irrelevant here -- those are
    integer registers, a disjoint numbering space, never collide with
    float4 c<N> macros)."""
    if dtbl_off == 0:
        return set()
    header_end = dtbl_off + _DEFINITION_TABLE_HEADER.size
    if header_end > len(data):
        return set()
    registers: set[int] = set()
    offset = header_end
    register_offset = 256 if is_pixel_shader else 0
    while offset + 8 <= len(data):
        (word0,) = struct.unpack_from(">I", data, offset)
        if word0 == 0:
            break
        register_index, count = struct.unpack_from(">HH", data, offset)
        for i in range((count + 3) // 4):
            registers.add(register_index + i - register_offset)
        offset += 8
    return registers


def run_ucode_analyze(binary: Path, kind: str, ucode_path: Path) -> dict:
    proc = subprocess.run([str(binary), kind, str(ucode_path)],
                           capture_output=True, timeout=30)
    if proc.returncode != 0:
        raise RuntimeError(f"ucode_analyze failed for {ucode_path}: {proc.stderr.decode(errors='replace')}")
    return json.loads(proc.stdout)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    analyze_bin = Path(sys.argv[1])
    shader_dir = Path(sys.argv[2])
    manifest_path = Path(sys.argv[3]) if len(sys.argv) > 3 else None

    results = []  # (tag, action)
    for bin_path in sorted(shader_dir.glob("*.bin")):
        tag = bin_path.stem
        data = bin_path.read_bytes()
        if len(data) < 36:
            results.append((tag, "too-small"))
            continue
        flags, vsize, psize, fieldc, ctbl_off, dtbl_off, shader_off, f1c, f20 = \
            _CONTAINER_STRUCT.unpack_from(data, 0)
        if (flags & _MAGIC_MASK) != _MAGIC_VALUE:
            results.append((tag, "not-a-container"))
            continue
        if ctbl_off != 0:
            results.append((tag, "already-has-table"))
            continue

        ucode_path = bin_path.with_suffix(".ucode")
        if not ucode_path.exists():
            results.append((tag, "no-ucode-file"))
            continue

        kind = "vs" if tag.startswith("vs_") else ("ps" if tag.startswith("ps_") else None)
        if kind is None:
            results.append((tag, "unknown-kind"))
            continue

        try:
            info = run_ucode_analyze(analyze_bin, kind, ucode_path)
        except Exception as e:
            results.append((tag, f"analyze-failed:{e}"))
            continue

        float_regs = info.get("float_constants", [])
        sampler_regs = sorted({tb["fetch_constant"] for tb in info.get("texture_bindings", [])})
        bool_regs = info.get("bool_constants", [])

        if info.get("float_dynamic_addressing"):
            # All 256 registers must be assumed live -- matches
            # ConstantRegisterMap's own documented semantics for this case.
            float_regs = list(range(256))

        # See parse_definition_table_float_registers's own header
        # comment: exclude any register the REAL definition
        # table already gives a literal value to -- adding it to the
        # synthesized constant table too creates a real `c<N>` macro/local-
        # variable naming collision, which fails DXC's compile and (since
        # XenosRecomp has zero error handling for a failed compile) crashes
        # XenosRecomp outright.
        definition_table_regs = parse_definition_table_float_registers(data, dtbl_off, kind == "ps")
        float_regs = [r for r in float_regs if r not in definition_table_regs]

        if not float_regs and not sampler_regs and not bool_regs:
            results.append((tag, "no-constants-needed"))
            continue

        table_bytes = build_constant_table(float_regs, sampler_regs, bool_regs, is_pixel_shader=(kind == "ps"))
        patched = patch_container(data, table_bytes)

        backup_path = bin_path.with_suffix(".bin.orig")
        if not backup_path.exists():
            backup_path.write_bytes(data)
        bin_path.write_bytes(patched)
        results.append((tag, f"patched:{len(float_regs)}floats,{len(sampler_regs)}samplers"))

    patched_count = sum(1 for _, a in results if a.startswith("patched"))
    print(f"xenos_synthesize_constant_table: {patched_count}/{len(results)} shaders patched")

    if manifest_path is not None:
        with manifest_path.open("w") as f:
            for tag, action in results:
                f.write(f"{tag} {action}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
