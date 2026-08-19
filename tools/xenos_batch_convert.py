#!/usr/bin/env python3
"""Batch-convert a directory of dumped guest shaders to compiled SPIR-V via
XenosRecomp, isolating each shader in its own subprocess so a crash (26/748
real Banjo shaders segfault XenosRecomp's own directory-scan mode) only
drops that one shader instead of aborting the whole batch.

XenosRecomp's directory-scan mode (see its main.cpp) is what actually invokes
DXC and smol-v-encodes SPIR-V -- single-file mode only emits HLSL text. But
directory mode processes every shader it finds in one process with bare
asserts and no per-shader recovery, so a bad shader takes the whole run down.
This script gets the same directory-mode output (a real multi-shader cache)
by feeding XenosRecomp ONE synthetic single-shader directory per invocation:
copy (well, symlink) exactly one dumped shader into a scratch directory, run
XenosRecomp against that directory, check the exit code, then merge every
successful shader's cache entries into one combined output.

Usage:
    xenos_batch_convert.py <xenos_recomp_binary> <shader_dump_dir>
        <shader_common_header> <output_cpp> [<output_manifest>] [<output_remap>]

<output_cpp> gets XenosRecomp's own generated shader-cache C++ (the
ShaderCacheEntry table + compressed DXIL/SPIR-V blobs), but built only from
the shaders that converted successfully. <output_manifest>, if given, gets a
plain-text list of "<hash> <status>" lines (ok/crash/error) for visibility
into what got skipped. <output_remap>, if given, gets a length-prefixed
binary stream of every successfully-converted shader's real, whole container
bytes -- consumed by xenos_cache_unpack.cpp to fix a real hash mismatch (see
the comment where remap_entries is populated below): XenosRecomp's own
cache-entry hash is NOT the same value the Vulkan pipeline cache looks
shaders up by at runtime.
"""

import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Real ShaderContainer layout (see XenosRecomp/XenosRecomp/shader.h):
#   flags, virtualSize, physicalSize, fieldC, constantTableOffset,
#   definitionTableOffset, shaderOffset, field1C, field20 -- all big-endian
#   uint32. Magic gate: (flags & 0xFFFFFF00) == 0x102A1100.
_MAGIC_MASK = 0xFFFFFF00
_MAGIC_VALUE = 0x102A1100
_CONTAINER_STRUCT = struct.Struct(">IIIIIIIII")  # 9 big-endian uint32 = 36 bytes


def find_containers(data: bytes):
    """Yield (offset, size, virtual_size, physical_size) for each real
    ShaderContainer found in data, mirroring XenosRecomp main.cpp's own
    directory-mode scan exactly so we isolate the same units it would.
    virtual_size/physical_size are exposed so callers can locate the real
    ucode bytes (the physical section, appended right after the virtual
    section) without re-parsing the header themselves."""
    i = 0
    n = len(data)
    header_size = _CONTAINER_STRUCT.size
    while i + header_size < n:
        (flags, virtual_size, physical_size, field_c, ctbl_off, dtbl_off,
         shader_off, field1c, field20) = _CONTAINER_STRUCT.unpack_from(data, i)
        data_size = virtual_size + physical_size
        if ((flags & _MAGIC_MASK) == _MAGIC_VALUE and data_size <= (n - i) and
                field1c == 0 and field20 == 0):
            yield i, data_size, virtual_size, physical_size
            i += data_size
        else:
            i += 4


# Real per-shader result cache: each real capture/recapture
# session re-touches or re-adds files across the WHOLE shader_dump_dir (a
# fresh play session writes many files with new mtimes even for shaders
# whose CONTENT is unchanged from before), and CMake's own DEPENDS on that
# directory's contents means any such touch triggers a full rebuild here --
# real, confirmed cost: ~2200 shaders x one XenosRecomp subprocess each,
# every single recapture, even when only a handful of shaders are actually
# new. XenosRecomp's own per-shader result depends ONLY on that shader's
# real container bytes (content), not on its filename/mtime/position in the
# directory -- so a real content-hash keyed cache is exactly correct, not
# an approximation: identical bytes always convert to the identical result.
# Keyed on a real cryptographic hash (not XXH3, which the compressed cache
# entries themselves already use for a DIFFERENT purpose -- see the hash-
# remap comment lower in this file; reusing it here would risk confusion
# between two unrelated hash spaces) of the shader's own container bytes.
_CACHE_VERSION = 1


