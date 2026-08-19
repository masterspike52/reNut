#!/bin/sh
# Syntax-checks rexglue-sdk GPU sources with GCC (-fsyntax-only).
# The real build uses clang; this exists so SDK-side edits can be verified
# without clang present. Pass file paths relative to the SDK source root,
# or no args to check the Vulkan backend plus the renut plugin sources.
set -eu

SDK="${SDK_SRC:-/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src}"
cd "$SDK"

CXX="${CXX:-g++}"

set -- ${1:+"$@"}
if [ "$#" -eq 0 ]; then
    set -- src/graphics/vulkan/*.cpp src/graphics/plugin_main.cpp
    if [ -d src/graphics/renut ]; then
        set -- "$@" src/graphics/renut/*.cpp
    fi
fi

# -I../reNut/src (RENUT_REPO_SRC below) is REQUIRED for renut_xenos_pipeline_cache.cpp
# to actually type-check its real body: that file gates most of its own code on
# `#if __has_include("renut_engine/renut_xenos_shader_cache.h")`, a build-generated
# header that lives in reNut's own repo, not this SDK checkout. Without this include
# path, RENUT_HAVE_XENOS_SHADER_CACHE silently evaluates to 0 and the ENTIRE real
# function bodies go unchecked -- confirmed real: a genuine duplicate-variable-
# declaration bug in that file passed this exact script with EXIT=0 before this path
# was added, because the code containing the bug was never actually compiled.
RENUT_REPO_SRC="${RENUT_REPO_SRC:-/home/nick/Desktop/reNut/src}"

FLAGS="-std=gnu++23 -fsyntax-only
-DVK_ENABLE_BETA_EXTENSIONS -DVK_USE_PLATFORM_XCB_KHR -DSPDLOG_FMT_EXTERNAL -DREX_HAS_VULKAN=1
-Iinclude -I. -I$RENUT_REPO_SRC
-Ithirdparty/spdlog/include -Ithirdparty/fmt/include -Ithirdparty/renderdoc
-Ithirdparty/vulkan-headers/include -Ithirdparty/volk
-Ithirdparty/vulkan-memory-allocator/include -Ithirdparty/xxHash -Ithirdparty/snappy
-Ithirdparty/simde -Ithirdparty/tracy/public -Ithirdparty/tomlplusplus/include
-Ithirdparty/utfcpp/source -Ithirdparty/disruptorplus/include
-Ithirdparty/glslang -Ithirdparty/glslang/glslang/Include -Ithirdparty/imgui"

# Both configurations must build: the renut plugin defines RENUT_OPTIMISATIONS,
# xenos does not, and code inside #if blocks is only checked in one of them.
status=0
for f in "$@"; do
    [ -f "$f" ] || continue
    for def in "-DRENUT_OPTIMISATIONS=1" ""; do
        label=$([ -n "$def" ] && echo "renut" || echo "xenos")
        printf '%-52s %-6s ' "$f" "$label"
        if out=$($CXX $FLAGS $def "$f" 2>&1); then
            echo "OK"
        else
            echo "FAIL"
            echo "$out" | head -25
            status=1
        fi
    done
done
exit $status
