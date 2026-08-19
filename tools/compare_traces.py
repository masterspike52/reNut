#!/usr/bin/env python3
"""Compare renut/xenos frame traces produced by the renut_frame_trace cvar.

Usage:
    tools/compare_traces.py baseline.csv candidate.csv
    tools/compare_traces.py single.csv

Frames are filtered to a steady-state window before comparison: rows below
--min-draws are treated as menus/loading screens rather than the workload under
test, and the first --warmup qualifying frames are dropped so shader compilation
and cache population do not count against the run.
"""

import argparse
import csv
import statistics
import sys

METRICS = [
    ("frame_ms", "frame time", "ms"),
    ("issuedraw_ms", "IssueDraw total", "ms"),
    ("ns_per_draw", "per draw", "ns"),
    ("tex_ms", "  RequestTextures", "ms"),
    ("bind_ms", "  UpdateBindings", "ms"),
    ("updsets_ms", "    vkUpdateDescriptorSets", "ms"),
    ("sysconst_ms", "  UpdateSystemConstants", "ms"),
    ("vfetch_ms", "  vertex fetch residency", "ms"),
    ("post_ms", "  post-pipeline (superset)", "ms"),
    ("samp_ms", "  samplers", "ms"),
    ("rt_ms", "  render targets", "ms"),
    ("prim_ms", "  primitive processing", "ms"),
    ("pipe_ms", "  ConfigurePipeline", "ms"),
    ("dyn_ms", "  dynamic state", "ms"),
    ("fencewait_ms", "  GPU fence wait (BLOCKING)", "ms"),
    ("issuecopy_ms", "IssueCopy / EDRAM resolve", "ms"),
]

# Redundancy rates: how often a draw could reuse the previous draw's state.
# Reported as a fraction of draws, so they are comparable across scenes.
RATES = [
    ("pipe_same", "same pipeline as prev draw"),
    ("layout_same", "same pipeline layout"),
    ("texmask_same", "same used-texture mask"),
    ("sets_valid", "all descriptor sets valid"),
    ("samp_reused", "sampler setup reused"),
    ("mergeable", "mergeable into prev draw"),
    ("scissor_empty", "zero-area scissor (invisible)"),
    ("viewport_tiny", "sub-pixel viewport"),
    ("offscreen", "viewport outside scissor"),
    ("depthonly", "depth-only (shadow/prepass)"),
    ("nopixelshader", "no pixel shader"),
    ("smallrt", "small render target (atlas)"),
    ("rt_switch", "render target switches"),
    ("issuecopy_n", "EDRAM resolves"),
]


def load(path, min_draws, warmup):
    meta = {}
    rows = []
    malformed = 0
    with open(path, newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                for token in line.lstrip("#").split():
                    if "=" in token:
                        key, value = token.split("=", 1)
                        meta[key] = value
                continue
            handle.seek(0)
            break
        reader = csv.DictReader(row for row in handle if not row.startswith("#"))
        for row in reader:
            # A run killed mid-write leaves one truncated final line; anything
            # beyond that means the trace is not trustworthy.
            if None in row or any(v in (None, "") for v in row.values()):
                malformed += 1
                continue
            try:
                parsed = {k: float(v) for k, v in row.items()}
            except (ValueError, TypeError):
                malformed += 1
                continue
            if parsed["draws"] >= min_draws:
                rows.append(parsed)
    if malformed > 1:
        print(f"  warning: {path}: {malformed} malformed rows skipped", file=sys.stderr)
    return meta, rows[warmup:]


def summarise(rows, key):
    values = [r[key] for r in rows if key in r]
    if not values:
        return None
    values.sort()
    return {
        "median": statistics.median(values),
        "p95": values[min(int(len(values) * 0.95), len(values) - 1)],
        "mean": statistics.fmean(values),
    }


def describe(path, meta, rows):
    if not rows:
        sys.exit(f"{path}: no frames matched the filter — lower --min-draws or capture more")
    draws = summarise(rows, "draws")
    label = meta.get("backend", "?")
    warn = "  [RENDERDOC ATTACHED]" if meta.get("renderdoc") == "1" else ""
    print(f"{path}\n  backend={label} frames={len(rows)} "
          f"median draws/frame={draws['median']:.0f}{warn}")
    return label


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("candidate", nargs="?")
    parser.add_argument("--min-draws", type=int, default=800,
                        help="ignore frames below this draw count (default: 800)")
    parser.add_argument("--warmup", type=int, default=120,
                        help="drop this many qualifying frames first (default: 120)")
    args = parser.parse_args()

    base_meta, base_rows = load(args.baseline, args.min_draws, args.warmup)
    print()
    describe(args.baseline, base_meta, base_rows)

    if not args.candidate:
        print()
        for key, label, unit in METRICS:
            stat = summarise(base_rows, key)
            if stat:
                print(f"  {label:<26} {stat['median']:9.2f} {unit:<3} "
                      f"(p95 {stat['p95']:.2f})")
        draws = summarise(base_rows, "draws")
        if draws and any(k in base_rows[0] for k, _ in RATES):
            print("\n  redundancy rates (fraction of draws):")
            for key, label in RATES:
                stat = summarise(base_rows, key)
                if stat:
                    print(f"    {label:<30} {stat['median'] / draws['median'] * 100:5.1f}%")
        print()
        return

    cand_meta, cand_rows = load(args.candidate, args.min_draws, args.warmup)
    describe(args.candidate, cand_meta, cand_rows)

    if base_meta.get("renderdoc") != cand_meta.get("renderdoc"):
        print("\n  !! RenderDoc state differs between runs — results are NOT comparable")

    print(f"\n  {'metric':<26} {'baseline':>10} {'candidate':>10} {'change':>10}")
    print(f"  {'-' * 58}")
    for key, label, unit in METRICS:
        base = summarise(base_rows, key)
        cand = summarise(cand_rows, key)
        if not base or not cand:
            continue
        delta = cand["median"] - base["median"]
        pct = (delta / base["median"] * 100) if base["median"] else 0.0
        arrow = "faster" if delta < 0 else "slower" if delta > 0 else ""
        print(f"  {label:<26} {base['median']:10.2f} {cand['median']:10.2f} "
              f"{pct:+9.1f}% {arrow}")
    print()


if __name__ == "__main__":
    main()
