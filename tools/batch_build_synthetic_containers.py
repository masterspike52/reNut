#!/usr/bin/env python3
"""Runs tools/build_synthetic_container.py against EVERY real raw-ucode
capture in shaders_ucode_hash/ (produced by the renut_dump_ucode_hash cvar,
keyed by the real runtime ucode_data_hash() -- NOT shader_dump.cpp's D3D9-
hook-based captures, which structurally miss most of this game's actual
draw-time shaders (confirmed 0% overlap between shader_dump.cpp's captures
and real `renut color draw pair` draw identities, root-caused to IM_LOAD/
IM_LOAD_IMMEDIATE GPU command-stream shader loads that never call D3D9's
CreateVertexShader/CreatePixelShader at all -- see docs/ai/research.md).

Builds one synthetic ShaderContainer .bin per real .ucode file into
OUTPUT_DIR, using the same real vertex-fetch/texture-fetch analysis
(tools/ucode_analyze) build_synthetic_container.py already uses for single
shaders -- this script is purely the batch driver, no new container-building
logic. OUTPUT_DIR is meant to be fed into tools/xenos_batch_convert.py
exactly like shader_dump.cpp's shaders/ directory is.

Usage:
    batch_build_synthetic_containers.py <ucode_dir> <output_dir>
        [<ucode_analyze_path>] [--reject-heuristic]

--reject-heuristic: skip (don't emit a container for) any vertex shader
    whose semantics needed the fallback POSITION-first/TEXCOORD-follows
    heuristic (no real captured D3D9 vertex declaration available) --
    fix for a confirmed finding: applying that
    heuristic broadly (not just to the one shader it was originally
    verified against) produces genuinely wrong vertex data for shaders
    with different real layouts (normals, colors, multiple UV sets in a
    different order), causing visible in-game geometry corruption once
    ~150 heuristic-based shaders went live simultaneously. Real vertex-
    declaration capture is structurally unavailable for most of these
    shaders (same IM_LOAD root cause as shader creation itself bypassing
    D3D9 hooks -- see docs/ai/research.md), so this flag is the
    practical way to get a smaller but verified-correct native shader set
    instead of a larger but partially-wrong one. Pixel shaders are
    unaffected (no vertex-semantic heuristic exists for them).
"""

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    ucode_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    remaining = sys.argv[3:]
    reject_heuristic = "--reject-heuristic" in remaining
    remaining = [a for a in remaining if a != "--reject-heuristic"]
    ucode_analyze_path = remaining[0] if remaining else None

    if not ucode_dir.is_dir():
        print(f"batch_build_synthetic_containers: '{ucode_dir}' is not a directory")
        return 1

    output_dir.mkdir(parents=True, exist_ok=True)
    build_script = Path(__file__).parent / "build_synthetic_container.py"

    results = []  # (tag, status)
    ucode_files = sorted(ucode_dir.glob("*.ucode"))
    for ucode_path in ucode_files:
        stem = ucode_path.stem  # e.g. "vs_1e6883fccde1f688"
        if "_" not in stem:
            continue
        kind, hex_hash = stem.split("_", 1)
        if kind not in ("vs", "ps"):
            continue

        out_path = output_dir / f"{stem}.bin"
        cmd = [sys.executable, str(build_script), str(ucode_dir), kind, hex_hash, str(out_path)]
        if ucode_analyze_path:
            cmd += ["--ucode-analyze", ucode_analyze_path]
        if reject_heuristic and kind == "vs":
            cmd += ["--reject-heuristic"]

        try:
            proc = subprocess.run(cmd, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            results.append((stem, "timeout"))
            continue

        if proc.returncode == 0 and out_path.exists():
            results.append((stem, "ok"))
        elif proc.returncode == 2:
            # build_synthetic_container.py's own
            # REJECTED line on stderr already distinguishes "no real vertex
            # declaration, --reject-heuristic given" from "references a
            # missing-literal-constant register" -- surface which one
            # instead of collapsing both into "rejected-heuristic", so the
            # manifest can actually be used to see why without re-running
            # the script by hand.
            reason = "rejected-heuristic"
            marker = b"build_synthetic_container: REJECTED"
            idx = proc.stderr.find(marker)
            if idx != -1:
                line = proc.stderr[idx:].split(b"\n", 1)[0]
                after_dashes = line.split(b" -- ", 1)
                if len(after_dashes) == 2:
                    reason = f"rejected:{after_dashes[1].decode(errors='replace')}"
            results.append((stem, reason))
        else:
            results.append((stem, "fail"))

    ok = sum(1 for _, s in results if s == "ok")
    print(f"batch_build_synthetic_containers: {ok}/{len(results)} synthetic containers built "
          f"({len(results) - ok} failed -- usually 'no vertex_bindings' for shaders with no "
          f"real vfetch instructions, or a ucode_analyze crash on unusual control flow)")

    fail_reasons = {}
    for tag, status in results:
        if status != "ok":
            fail_reasons[status] = fail_reasons.get(status, 0) + 1
    for reason, count in sorted(fail_reasons.items(), key=lambda kv: -kv[1]):
        print(f"  {count} x {reason}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
