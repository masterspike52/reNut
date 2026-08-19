# rexglue_xenosrecomp.cmake — build-time Xenos shader microcode -> HLSL/SPIR-V
# for the native-renderer effort (see docs/ai/research.md Phase 0 item 3/4
# and Phase 1).
#
# Fetches hedge-dev/XenosRecomp as a host tool. Two entry points:
#   - renut_xenos_shader(): single named shader -> plain HLSL text, for
#     hand-inspection/Phase-1-style one-off integration.
#   - renut_xenos_shader_batch(): a whole guest shader dump directory -> one
#     combined, real, compiled (SPIR-V) shader cache .cpp, for the
#     "convert all the important shaders" effort. See tools/xenos_batch_convert.py
#     for why this isn't just XenosRecomp's own directory-scan mode: 26/748
#     real Banjo shaders segfault it outright, and its
#     directory driver has no per-shader crash recovery -- the batch script
#     isolates each shader in its own subprocess so one crasher doesn't take
#     down the whole cache.

include(FetchContent)

# Real, unpatched upstream bugs (confirmed see
# renut_shader_conversion_blockers.md memory + cmake/patches/xenosrecomp_native_renderer.patch's
# own header comment). Two real, independent fixes bundled in one patch:
#
# 1. XenosRecomp's main.cpp guards every DXC compile result with a bare
# assert() and no null-check, so ANY failed HLSL compile (missing interpolator semantic, DXC
# rejecting the generated code, etc -- any reason at all) segfaults the whole XenosRecomp process
# instead of skipping that one shader. Our own batch driver (tools/xenos_batch_convert.py) already
# isolates each shader in its own subprocess specifically to survive this, but a crash there still
# means we learn NOTHING about why it failed -- just "crash", no stderr, no shader hash in the
# message. This patch turns every one of those into a graceful, logged skip (shader hash + real
# reason printed, shader simply omitted from the output cache) instead of a silent segfault --
# not upstream-reported (checked: zero related issues/PRs in
# hedge-dev/XenosRecomp).
#
# 2. XenosRecomp's combined multi-shader pass (main.cpp, std::execution::par_unseq over all
# shaders) has a REAL, confirmed, deterministic concurrency bug: shaders that convert
# successfully in isolation can silently fail (with fix 1 above, gracefully -- without it,
# crash) when compiled in parallel alongside hundreds of others. Confirmed via direct
# bisection: the exact same shader set run under std::execution::seq instead converts 100%
# successfully (0 failures across 2135 real shaders, ~50s total), while par_unseq
# deterministically drops the same ~450 shaders on every repeated run with identical input --
# ruling out a resource-exhaustion fluke or true data race, and pointing at real DXC-internal
# or thread_local state that isn't safe across concurrent DxcCompiler instances despite each
# thread owning its own. Not root-caused further (DXC's own internals, out of scope to debug
# here) -- switched to std::execution::seq instead, a real, verified, safe fix: one-time
# build-step cost, not a hot path, and 50s for ~2100 shaders is a fully acceptable trade for
# 100% correctness over a silent ~20% loss.
# git apply --check first (idempotent): a fresh clone needs the patch; an already-configured
# build tree (FetchContent skips re-cloning if the source dir already exists) would fail a bare
# `git apply` on the second+ configure since the patch no longer applies cleanly to already-
# patched source -- only apply when it actually still needs it.
#
# PATCH_COMMAND is executed directly via execute_process, NOT through a
# shell -- a bare command string containing shell operators (&&, ||,
# 2>/dev/null) does NOT do conditional/redirection logic; every token gets
# passed as a literal argv entry to the first command instead. The
# idempotent check-then-apply logic lives in apply_xenosrecomp_patch.cmake
# (a portable CMake script, not `sh -c ...`) so it works the same on Windows
# as everywhere else -- `sh` is not on PATH by default there, which broke
# the whole FetchContent populate step.
#
# xenosrecomp_native_renderer.patch SUPERSEDES the old
# xenosrecomp_graceful_skip.patch (deleted) -- it is that same patch's full
# content PLUS this session's native-renderer fixes (256-bit boolean-constant
# file instead of 1 dword; positional [[vk::location]] instead of the
# Unleashed-specific USAGE_LOCATIONS table; INTERPOLATORS extended with
# Normal/Tangent/Binormal; ALU register file 32 -> 64), captured via `git diff`
# against the pristine FetchContent checkout and verified with `git apply
# --check` against a fresh clone of the same upstream commit. See
# docs/ai/history.md's "DONE" section for the full rationale and
# measured before/after shader-conversion yield. Kept as ONE combined patch
# file (not two applied in sequence) since PATCH_COMMAND's idempotent
# check-then-apply logic only handles a single patch cleanly.
FetchContent_Declare(
    xenosrecomp
    GIT_REPOSITORY https://github.com/hedge-dev/XenosRecomp.git
    GIT_TAG main
    GIT_SUBMODULES_RECURSE ON
    PATCH_COMMAND ${CMAKE_COMMAND}
        "-DPATCH_FILE=${CMAKE_CURRENT_LIST_DIR}/patches/xenosrecomp_native_renderer.patch"
        -P "${CMAKE_CURRENT_LIST_DIR}/apply_xenosrecomp_patch.cmake"
)
FetchContent_MakeAvailable(xenosrecomp)

