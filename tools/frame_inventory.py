#!/usr/bin/env python3
"""Full per-frame cost inventory from a renut frame trace.

Accounts for every millisecond of the GPU command thread, ranks each component
by cost, and flags which ones are worth attacking. Use this to choose a target
instead of guessing.

    tools/frame_inventory.py trace.csv [--min-draws N]
"""

import argparse
import csv
import statistics
import sys

# label, csv key, kind, note
# kind: "phase" = inside IssueDraw, "top" = sibling of IssueDraw, "count" = not a time
COMPONENTS = [
    ("IssueDraw (draw submission)", "issuedraw_ms", "top",
     "sum of the phases below"),
    ("  RequestTextures", "tex_ms", "phase",
     "texture fetch parse + load; early-out already added"),
    ("  UpdateBindings", "bind_ms", "phase",
     "constant buffers + descriptor sets"),
    ("    vkUpdateDescriptorSets", "updsets_ms", "phase",
     "subset of UpdateBindings"),
    ("  vertex fetch residency", "vfetch_ms", "phase",
     "shared-memory range requests per vertex buffer"),
    ("  UpdateSystemConstants", "sysconst_ms", "phase",
     "system constant block"),
    ("  samplers", "samp_ms", "phase",
     "GetSamplerParameters + UseSampler per binding"),
    ("  render targets", "rt_ms", "phase",
     "render_target_cache_->Update"),
    ("  primitive processing", "prim_ms", "phase",
     "index buffer conversion"),
    ("  ConfigurePipeline", "pipe_ms", "phase",
     "pipeline state lookup"),
    ("  dynamic state", "dyn_ms", "phase",
     "viewport/scissor/depth bias"),
    ("  post-pipeline (superset)", "post_ms", "phase",
     "SUPERSET: contains bind/vfetch/sysconst/dyn"),
    ("  GPU fence wait", "fencewait_ms", "phase",
     "blocking wait on a frame in flight"),
    ("IssueCopy (EDRAM resolve)", "issuecopy_ms", "top",
     "Xenos tile memory resolve"),
    ("IssueSwap (present)", "swap_ms", "top",
     "gamma/FXAA/blit + queue present"),
    ("ExecutePrimaryBuffer (PM4)", "execprimary_ms", "top",
     "SUPERSET: packet parsing, contains IssueDraw/Copy"),
    ("GPU thread stall (idle)", "stall_ms", "top",
     "waiting for the guest CPU to produce commands"),
    ("pending_fns (cross-thread)", "pendingfns_ms", "top",
     "callbacks marshalled onto the GPU thread"),
    ("CPU blocked on GPU", "gpuwait_ms", "top",
     "presenter thread in vkWaitForFences - the REAL GPU cost"),
]

COUNTS = [
    ("draws", "draws", "draws entering IssueDraw"),
    ("PM4 type-3 packets", "pkt3", "command packets (draws, state, sync)"),
    ("PM4 type-0 packets", "pkt0", "register-write packets"),
    ("guest register writes", "regwrites", "WriteRegister calls"),
    ("EDRAM resolves", "issuecopy_n", "IssueCopy invocations"),
    ("render target switches", "rt_switch", "surface/mode changes"),
    ("stalls", "stall_n", "times the thread ran out of work"),
    ("primary buffer executions", "execprimary_n", "ring buffer batches"),
    ("RequestTextures calls", "texcalls", "Vulkan texture-cache entries"),
    ("  ...needing NO transition", "tex_notrans", "candidates for an early-out"),
    ("GPU fence waits", "gpuwait_n", "times the CPU blocked on the GPU"),
    ("render passes begun", "rp_n", "each does a full LOAD + STORE of its target"),
    ("  ...small (<256x256)", "rp_small", "shadow atlases etc"),
    ("  ...render-target transfers", "rp_transfer", "EDRAM tile copies"),
    ("barrier submits", "barrier_submits", "each ENDS the render pass"),
    ("  buffer barriers", "barrier_buf", ""),
    ("  image barriers", "barrier_img", ""),
    ("shared mem: read-write", "use_rw", "memexport with known extent"),
    ("shared mem: read-write (conservative)", "use_rw_cons", "memexport-capable, unknown extent"),
    ("shared mem: read-only", "use_read", "no memexport - needs no barrier"),
]


