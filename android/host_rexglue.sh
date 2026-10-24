#!/bin/sh
# Link launcher of the SDK's rexglue in the Android build (CMakeLists.txt). The
# SDK's codegen step runs the rexglue it builds, which here would be an Android
# binary: in place of linking it, writes a script that runs the host's
# ($1) to the link's output.
host=$1
shift
out=
prev=
for arg; do
    [ "$prev" = -o ] && out=$arg
    prev=$arg
done
[ -n "$out" ] || { echo "host_rexglue.sh: no -o in the link command" >&2; exit 1; }
printf '#!/bin/sh\nexec "%s" "$@"\n' "$host" > "$out" && chmod +x "$out"