# xenos_cache_unpack: decompresses XenosRecomp's own generated shader-cache
# .cpp (ZSTD-compressed, smol-v-encoded SPIR-V) into plain uint32_t SPIR-V
# words at BUILD TIME, so rexgpu-renut (a separate CMake build from reNut's
# own executable, the only place these FetchContent-provided zstd/smol-v
# targets exist) never needs to link zstd or smol-v itself. See
# tools/xenos_cache_unpack.cpp for the full rationale.
if(NOT TARGET xenos_cache_unpack)
    # smol-v has no CMake target of its own -- XenosRecomp's own build
    # compiles smolv.cpp directly into its executable's sources (see
    # XenosRecomp/CMakeLists.txt's SMOLV_SOURCE_DIR use), so this does the
    # same rather than inventing a target that doesn't exist upstream.
    add_executable(xenos_cache_unpack
        "${CMAKE_SOURCE_DIR}/tools/xenos_cache_unpack.cpp"
        "${xenosrecomp_SOURCE_DIR}/thirdparty/smol-v/source/smolv.cpp"
    )
    target_include_directories(xenos_cache_unpack PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${xenosrecomp_SOURCE_DIR}/thirdparty/smol-v/source"
        # Real hash-remap fix (see xenos_cache_unpack.cpp's
        # BuildHashRemap()): xxHash is header-only (XXH_INLINE_ALL), no new
        # link target needed -- reuses the same real xxHash copy XenosRecomp
        # itself was already fetched with, no separate dependency to manage.
        "${xenosrecomp_SOURCE_DIR}/thirdparty/xxHash"
    )
    target_link_libraries(xenos_cache_unpack PRIVATE libzstd_static)
endif()