def _load_result_cache(cache_path: Path) -> dict[str, dict]:
    if not cache_path.exists():
        return {}
    try:
        data = json.loads(cache_path.read_text())
    except (json.JSONDecodeError, OSError):
        return {}
    if data.get("version") != _CACHE_VERSION:
        return {}
    return data.get("entries", {})


def _save_result_cache(cache_path: Path, entries: dict[str, dict]) -> None:
    cache_path.write_text(json.dumps({"version": _CACHE_VERSION, "entries": entries}))


def _run_one_shader(xenos_recomp: Path, common_header: Path, tag: str, chunk: bytes) -> str:
    """Runs XenosRecomp against exactly one shader in its own isolated temp
    directory/subprocess -- identical isolation to what the old strictly-
    sequential loop did per shader, just pulled into a function so many of
    these can run concurrently (see the ThreadPoolExecutor in main()).
    Returns the same status strings the old inline loop produced: "timeout",
    "crash", "error", "no-output", "skipped-by-xenosrecomp:<reason>", or "ok"."""
    with tempfile.TemporaryDirectory(prefix="xenos_batch_try_") as trial_dir:
        trial_path = Path(trial_dir)
        shader_path = trial_path / f"{tag}.bin"
        shader_path.write_bytes(chunk)
        out_path = trial_path / f"{tag}_cache.cpp"

        try:
            proc = subprocess.run(
                [str(xenos_recomp), str(trial_path), str(out_path), str(common_header)],
                capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            return "timeout"

        if proc.returncode != 0:
            return "crash" if proc.returncode < 0 else "error"

        if not out_path.exists():
            return "no-output"

        # See the historical comment on this same check in the old inline
        # loop (removed here, kept in git blame): a gracefully-skipped
        # shader still exits 0 and still writes out_path, so returncode/
        # out_path.exists() alone can't distinguish "converted" from
        # "skipped".
        if b"Skipping shader" in proc.stdout:
            reason_match = re.search(rb"Skipping shader [0-9A-Fa-f]+: (.+?)\.?\r?\n", proc.stdout)
            reason = reason_match.group(1).decode(errors="replace") if reason_match else "unknown"
            return f"skipped-by-xenosrecomp:{reason}"

        return "ok"


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__)
        return 1

    xenos_recomp = Path(sys.argv[1])
    dump_dir = Path(sys.argv[2])
    common_header = Path(sys.argv[3])
    output_cpp = Path(sys.argv[4])
    manifest_path = Path(sys.argv[5]) if len(sys.argv) > 5 else None
    remap_path = Path(sys.argv[6]) if len(sys.argv) > 6 else None

    if not dump_dir.is_dir():
        print(f"xenos_batch_convert: '{dump_dir}' is not a directory, nothing to do")
        output_cpp.write_text("// no shader dump directory found\n")
        return 0

    bin_files = sorted(dump_dir.glob("*.bin"))
    if not bin_files:
        print(f"xenos_batch_convert: no .bin files in '{dump_dir}', nothing to do")
        output_cpp.write_text("// no shaders found\n")
        return 0

    # Real, persistent, content-keyed cache -- lives next to output_cpp so
    # it survives across full reconfigures (it's a real build BY-PRODUCT,
    # not build INPUT, so it's safe to keep outside version control the
    # same way the rest of the shader dump/synthesis pipeline already is).
    cache_path = Path(str(output_cpp) + ".resultcache.json")
    result_cache = _load_result_cache(cache_path)
    cache_hits = 0

    results = []  # (hash_hex, status)
    remap_entries = []  # (tag, ucode_bytes) for successfully-converted shaders
    good_dir_root = Path(tempfile.mkdtemp(prefix="xenos_batch_ok_"))
    good_shaders_dir = good_dir_root / "shaders"
    good_shaders_dir.mkdir()

    new_cache_entries = {}  # content_hash -> {"status": ..., "tag": ...}, this run's fresh results

    kept = 0
    pending = []  # (tag, content_hash, chunk) -- needs a real XenosRecomp run
    for bin_file in bin_files:
        data = bin_file.read_bytes()
        containers = list(find_containers(data))
        if not containers:
            continue
        # A dumped .bin is expected to hold exactly one real shader
        # (shader_dump.cpp writes header+ucode for one CreateVertexShader/
        # CreatePixelShader call), but scan generically in case that changes.
        for idx, (offset, size, virtual_size, physical_size) in enumerate(containers):
            chunk = data[offset:offset + size]
            tag = bin_file.stem if idx == 0 else f"{bin_file.stem}_{idx}"
            content_hash = hashlib.sha256(chunk).hexdigest()

            cached = result_cache.get(content_hash)
            if cached is not None:
                # Real cache hit: this EXACT shader (by content, not name/
                # mtime) has already been run through XenosRecomp before,
                # with this same real result -- skip the subprocess entirely.
                cache_hits += 1
                status = cached["status"]
                results.append((tag, status))
                new_cache_entries[content_hash] = cached
                if status == "ok":
                    (good_shaders_dir / f"{tag}.bin").write_bytes(chunk)
                    kept += 1
                    remap_entries.append(chunk)
                continue

            pending.append((tag, content_hash, chunk))

    # "Only 4 cores used" report: this per-shader
    # isolation loop used to run every pending shader through its own
    # XenosRecomp subprocess ONE AT A TIME on the main thread -- for a
    # ~2200-shader corpus that meant almost the entire batch-convert
    # wall-clock was spent with a single subprocess in flight (whatever
    # internal DXC/LLVM codegen threads that one process spun up was all the
    # parallelism the machine ever showed). Each pending shader already runs
    # in its own subprocess + temp dir, so running many concurrently is
    # exactly as crash-safe as running them one at a time -- a crash still
    # only drops that one shader's result. Deliberately NOT applied to the
    # "combined pass" below or to XenosRecomp's own internal directory-mode
    # loop, which stays on std::execution::seq (see
    # cmake/patches/xenosrecomp_graceful_skip.patch) because of a real,
    # confirmed thread_local DxcCompiler state leak across shaders sharing a
    # worker thread inside ONE process -- that risk doesn't apply here since
    # each of these is a fresh XenosRecomp process with its own memory space.
    max_workers = os.cpu_count() or 4
    if pending:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
            future_to_item = {
                pool.submit(_run_one_shader, xenos_recomp, common_header, tag, chunk): (tag, content_hash, chunk)
                for tag, content_hash, chunk in pending
            }
            for future in concurrent.futures.as_completed(future_to_item):
                tag, content_hash, chunk = future_to_item[future]
                try:
                    status = future.result()
                except Exception as e:
                    status = f"error:{e}"

                results.append((tag, status))

                if status == "timeout":
                    # Real, deliberate exclusion: NOT cached. A timeout can
                    # be a transient machine-load fluke, not a property of
                    # the shader's bytes -- caching it risks permanently
                    # skipping a shader that would actually succeed on a
                    # quieter run.
                    continue

                new_cache_entries[content_hash] = {"status": status}
                if status != "ok":
                    continue

                # Success: keep the source shader for the real combined pass
                # below (re-running XenosRecomp once over ALL good shaders
                # together, so the combined cache is one real dedup'd table
                # rather than a hand-merge of N single-shader C++ files).
                (good_shaders_dir / f"{tag}.bin").write_bytes(chunk)
                kept += 1

                # Real bug found XenosRecomp's own cache-entry hash
                # (XXH3_64bits over the WHOLE container -- main.cpp's
                # `XXH3_64bits(shaderContainer, dataSize)`) is NOT the same
                # value the runtime looks shaders up by. The Vulkan pipeline
                # cache keys native-pipeline lookups on `ucode_data_hash()`
                # (pipeline_cache.cpp's `LoadShader`), which hashes ONLY the
                # ucode dwords the GPU command stream loads -- a different byte
                # range, different hash. This mismatch meant the generalized
                # native-pipeline cache (HasNativePipeline/GetOrCreatePipeline)
                # has never matched ANY batch-converted shader in this
                # project's history -- confirmed via zero real "native pipeline
                # created" log lines ever. Record the real whole container here
                # (see the remap file write below for how it's used) so
                # xenos_cache_unpack.cpp can remap XenosRecomp's container hash
                # to the real one the runtime actually looks up by.
                remap_entries.append(chunk)

    _save_result_cache(cache_path, new_cache_entries)
    print(f"xenos_batch_convert: {cache_hits}/{len(bin_files)} shaders skipped "
          f"(cached result from a previous run)")

    if kept == 0:
        print("xenos_batch_convert: 0/%d shaders converted successfully" % len(results))
        output_cpp.write_text("// all shaders failed to convert\n")
    else:
        # Real, compressed (ZSTD+smol-v) XenosRecomp output -- written
        # directly to output_cpp. Decompressing this into plain SPIR-V words
        # is a SEPARATE build step (tools/xenos_cache_unpack.cpp, chained
        # after this script by renut_xenos_shader_batch() in
        # cmake/rexglue_xenosrecomp.cmake), not done here.
        proc = subprocess.run(
            [str(xenos_recomp), str(good_shaders_dir), str(output_cpp), str(common_header)],
            capture_output=True, timeout=1800)
        if proc.returncode != 0:
            print("xenos_batch_convert: combined pass over %d good shaders FAILED "
                  "unexpectedly (rc=%d) -- stderr:\n%s" % (kept, proc.returncode,
                                                             proc.stderr.decode(errors="replace")))
            return 1

        # After the XenosRecomp crash-safety patch (see
        # cmake/patches/xenosrecomp_graceful_skip.patch) started letting
        # per-shader compile failures inside a MULTI-shader batch skip
        # gracefully instead of crashing the whole process, shaders
        # that convert successfully in ISOLATION (this script's own per-
        # shader subprocess above, the real source of truth for "does this
        # shader work") can still silently fail when recompiled together
        # with ~2000 others in this one combined pass -- confirmed real,
        # deterministic (same exact ~450/2135 shaders fail on repeated runs
        # with identical input, not a race/resource-exhaustion fluke),
        # cause not yet root-caused (suspected thread_local DxcCompiler
        # state leaking between shaders processed by the same worker
        # thread, since main.cpp's std::execution::par_unseq resets
        # ShaderRecompiler but not DxcCompiler between shaders). Detect and
        # report it loudly rather than silently shipping a smaller cache
        # than what individually verified as working -- this real gap
        # would otherwise be invisible (proc.returncode == 0, no error our
        # old code would have noticed).
        combined_skip_count = proc.stdout.count(b"Skipping shader")
        if combined_skip_count > 0:
            print(f"xenos_batch_convert: WARNING {combined_skip_count} shader(s) verified OK in "
                  f"ISOLATION were silently dropped by the combined multi-shader pass (real, "
                  f"confirmed XenosRecomp bug, not this script's own logic -- see this line's own "
                  f"comment). The output cache is missing these shaders even though they "
                  f"individually convert fine.", file=sys.stderr)

    shutil.rmtree(good_dir_root, ignore_errors=True)

    ok = sum(1 for _, s in results if s == "ok")
    print(f"xenos_batch_convert: {ok}/{len(results)} shaders converted "
          f"({len(results) - ok} skipped: crashes/errors isolated per-shader)")

    if manifest_path is not None:
        with manifest_path.open("w") as f:
            for tag, status in results:
                f.write(f"{tag} {status}\n")

    # Real hash-mismatch fix (see the comment above where
    # remap_entries is populated): write the REAL, WHOLE container bytes
    # (virtual+physical sections, exactly what XenosRecomp itself hashes) for
    # every successfully-converted shader. xenos_cache_unpack.cpp reads this
    # and, for each entry, computes BOTH the container hash (matching
    # XenosRecomp's own `XXH3_64bits(shaderContainer, dataSize)` -- the key
    # already in its emitted table) and the real ucode-only hash (matching
    # pipeline_cache.cpp's `LoadShader`/`ucode_data_hash()` -- what the
    # Vulkan pipeline cache's HasNativePipeline()/GetOrCreatePipeline()
    # actually look shaders up by), then remaps the table to use the latter.
    # A length-prefixed stream (not one-per-line text) since container bytes
    # are arbitrary binary, not necessarily printable/newline-safe.
    if remap_path is not None:
        with remap_path.open("wb") as f:
            for container_bytes in remap_entries:
                f.write(len(container_bytes).to_bytes(4, "little"))
                f.write(container_bytes)

    return 0


if __name__ == "__main__":
    sys.exit(main())
