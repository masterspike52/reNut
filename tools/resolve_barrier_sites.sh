#!/bin/sh
# Resolves the barrier-site addresses logged by the renut plugin to source
# locations. The plugin logs raw return addresses because tagging all 18
# PushBufferMemoryBarrier call sites by hand would be error-prone.
#
#   tools/resolve_barrier_sites.sh [log-file]
set -eu

SO="${RENUT_PLUGIN:-/home/nick/Desktop/reNut/out/build/linux-amd64-relwithdebinfo/librexgpu-renutrd.so}"
LOG="${1:-$(ls -t "$HOME"/.local/state/renut/logs/*.log | head -1)}"

[ -f "$SO" ] || { echo "plugin not found: $SO" >&2; exit 1; }

# spdlog rotates logs by size (renut_NNN.log, .1.log, .2.log, ...), so a single
# play session - including the process-start line with the plugin base address
# - can be split across several files. Search all rotated parts belonging to
# the same base name, oldest first, so the last match is the most recent one.
LOG_BASE=$(printf '%s' "$LOG" | sed -E 's/\.[0-9]+\.log$/.log/; s/\.log$//')
LOG_PARTS=$(ls -tr "${LOG_BASE}".log "${LOG_BASE}".*.log 2>/dev/null || true)

# The load address must be subtracted from the logged runtime address to get a
# file offset addr2line can use.
BASE=$(grep -h -oE "renut: plugin base 0x[0-9a-f]+" $LOG_PARTS 2>/dev/null | tail -1 | grep -oE "0x[0-9a-f]+" || true)

echo "plugin: $SO"
echo "log:    $LOG"
if [ -n "$BASE" ]; then
    echo "base:   $BASE"
else
    echo "base:   (not logged - addresses assumed to be file offsets)"
fi
echo

grep -oE "renut barrier site 0x[0-9a-f]+ -> [0-9]+ per frame" "$LOG" \
  | tail -20 \
  | while read -r _ _ _ addr _ count _ _; do
        if [ -n "$BASE" ]; then
            off=$(printf '0x%x' $(( addr - BASE )))
        else
            off="$addr"
        fi
        loc=$(addr2line -e "$SO" -f -C -p "$off" 2>/dev/null | head -1)
        printf '%8s/frame  %s\n' "$count" "${loc:-<unresolved> $addr}"
    done