def load(path, min_draws, warmup):
    meta, rows, bad = {}, [], 0
    with open(path, newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                for tok in line.lstrip("#").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta[k] = v
                continue
            handle.seek(0)
            break
        for row in csv.DictReader(r for r in handle if not r.startswith("#")):
            if None in row or any(v in (None, "") for v in row.values()):
                bad += 1
                continue
            try:
                p = {k: float(v) for k, v in row.items()}
            except (ValueError, TypeError):
                bad += 1
                continue
            if p["draws"] >= min_draws:
                rows.append(p)
    if bad > 1:
        print(f"  warning: {bad} malformed rows skipped", file=sys.stderr)
    return meta, rows[warmup:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--min-draws", type=int, default=3000)
    ap.add_argument("--warmup", type=int, default=60)
    args = ap.parse_args()

    meta, rows = load(args.trace, args.min_draws, args.warmup)
    if not rows:
        sys.exit("no frames matched — lower --min-draws")

    def med(key):
        vals = [r[key] for r in rows if key in r]
        return statistics.median(vals) if vals else None

    frame = med("frame_ms")
    draws = med("draws")
    rdoc = meta.get("renderdoc") == "1"

    print(f"\n{'=' * 74}")
    print(f"FRAME COST INVENTORY  —  {meta.get('backend', '?')}"
          f"{'  [RENDERDOC ATTACHED — inflated]' if rdoc else ''}")
    print(f"{len(rows)} frames, median {draws:.0f} draws/frame, "
          f"{frame:.2f} ms/frame = {1000 / frame:.0f} fps")
    print(f"{'=' * 74}\n")

    measured = []
    for label, key, kind, note in COMPONENTS:
        v = med(key)
        if v is None:
            continue
        measured.append((label, key, kind, note, v))

    print(f"{'component':<32}{'ms':>8}{'% frame':>9}   note")
    print(f"{'-' * 74}")
    for label, key, kind, note, v in measured:
        print(f"{label:<32}{v:>8.2f}{v / frame * 100:>8.1f}%   {note}")

    # Anything not covered by the top-level siblings is unmeasured.
    tops = {k: v for _, k, kind, _, v in measured if kind == "top"}
    # gpuwait_ms is on the presenter thread and overlaps this one, so it is not
    # part of the GPU-command-thread budget.
    accounted = (tops.get("execprimary_ms", 0) + tops.get("swap_ms", 0)
                 + tops.get("stall_ms", 0) + tops.get("pendingfns_ms", 0))
    print(f"{'-' * 74}")
    print(f"{'accounted (top-level)':<32}{accounted:>8.2f}{accounted / frame * 100:>8.1f}%")
    print(f"{'UNMEASURED':<32}{frame - accounted:>8.2f}"
          f"{(frame - accounted) / frame * 100:>8.1f}%")

    print(f"\n{'per-frame counts':<32}{'n':>10}{'per draw':>11}")
    print(f"{'-' * 74}")
    for label, key, note in COUNTS:
        v = med(key)
        if v is None:
            continue
        print(f"{label:<32}{v:>10.0f}{v / draws:>11.3f}   {note}")

    # Render pass load/store traffic: every pass reads and writes its whole
    # attachment (LOAD_OP_LOAD + STORE_OP_STORE), so this is real bandwidth.
    rp_mpixels = med("rp_mpixels")
    if rp_mpixels:
        # rp_mpixels already counts each attachment separately; LOAD+STORE
        # means every attachment pixel is read once and written once.
        gb = rp_mpixels * 1e6 * 4 * 2 / 1e9
        fps = 1000.0 / frame
        print(f"\n{'=' * 74}")
        print("RENDER PASS LOAD/STORE TRAFFIC (the GPU-side suspect)")
        print(f"{'=' * 74}")
        print(f"  {'attachment pixels (all targets)':<34}{rp_mpixels:>10.1f} Mpx/frame")
        print(f"  {'implied bandwidth':<34}{gb:>10.2f} GB/frame"
              f"  = {gb * fps:.0f} GB/s at {fps:.0f} fps")
        print("  (read + write, 4 B/px, summed over every attachment)")
        print("  RX 9070 peak is ~640 GB/s; near or above that means bandwidth-bound.")

    # Rank only leaf phases — supersets would double-count.
    superset = {"post_ms", "issuedraw_ms", "execprimary_ms", "updsets_ms"}
    leaves = [(l, v) for l, k, kind, _, v in measured
              if k not in superset and v > 0.01]
    leaves.sort(key=lambda x: -x[1])
    print(f"\n{'=' * 74}")
    print("RANKED BY COST (leaf components only — supersets excluded)")
    print(f"{'=' * 74}")
    for i, (label, v) in enumerate(leaves[:12], 1):
        ceiling = v / frame * 100
        verdict = ("worth attacking" if v >= 2.0 else
                   "marginal" if v >= 1.0 else "not worth it (<1 ms)")
        print(f"{i:>2}. {label.strip():<30}{v:>7.2f} ms  "
              f"max {ceiling:>4.1f}% of frame  — {verdict}")
    print()


if __name__ == "__main__":
    main()