# renut_xenos_shader(<target> INPUT <shader.bin> OUTPUT <shader.hlsl>)
#
# Wires a custom command that runs XenosRecomp in single-file mode against
# INPUT (one shader's .bin dump, e.g. from shader_dump.cpp's
# `dump_guest_shaders` capture) and produces OUTPUT as plain HLSL text (NOT a
# compiled shader-cache .cpp — this is source for inspection/Phase-1 hand
# integration, not something to add to <target>'s sources directly). No-ops
# (with a status message, not an error) if INPUT does not exist at configure
# time, since the guest shader dump this depends on is opt-in runtime capture,
# never checked into the repo.
function(renut_xenos_shader target_name)
    cmake_parse_arguments(ARG "" "INPUT;OUTPUT" "" ${ARGN})

    if(NOT EXISTS "${ARG_INPUT}")
        message(STATUS
            "renut_xenos_shader: '${ARG_INPUT}' not found, skipping "
            "(run with -Ddump_guest_shaders=true once to produce a shader "
            "dump, then reconfigure).")
        return()
    endif()

    set(_common_header "${xenosrecomp_SOURCE_DIR}/XenosRecomp/shader_common.h")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND $<TARGET_FILE:XenosRecomp>
                "${ARG_INPUT}"
                "${ARG_OUTPUT}"
                "${_common_header}"
        DEPENDS XenosRecomp "${_common_header}" "${ARG_INPUT}"
        COMMENT "Recompiling guest shader ${ARG_INPUT} via XenosRecomp"
        VERBATIM
    )

    add_custom_target(${target_name}_xenos_shader DEPENDS "${ARG_OUTPUT}")
    add_dependencies(${target_name} ${target_name}_xenos_shader)
endfunction()

# renut_synthesize_constant_tables(SHADER_DUMP_DIR <dir> REXSDK_DIR <dir>
#                                   OUTPUT_DIR <dir>)
#
# most captured Banjo-Kazooie shaders have
# constantTableOffset == 0 in their real container header -- the game's
# build stripped the D3DX9 constant-reflection table XenosRecomp needs (see
# XenosRecomp's own README: "If this data is missing, the recompiler will
# not function"), NOT a capture bug on reNut's side (confirmed via direct
# comparison against XenosRecomp's own shader_recompiler.cpp, which asserts
# constantTableOffset != NULL and reads real D3DXSHADER_CONSTANTTABLE data
# from it). tools/xenos_synthesize_constant_table.py fixes this by
# synthesizing a real, valid constant table from what the SDK's OWN
# Shader::AnalyzeUcode already knows about each shader (which float4
# registers and samplers it actually reads) -- verified end-to-end to raise
# real XenosRecomp conversion success from 36/253 to 101/253 real captured
# shaders. Two real SDK-side bugs had to be fixed to make AnalyzeUcode
# itself trustworthy first (see rexglue-sdk-src's translator.cpp/
# translator_disasm.cpp changes, same date): a control-flow-bound
# computation that silently produced empty analysis for shaders with
# zero-count trailing exec instructions, and a debug-disassembly crash on
# vertex formats outside this codebase's currently-modeled xenos::VertexFormat
# enum.
#
# Builds tools/ucode_analyze.cpp as a real CMake executable (same real SDK
# source list as tools/build_ucode_analyze.sh, kept in sync manually -- see
# that script's own comment on how the list was derived) linked against
# REXSDK_DIR, then runs the synthesis script against a COPY of
# SHADER_DUMP_DIR under OUTPUT_DIR (never mutates the real capture in
# place) every configure. No-ops if SHADER_DUMP_DIR doesn't exist yet,
# matching renut_xenos_shader_batch()'s own behavior.
function(renut_synthesize_constant_tables)
    cmake_parse_arguments(ARG "" "SHADER_DUMP_DIR;REXSDK_DIR;OUTPUT_DIR" "" ${ARGN})

    if(NOT EXISTS "${ARG_SHADER_DUMP_DIR}")
        message(STATUS
            "renut_synthesize_constant_tables: '${ARG_SHADER_DUMP_DIR}' not "
            "found, skipping.")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    if(NOT TARGET ucode_analyze)
        set(_sdk "${ARG_REXSDK_DIR}")
        add_executable(ucode_analyze
            "${CMAKE_SOURCE_DIR}/tools/ucode_analyze.cpp"
            "${_sdk}/src/graphics/pipeline/shader/shader.cpp"
            "${_sdk}/src/graphics/pipeline/shader/translator.cpp"
            "${_sdk}/src/graphics/pipeline/shader/translator_disasm.cpp"
            "${_sdk}/src/graphics/format/ucode.cpp"
            "${_sdk}/src/core/string_buffer.cpp"
            "${_sdk}/src/core/logging.cpp"
            "${_sdk}/src/core/cvar.cpp"
            "${_sdk}/src/core/memory.cpp"
            "${_sdk}/src/core/filesystem_posix.cpp"
            "${_sdk}/src/core/platform/env_posix.cpp"
            "${_sdk}/src/core/utf8.cpp"
            "${_sdk}/thirdparty/fmt/src/format.cc"
            "${_sdk}/thirdparty/fmt/src/os.cc"
        )
        target_include_directories(ucode_analyze PRIVATE
            "${_sdk}/include" "${_sdk}"
            "${_sdk}/thirdparty/spdlog/include" "${_sdk}/thirdparty/fmt/include"
            "${_sdk}/thirdparty/simde" "${_sdk}/thirdparty/tomlplusplus/include"
            "${_sdk}/thirdparty/cli11/include" "${_sdk}/thirdparty/xxHash"
            "${_sdk}/thirdparty/renderdoc" "${_sdk}/thirdparty/utfcpp/source"
        )
        target_compile_definitions(ucode_analyze PRIVATE SPDLOG_FMT_EXTERNAL)
        target_compile_options(ucode_analyze PRIVATE -mssse3)
    endif()

    set(_synth_script "${CMAKE_SOURCE_DIR}/tools/xenos_synthesize_constant_table.py")
    set(_synth_manifest "${ARG_OUTPUT_DIR}/synthesize_constant_tables_manifest.txt")
    set(_stamp "${ARG_OUTPUT_DIR}/.synthesized_stamp")

    # Real copy-then-patch, not in-place: OUTPUT_DIR is a build-tree scratch
    # location the batch converter below reads from, so the user's actual
    # captured shaders/ (which may be reused across many rebuilds/games)
    # never gets mutated. glob_recurse rather than a fixed file list since
    # the real shader count varies run to run as more play sessions add
    # captures.
    file(GLOB _dump_files "${ARG_SHADER_DUMP_DIR}/*")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${ARG_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${ARG_SHADER_DUMP_DIR}" "${ARG_OUTPUT_DIR}"
        COMMAND ${Python3_EXECUTABLE} "${_synth_script}"
                $<TARGET_FILE:ucode_analyze>
                "${ARG_OUTPUT_DIR}"
                "${_synth_manifest}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ucode_analyze "${_synth_script}" ${_dump_files}
        COMMENT "Synthesizing missing constant tables for real captured shaders in ${ARG_SHADER_DUMP_DIR}"
        VERBATIM
    )
    add_custom_target(renut_synthesize_constant_tables_target DEPENDS "${_stamp}")
