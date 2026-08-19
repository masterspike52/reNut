#!/bin/sh
# Builds tools/ucode_analyze.cpp as a standalone binary, linked against real
# rexglue-sdk shader-analysis source (Shader::AnalyzeUcode and its
# dependencies) but NOT the game's Vulkan/PPC runtime -- this is a real,
# working, offline tool, not a stub.
#
# The exact source list below was found empirically: started from just
# shader.cpp+translator.cpp (confirmed via direct #include inspection to have
# no Vulkan/PPC-runtime dependency) and resolved each real missing linker
# symbol one at a time (fmt's compiled bits, rex::filesystem/env/cvar/memory,
# ucode Disassemble() implementations, utf8 conversion) rather than guessing
# a source list up front. dump_shaders is stubbed locally in
# ucode_analyze.cpp instead of linking the real graphics/flags.cpp, because
# that file pulls in a RenderDoc integration this analysis-only tool has no
# use for.
set -eu

SDK="${SDK_SRC:-/home/nick/Desktop/reNut-build-scratch/rexglue-sdk-src}"
OUT="${1:-/tmp/ucode_analyze}"

cd "$SDK"
g++ -std=gnu++23 -O2 -mssse3 -DNDEBUG \
  -I include -I . \
  -I thirdparty/spdlog/include -I thirdparty/fmt/include -I thirdparty/simde \
  -I thirdparty/tomlplusplus/include -I thirdparty/cli11/include -I thirdparty/xxHash \
  -I thirdparty/renderdoc -I thirdparty/utfcpp/source \
  -DSPDLOG_FMT_EXTERNAL \
  "$OLDPWD/tools/ucode_analyze.cpp" \
  src/graphics/pipeline/shader/shader.cpp \
  src/graphics/pipeline/shader/translator.cpp \
  src/graphics/pipeline/shader/translator_disasm.cpp \
  src/graphics/format/ucode.cpp \
  src/core/string_buffer.cpp \
  src/core/logging.cpp \
  src/core/cvar.cpp \
  src/core/memory.cpp \
  src/core/filesystem_posix.cpp \
  src/core/platform/env_posix.cpp \
  src/core/utf8.cpp \
  thirdparty/fmt/src/format.cc \
  thirdparty/fmt/src/os.cc \
  -o "$OUT"

echo "build_ucode_analyze: wrote $OUT"