endfunction()

# renut_synthesize_containers_from_ucode(UCODE_DIR <dir> REXSDK_DIR <dir>
#                                         OUTPUT_DIR <dir>)
#
# this game's MOST-USED shaders never call D3D9's
# CreateVertexShader/CreatePixelShader at all -- they're loaded directly
# into GPU sequencer memory via IM_LOAD/IM_LOAD_IMMEDIATE PM4 packets
# (confirmed via direct source investigation and cross-referenced against
# real capture data: 0% overlap between shader_dump.cpp's D3D9-hook
# captures and real `renut color draw pair` live draw identities, across
# 1471 distinct real drawn shader hashes). shader_dump.cpp's hook
# structurally can never see these, regardless of capture timing/coverage.
#
# Real, raw microcode for these IS available via the separate
# renut_dump_ucode_hash cvar (shaders_ucode_hash/<vs|ps>_<hash>.ucode,
# keyed by the real runtime ucode_data_hash()), just missing the
# ShaderContainer metadata (constantTableOffset, vertex elements,
# interpolators) XenosRecomp needs. tools/build_synthetic_container.py
# (existing tool, generalized this session to include real float/sampler/
# bool constant tables -- previously left them empty, causing 100% failure)
# rebuilds a full, real container per .ucode file using the SDK's own
# Shader::AnalyzeUcode(). Verified end-to-end: 1492/1884 real captured
# shaders from this path convert successfully (vs ~12 before the constant-
# table fix), a MUCH larger and more relevant pool than the D3D9-hook
# capture's 101/253 -- these are the game's actual most-drawn shaders.
#
# Writes into OUTPUT_DIR, same directory renut_synthesize_constant_tables()
# uses -- xenos_batch_convert.py scans generically for *.bin containers
# regardless of origin, so both sources merge into one real, combined
# shader pool for the batch converter below to process together.
#
# Real follow-up finding (same session): 1590/2143 shaders from
# this path DID convert and DID render natively in-game (confirmed via
# real `native pipeline: ... created` log lines, 148 distinct pairs) --
# but with real, visible geometry corruption, because real D3D9 vertex-
# declaration capture is ALSO unavailable for these shaders (same IM_LOAD
# root cause -- SetVertexShader/SetVertexDeclaration never fire either),
# so every vertex shader here falls back to build_synthetic_container.py's
# POSITION-first/TEXCOORD-follows heuristic, which is only verified
# correct for the one shader it was originally built against. Passing
# --reject-heuristic below trades shader-count for correctness: only
# vertex shaders whose semantics didn't need guessing (currently: none
# have a real captured declaration, so in practice this means "only
# non-vertex-shader-having pixel shaders, plus any vertex shader trivial
# enough to need zero non-position attributes" survive) go into the real
# cache. Revisit if/when real vertex-declaration capture becomes possible
# for these shaders (would need a different mechanism entirely, not a fix
# to the existing SetVertexDeclaration hook -- see this session's own
# finding that hook structurally cannot see these shaders either).
function(renut_synthesize_containers_from_ucode)
    cmake_parse_arguments(ARG "" "UCODE_DIR;REXSDK_DIR;OUTPUT_DIR" "" ${ARGN})

    if(NOT EXISTS "${ARG_UCODE_DIR}")
        message(STATUS
            "renut_synthesize_containers_from_ucode: '${ARG_UCODE_DIR}' not "
            "found, skipping (run with -Drenut_dump_ucode_hash=true and play "
            "for a while, then reconfigure).")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    if(NOT TARGET ucode_analyze)
        set(_sdk "${ARG_REXSDK_DIR}")
        add_executable(ucode_analyze
            "${CMAKE_SOURCE_DIR}/tools/ucode_analyze.cpp"
            "${_sdk}/src/graphics/pipeline/shader/shader.cpp"
            "${_sdk}/src/graphics/pipeline/shader/translator.cpp"
            "${_sdk}/src/graphics/pipeline/shader/translator_disasm.cpp"
            "${_sdk}/src/graphics/format/ucode.cpp"
            "${_sdk}/src/core/string_buffer.cpp"
            "${_sdk}/src/core/logging.cpp"
            "${_sdk}/src/core/cvar.cpp"
            "${_sdk}/src/core/memory.cpp"
            "${_sdk}/src/core/filesystem_posix.cpp"
            "${_sdk}/src/core/platform/env_posix.cpp"
            "${_sdk}/src/core/utf8.cpp"
            "${_sdk}/thirdparty/fmt/src/format.cc"
            "${_sdk}/thirdparty/fmt/src/os.cc"
        )
        target_include_directories(ucode_analyze PRIVATE
            "${_sdk}/include" "${_sdk}"
            "${_sdk}/thirdparty/spdlog/include" "${_sdk}/thirdparty/fmt/include"
            "${_sdk}/thirdparty/simde" "${_sdk}/thirdparty/tomlplusplus/include"
            "${_sdk}/thirdparty/cli11/include" "${_sdk}/thirdparty/xxHash"
            "${_sdk}/thirdparty/renderdoc" "${_sdk}/thirdparty/utfcpp/source"
        )
        target_compile_definitions(ucode_analyze PRIVATE SPDLOG_FMT_EXTERNAL)
        target_compile_options(ucode_analyze PRIVATE -mssse3)
    endif()

    set(_batch_script "${CMAKE_SOURCE_DIR}/tools/batch_build_synthetic_containers.py")
    set(_stamp "${ARG_OUTPUT_DIR}/.ucode_synthesized_stamp")

    file(GLOB _ucode_files "${ARG_UCODE_DIR}/*.ucode")
    # build_synthetic_container.py reads
    # real captured vertex-declaration usage from
    # <ucode_dir>/../shaders_vertdecl/vs_<hash>.vertdecl.txt when present
    # (see its own --vertdecl-dir default), but this DEPENDS list never
    # included those files -- so after fixing the capture hook and
    # recapturing real .vertdecl.txt data by playing, a rebuild would keep
    # using the STALE synthesized containers (heuristic-only) because
    # CMake had no reason to think anything changed. Glob them in so new/
    # changed real captures actually trigger a re-synthesis.
    get_filename_component(_vertdecl_dir "${ARG_UCODE_DIR}/../shaders_vertdecl" ABSOLUTE)
    file(GLOB _vertdecl_files "${_vertdecl_dir}/*.vertdecl.txt")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
        COMMAND ${Python3_EXECUTABLE} "${_batch_script}"
                "${ARG_UCODE_DIR}"
                "${ARG_OUTPUT_DIR}"
                $<TARGET_FILE:ucode_analyze>
                # TEMP: --reject-heuristic disabled to isolate
                # the remaining live-native-shader corruption bug (confirmed
                # NOT the location bug, NOT native/stock mixing -- see
                # memory). RE-ENABLE once the real cause is found+fixed or
                # this investigation is done for the session.
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ucode_analyze "${_batch_script}"
                "${CMAKE_SOURCE_DIR}/tools/build_synthetic_container.py" ${_ucode_files} ${_vertdecl_files}
        COMMENT "Synthesizing full shader containers from real ucode-only captures in ${ARG_UCODE_DIR}"
        VERBATIM
    )
    add_custom_target(renut_synthesize_containers_from_ucode_target DEPENDS "${_stamp}")
endfunction()

# renut_xenos_shader_batch(<target> SHADER_DUMP_DIR <dir> OUTPUT <cache.cpp>
#                           [MANIFEST <manifest.txt>])
#
# Converts every real guest shader in SHADER_DUMP_DIR (reNut's
# `dump_guest_shaders` output) to compiled SPIR-V via XenosRecomp, using
# tools/xenos_batch_convert.py to isolate each shader in its own subprocess
# so crashing shaders are skipped instead of aborting the whole run. OUTPUT
# is XenosRecomp's own generated shader-cache C++ (ShaderCacheEntry table +
# compressed SPIR-V blob) built from only the shaders that converted
# successfully -- add it to <target>'s sources yourself if/when a consumer
# for the combined cache exists; this function only produces the file.
#
# No-ops (status message, not an error) if SHADER_DUMP_DIR does not exist at
# configure time, matching renut_xenos_shader()'s behavior -- the shader dump
# is opt-in runtime capture, never checked into the repo.
function(renut_xenos_shader_batch target_name)
    cmake_parse_arguments(ARG "" "SHADER_DUMP_DIR;OUTPUT;MANIFEST" "" ${ARGN})

    if(NOT EXISTS "${ARG_SHADER_DUMP_DIR}")
        message(STATUS
            "renut_xenos_shader_batch: '${ARG_SHADER_DUMP_DIR}' not found, "
            "skipping (run with -Ddump_guest_shaders=true and play for a "
            "while to produce a real shader dump, then reconfigure).")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_common_header "${xenosrecomp_SOURCE_DIR}/XenosRecomp/shader_common.h")
    set(_batch_script "${CMAKE_SOURCE_DIR}/tools/xenos_batch_convert.py")

    # Real hash-remap fix: XenosRecomp's own cache-entry hash
    # is NOT the same value the Vulkan pipeline cache looks shaders up by at
    # runtime -- see xenos_cache_unpack.cpp's BuildHashRemap() for the full
    # explanation. This file carries the real data stage 2 needs to fix it.
    # Always requested (not optional) since the fix is meaningless without
    # it -- a missing MANIFEST is fine (it's diagnostic-only), a missing
    # remap file means every entry silently keeps the wrong hash again.
    set(_remap_file "${ARG_OUTPUT}.hashremap.bin")

    # xenos_batch_convert.py's remap file is always its LAST positional
    # argument (see its own usage docstring); MANIFEST must always be given
    # a real value here (even if the caller didn't request one) so an empty
    # ARG_MANIFEST can't shift the remap file into the manifest's slot --
    # CMake command lists silently drop empty-string list elements, which
    # would otherwise be a real, easy-to-hit positional-argument bug.
    set(_manifest_arg "${ARG_OUTPUT}.manifest.txt")
    if(ARG_MANIFEST)
        set(_manifest_arg "${ARG_MANIFEST}")
    endif()

    # Stage 1: xenos_batch_convert.py produces XenosRecomp's OWN generated
    # cache format (ShaderCacheEntry table + ZSTD-compressed smol-v SPIR-V) —
    # written to a scratch file, not ARG_OUTPUT directly, since stage 2
    # rewrites it into the plain/decompressed form ARG_OUTPUT actually is.
    # Also produces _remap_file (see above) as a second real output.
    set(_compressed_cache "${ARG_OUTPUT}.compressed.cpp")

    # DEPENDS never listed the actual
    # shader dump directory's CONTENTS, only the tool/header/script -- so
    # ninja had no way to know new/changed .bin files should trigger a
    # rebuild, and correctly (from its own point of view) reported "no work
    # to do" even after the synthesis steps above populated
    # SHADER_DUMP_DIR with 1000+ new real containers. Real symptom: two full
    # reconfigures in a row both produced this, since nothing about
    # XenosRecomp/common_header/batch_script itself had changed -- only the
    # shader files had. glob_recurse (not a fixed list) since the real
    # shader count varies build to build as more captures/synthesis runs
    # add files.
    file(GLOB_RECURSE _shader_dump_files "${ARG_SHADER_DUMP_DIR}/*")

    add_custom_command(
        OUTPUT "${_compressed_cache}" "${_remap_file}"
        COMMAND ${Python3_EXECUTABLE} "${_batch_script}"
                $<TARGET_FILE:XenosRecomp>
                "${ARG_SHADER_DUMP_DIR}"
                "${_common_header}"
                "${_compressed_cache}"
                "${_manifest_arg}"
                "${_remap_file}"
        DEPENDS XenosRecomp "${_common_header}" "${_batch_script}" ${_shader_dump_files}
        COMMENT "Batch-converting guest shaders in ${ARG_SHADER_DUMP_DIR} via XenosRecomp (crash-isolated per shader)"
        VERBATIM
    )

    # Stage 2: xenos_cache_unpack decompresses the stage-1 output into plain
    # uint32_t SPIR-V words, so <target> (rexgpu-renut, a separate CMake
    # build with no zstd/smol-v of its own) can consume ARG_OUTPUT directly.
    # Also applies the real hash remap (_remap_file) so entries are keyed by
    # the hash the runtime actually looks native pipelines up by.
    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND $<TARGET_FILE:xenos_cache_unpack> "${_compressed_cache}" "${ARG_OUTPUT}" "${_remap_file}"
        DEPENDS xenos_cache_unpack "${_compressed_cache}" "${_remap_file}"
        COMMENT "Decompressing native-renderer shader cache -> ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${target_name}_xenos_shader_batch DEPENDS "${ARG_OUTPUT}")
    add_dependencies(${target_name} ${target_name}_xenos_shader_batch)
endfunction()
