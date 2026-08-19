// Standalone helper for tools/xenos_batch_convert.py.
//
// XenosRecomp's directory-mode output (see XenosRecomp/main.cpp) is a C++
// source defining a ShaderCacheEntry table plus one ZSTD-compressed,
// smol-v-encoded SPIR-V blob shared by all shaders (offsets/sizes into it
// live in each entry). That's convenient for a project that already links
// zstd+smol-v into its runtime (as XenosRecomp's own upstream consumers do),
// but reNut's Vulkan GPU plugin (rexgpu-renut) is a separate CMake build
// from reNut's own executable (which is the only place XenosRecomp's
// FetchContent-provided zstd/smol-v targets exist) -- wiring that dependency
// across the plugin boundary is real, avoidable complexity for what this
// needs.
//
// So: decompress everything at BUILD TIME instead. This tool takes
// XenosRecomp's own generated cache .cpp, extracts+decompresses+decodes the
// real per-shader SPIR-V bytes, and re-emits a plain, self-contained C++
// source with one flat uint8_t array holding every shader's real (already
// decoded, ready-to-pass-to-vkCreateShaderModule) SPIR-V back to back, plus
// an entry table of (hash, offset, size) pairs -- zero runtime dependencies
// beyond what's already linked into rexgpu-renut.
//
// This is a source-to-source rewrite: it does NOT re-invoke XenosRecomp or
// touch shader semantics, only decodes the compression XenosRecomp itself
// applied. Parses the exact textual format XenosRecomp's main.cpp emits
// (fmt::println calls with fixed field order) rather than a general C++
// parser -- brittle by nature of matching one specific generator's output,
// documented and isolated on purpose so if XenosRecomp's output format ever
// changes, only this one file needs updating.

#include <zstd.h>

#include "smolv.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "renut_engine/renut_xenos_shader_cache.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Entry {
  uint64_t hash = 0;
  size_t spirvOffset = 0;
  size_t spirvSize = 0;
  uint32_t specConstantsMask = 0;
  // Filled in from the remap file's vertexLocations map after hash
  // remapping (main()); left all-unset (kRenutVertexLocationUnset) for
  // pixel shaders and for anything with no remap match, matching
  // RenutXenosShaderCacheEntry's own real, documented field semantics.
  std::array<uint8_t, kRenutMaxVertexLocations> vertexLocations{};
  // See kRenutTexCoordSemanticUnset's header comment -- same fill/remap
  // treatment as vertexLocations above.
  std::array<uint8_t, kRenutMaxVertexLocations> vertexTexCoordSemanticIndex{};
};

// Real ShaderContainer header layout (see XenosRecomp/XenosRecomp/shader.h),
// big-endian on disk -- matches tools/xenos_batch_convert.py's own
// _CONTAINER_STRUCT exactly. Only the fields needed to locate the physical
// (ucode) section are read here; the rest exist only to keep the struct's
// real size (36 bytes) correct for pointer arithmetic.
struct ShaderContainerHeader {
  uint32_t flags;
  uint32_t virtualSize;
  uint32_t physicalSize;
  uint32_t fieldC;
  uint32_t constantTableOffset;
  uint32_t definitionTableOffset;
  uint32_t shaderOffset;
  uint32_t field1C;
  uint32_t field20;
};
static_assert(sizeof(ShaderContainerHeader) == 36, "must match the real 36-byte ShaderContainer header");

uint32_t ReadBigEndianU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// the compiled SPIR-V vertex shader stage declares a
// FIXED Vulkan input location per D3D9 usage/usageIndex (XenosRecomp's own
// shader_recompiler.cpp, at the point it emits "in {type} i{var}{idx} :
// {semantic}{idx}" for each element of VertexShader::
// vertexElementsAndInterpolators[]) -- NOT sequential 0,1,2,... in
// vfetch-instruction order, which is what the runtime pipeline cache
// (renut_xenos_pipeline_cache.cpp) previously assumed. This table is a
// direct, verbatim port of XenosRecomp's real USAGE_LOCATIONS array (same
// file) -- must be kept in sync if XenosRecomp's own table ever changes.
struct DeclUsageLocation {
  uint32_t usage;
  uint32_t usageIndex;
  uint32_t location;
};
// DeclUsage enum values (XenosRecomp/shader.h): Position=0, BlendWeight=1,
// BlendIndices=2, Normal=3, PointSize=4, TexCoord=5, Tangent=6, Binormal=7,
// TessFactor=8, PositionT=9, Color=10, Fog=11, Depth=12, Sample=13.
constexpr DeclUsageLocation kUsageLocations[] = {
    {0, 0, 0},   // Position0
    {3, 0, 1},   // Normal0
    {6, 0, 2},   // Tangent0
    {7, 0, 3},   // Binormal0
    {5, 0, 4},   // TexCoord0
    {5, 1, 5},   // TexCoord1
    {5, 2, 6},   // TexCoord2
    {5, 3, 7},   // TexCoord3
    {10, 0, 8},  // Color0
    {2, 0, 9},   // BlendIndices0
    {1, 0, 10},  // BlendWeight0
    {10, 1, 11}, // Color1
    {5, 4, 12},  // TexCoord4
    {5, 5, 13},  // TexCoord5
    {5, 6, 14},  // TexCoord6
    {5, 7, 15},  // TexCoord7
    {0, 1, 15},  // Position1
};

// Real VertexElement bitfield layout (XenosRecomp/shader.h): 12 bits
// address, 4 bits usage, 4 bits usageIndex, packed into one be<uint32_t>
// (big-endian on disk, byte-swapped like every other be<T> in this format).
// Reading the raw 32-bit value and masking/shifting avoids relying on this
// TU's own bitfield layout matching the real on-disk one exactly.
uint32_t VertexElementUsage(uint32_t raw) { return (raw >> 12) & 0xF; }
uint32_t VertexElementUsageIndex(uint32_t raw) { return (raw >> 16) & 0xF; }

// SUPERSEDED by positional location assignment in
// ExtractVertexLocations below; retained only to document the scheme this
// build used to mirror from XenosRecomp's USAGE_LOCATIONS table.
[[maybe_unused]] uint8_t LookUpVertexLocation(uint32_t usage, uint32_t usageIndex) {
  for (const DeclUsageLocation& entry : kUsageLocations) {
    if (entry.usage == usage && entry.usageIndex == usageIndex) {
      return uint8_t(entry.location);
    }
  }
  return kRenutVertexLocationUnset;
}

// Real ShaderContainer's Shader base (XenosRecomp/shader.h's `struct
// Shader`) is 24 bytes (6 be<uint32_t> fields) before any VertexShader/
// PixelShader-specific fields begin. VertexShader adds field18 (offset 24),
// vertexElementCount (offset 28), field20 (offset 32), then
// vertexElementsAndInterpolators[] starts at offset 36 -- all relative to
// shaderOffset (the container-relative offset the real ShaderContainer
// header's own shaderOffset field points to), not the container start.
constexpr uint32_t kShaderBaseSize = 24;
constexpr uint32_t kVertexShaderFieldsSize = 12;  // field18 + vertexElementCount + field20

// Extracts real per-attribute Vulkan locations for a vertex shader's
// container, in vertexElementsAndInterpolators[] order (the SAME order
// tools/ucode_analyze.cpp's real Shader::AnalyzeUcode()-derived
// vertex_bindings() walks, since both describe the same underlying real
// vfetch-instruction sequence) -- up to kRenutMaxVertexLocations entries,
// unused slots left at kRenutVertexLocationUnset. Returns an all-unset
// array for anything that doesn't look like a real, well-formed vertex
// shader container (pixel shaders, malformed data) rather than asserting --
// this is diagnostic/best-effort data, a miss here just means the runtime
// falls back to its own (real, but not always correct) sequential
// assignment, not a hard failure.
std::array<uint8_t, kRenutMaxVertexLocations> ExtractVertexLocations(
    const std::vector<uint8_t>& container, uint32_t shaderOffset, bool isVertexShader) {
  std::array<uint8_t, kRenutMaxVertexLocations> locations{};
  locations.fill(kRenutVertexLocationUnset);
  if (!isVertexShader) return locations;

  if (size_t(shaderOffset) + kShaderBaseSize + kVertexShaderFieldsSize > container.size()) {
    std::fprintf(stderr, "xenos_cache_unpack DIAG: reject bounds1 shaderOffset=%u container=%zu\n",
                 shaderOffset, container.size());
    return locations;
  }
  const uint8_t* shaderData = container.data() + shaderOffset;
  const uint32_t vertexElementCount =
      ReadBigEndianU32(shaderData + kShaderBaseSize + 4);  // field18, vertexElementCount, field20
  const uint32_t field18 = ReadBigEndianU32(shaderData + kShaderBaseSize);
  const uint32_t elementsStart = kShaderBaseSize + kVertexShaderFieldsSize;

  if (vertexElementCount == 0 || vertexElementCount > kRenutMaxVertexLocations) {
    std::fprintf(stderr, "xenos_cache_unpack DIAG: reject count vertexElementCount=%u field18=%u\n",
                 vertexElementCount, field18);
    return locations;
  }
  const size_t elementsEnd =
      size_t(shaderOffset) + elementsStart + size_t(field18 + vertexElementCount) * 4;
  if (elementsEnd > container.size()) {
    std::fprintf(stderr, "xenos_cache_unpack DIAG: reject bounds2 elementsEnd=%zu container=%zu "
                 "field18=%u count=%u\n",
                 elementsEnd, container.size(), field18, vertexElementCount);
    return locations;
  }

  // locations are now POSITIONAL -- the i-th vertex
  // element gets location i -- exactly mirroring the [[vk::location(i)]]
  // XenosRecomp now emits for the same element, in the same
  // vertexElementsAndInterpolators[] order. This function and that emission
  // site are the two halves of one contract; changing either alone silently
  // binds the wrong buffer data to the wrong shader input.
  //
  // Previously both sides looked the usage up in XenosRecomp's
  // Unleashed-specific USAGE_LOCATIONS table, which (a) had no entry for the
  // POSITION2..12 / TEXCOORD8..12 this game uses, so 198 vertex shaders
  // failed to compile at all, and (b) mapped both (Position,1) and
  // (TexCoord,7) to location 15, so widening it would have aliased two
  // attributes onto one location. Indexing positionally removes the lookup,
  // and with it both failure modes -- and it matches the hardware, where a
  // vertex shader pulls its own data via vfetch and the D3D9 usage semantic
  // never reaches the GPU at all.
  for (uint32_t i = 0; i < vertexElementCount; ++i) {
    locations[i] = uint8_t(i);
  }
  return locations;
}

// Real per-raw-attribute D3D9 TexCoord usageIndex (0-7), or
// kRenutTexCoordSemanticUnset for any non-TexCoord usage -- see
// kRenutTexCoordSemanticUnset's header comment for why this is still needed
// even though vertex locations themselves went positional. Reads the SAME
// raw vertexElementsAndInterpolators[] entries ExtractVertexLocations reads,
// just decoding the usage/usageIndex bitfields (VertexElementUsage/
// VertexElementUsageIndex) instead of discarding them for a plain index.
// DeclUsage::TexCoord == 5, see the enum-value comment above kUsageLocations.
constexpr uint32_t kDeclUsageTexCoord = 5;

std::array<uint8_t, kRenutMaxVertexLocations> ExtractVertexTexCoordSemanticIndices(
    const std::vector<uint8_t>& container, uint32_t shaderOffset, bool isVertexShader) {
  std::array<uint8_t, kRenutMaxVertexLocations> semanticIndices{};
  semanticIndices.fill(kRenutTexCoordSemanticUnset);
  if (!isVertexShader) return semanticIndices;

  if (size_t(shaderOffset) + kShaderBaseSize + kVertexShaderFieldsSize > container.size()) {
    return semanticIndices;
  }
  const uint8_t* shaderData = container.data() + shaderOffset;
  const uint32_t vertexElementCount =
      ReadBigEndianU32(shaderData + kShaderBaseSize + 4);  // field18, vertexElementCount, field20
  const uint32_t field18 = ReadBigEndianU32(shaderData + kShaderBaseSize);
  const uint32_t elementsStart = kShaderBaseSize + kVertexShaderFieldsSize;

  if (vertexElementCount == 0 || vertexElementCount > kRenutMaxVertexLocations) {
    return semanticIndices;
  }
  const size_t elementsEnd =
      size_t(shaderOffset) + elementsStart + size_t(field18 + vertexElementCount) * 4;
  if (elementsEnd > container.size()) {
    return semanticIndices;
  }

  const uint8_t* elementsData = shaderData + elementsStart + field18 * 4;
  for (uint32_t i = 0; i < vertexElementCount; ++i) {
    const uint32_t raw = ReadBigEndianU32(elementsData + i * 4);
    if (VertexElementUsage(raw) == kDeclUsageTexCoord) {
      semanticIndices[i] = uint8_t(VertexElementUsageIndex(raw));
    }
  }
  return semanticIndices;
}

// XenosRecomp's own cache-entry hash
// (XXH3_64bits over the WHOLE container, main.cpp's
// `XXH3_64bits(shaderContainer, dataSize)`) is NOT the same value the
// Vulkan pipeline cache looks shaders up by at runtime -- that's
// `ucode_data_hash()` (pipeline_cache.cpp's `LoadShader`), which hashes
// ONLY the ucode dwords the GPU command stream loads. Confirmed via direct
// empirical hash computation this session: these are two different,
// non-overlapping 64-bit values for every real captured shader checked.
// This mismatch meant the generalized native-pipeline cache
// (HasNativePipeline/GetOrCreatePipeline in renut_xenos_pipeline_cache.cpp)
// has never matched ANY batch-converted shader in this project's history --
// confirmed via zero real "native pipeline created" log lines ever, across
// every saved session log. Builds a remap table (container hash -> real
// ucode-only hash) from the real, whole container bytes
// tools/xenos_batch_convert.py writes to <output_remap> for every
// successfully-converted shader.
struct RemapResult {
  std::unordered_map<uint64_t, uint64_t> hashRemap;
  // keyed by the SAME real ucode hash hashRemap maps
  // to (the runtime lookup key), not the container hash -- so this can be
  // applied to outEntries using the entry's ALREADY-remapped .hash field
  // directly, no second lookup needed.
  std::unordered_map<uint64_t, std::array<uint8_t, kRenutMaxVertexLocations>> vertexLocations;
  // Same keying/lifetime contract as vertexLocations above.
  std::unordered_map<uint64_t, std::array<uint8_t, kRenutMaxVertexLocations>> vertexTexCoordSemanticIndex;
};

RemapResult BuildHashRemap(const char* remapPath) {
  RemapResult result;
  if (!remapPath) return result;

  std::ifstream file(remapPath, std::ios::binary);
  if (!file) {
    // Not fatal -- an older/no-remap-arg invocation should still work,
    // just without the fix (entries stay keyed by the old, wrong hash).
    std::fprintf(stderr, "xenos_cache_unpack: no remap file at %s, hash remap skipped\n", remapPath);
    return result;
  }

  while (true) {
    uint32_t lengthLE = 0;
    file.read(reinterpret_cast<char*>(&lengthLE), sizeof(lengthLE));
    if (file.gcount() != sizeof(lengthLE)) break;  // clean EOF

    std::vector<uint8_t> container(lengthLE);
    file.read(reinterpret_cast<char*>(container.data()), lengthLE);
    if (size_t(file.gcount()) != container.size()) {
      std::fprintf(stderr, "xenos_cache_unpack: truncated remap entry, stopping remap parse early\n");
      break;
    }
    if (container.size() < sizeof(ShaderContainerHeader)) {
      continue;  // real container is always >= header size; skip anything malformed
    }

    const uint32_t flags = ReadBigEndianU32(container.data() + offsetof(ShaderContainerHeader, flags));
    const uint32_t virtualSize = ReadBigEndianU32(container.data() + offsetof(ShaderContainerHeader, virtualSize));
    const uint32_t physicalSize = ReadBigEndianU32(container.data() + offsetof(ShaderContainerHeader, physicalSize));
    const uint32_t shaderOffset = ReadBigEndianU32(container.data() + offsetof(ShaderContainerHeader, shaderOffset));
    if (size_t(virtualSize) + size_t(physicalSize) != container.size() || physicalSize == 0) {
      std::fprintf(stderr, "xenos_cache_unpack: remap entry has inconsistent virtual/physical size, skipping\n");
      continue;
    }

    // Same formula as XenosRecomp's own main.cpp: XXH3_64bits over the
    // whole container (virtual+physical), matching what's already keying
    // the entry table this remap will be applied to.
    const uint64_t containerHash = XXH3_64bits(container.data(), container.size());
    // Same formula as pipeline_cache.cpp's real LoadShader/ucode_data_hash():
    // XXH3_64bits over ONLY the physical (ucode) section, which is appended
    // immediately after the virtual section in a real ShaderContainer.
    const uint64_t ucodeHash = XXH3_64bits(container.data() + virtualSize, physicalSize);

    result.hashRemap[containerHash] = ucodeHash;

    // flags & 0x1 CLEAR means pixel shader (same real convention
    // build_synthetic_container.py's build_pixel_container already uses,
    // confirmed against XenosRecomp's own shader_recompiler.cpp).
    const bool isVertexShader = (flags & 0x1) != 0;
    result.vertexLocations[ucodeHash] = ExtractVertexLocations(container, shaderOffset, isVertexShader);
    result.vertexTexCoordSemanticIndex[ucodeHash] =
        ExtractVertexTexCoordSemanticIndices(container, shaderOffset, isVertexShader);
  }

  return result;
}

std::string ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "xenos_cache_unpack: cannot open %s\n", path);
    std::exit(1);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

// Parses one "{ 0xHASH, dxilOff, dxilSize, spirvOff, spirvSize, specMask },"
// line, matching XenosRecomp main.cpp's exact fmt::println format string:
//   "\t{{ 0x{:X}, {}, {}, {}, {}, {} }},"
std::vector<Entry> ParseEntries(const std::string& source) {
  std::vector<Entry> entries;
  size_t pos = source.find("g_shaderCacheEntries[]");
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: g_shaderCacheEntries not found in input\n");
    std::exit(1);
  }
  size_t braceOpen = source.find('{', pos);
  size_t arrayEnd = source.find("};", braceOpen);
  std::string body = source.substr(braceOpen + 1, arrayEnd - braceOpen - 1);

  size_t i = 0;
  while (true) {
    size_t entryOpen = body.find('{', i);
    if (entryOpen == std::string::npos) break;
    size_t entryClose = body.find('}', entryOpen);
    std::string entryText = body.substr(entryOpen + 1, entryClose - entryOpen - 1);

    Entry e;
    // hash, dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask
    unsigned long long hash = 0, dxilOff = 0, dxilSize = 0, spirvOff = 0, spirvSize = 0, specMask = 0;
    if (std::sscanf(entryText.c_str(), " 0x%llX , %llu , %llu , %llu , %llu , %llu", &hash, &dxilOff,
                     &dxilSize, &spirvOff, &spirvSize, &specMask) != 6) {
      std::fprintf(stderr, "xenos_cache_unpack: failed to parse entry '%s'\n", entryText.c_str());
      std::exit(1);
    }
    e.hash = uint64_t(hash);
    e.spirvOffset = size_t(spirvOff);
    e.spirvSize = size_t(spirvSize);
    e.specConstantsMask = uint32_t(specMask);
    entries.push_back(e);

    i = entryClose + 1;
  }
  return entries;
}

// Parses "const uint8_t g_compressedSpirvCache[] = {n,n,n,...};" into raw
// bytes.
std::vector<uint8_t> ParseByteArray(const std::string& source, const char* varName) {
  size_t pos = source.find(varName);
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: %s not found in input\n", varName);
    std::exit(1);
  }
  size_t braceOpen = source.find('{', pos);
  size_t braceClose = source.find('}', braceOpen);
  std::string body = source.substr(braceOpen + 1, braceClose - braceOpen - 1);

  std::vector<uint8_t> bytes;
  bytes.reserve(body.size() / 3);
  size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && (body[i] == ',' || body[i] == ' ' || body[i] == '\n' || body[i] == '\t')) ++i;
    if (i >= body.size()) break;
    size_t start = i;
    while (i < body.size() && body[i] != ',') ++i;
    if (i > start) {
      bytes.push_back(uint8_t(std::strtoul(body.substr(start, i - start).c_str(), nullptr, 10)));
    }
  }
  return bytes;
}

size_t ParseSizeT(const std::string& source, const char* varName) {
  size_t pos = source.find(varName);
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: %s not found in input\n", varName);
    std::exit(1);
  }
  size_t eq = source.find('=', pos);
  size_t semi = source.find(';', eq);
  return size_t(std::strtoull(source.substr(eq + 1, semi - eq - 1).c_str(), nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <xenos-recomp-generated-cache.cpp> <output.cpp> [<remap-file>]\n",
                 argv[0]);
    return 1;
  }

  std::string source = ReadFile(argv[1]);
  std::vector<Entry> entries = ParseEntries(source);
  std::vector<uint8_t> compressed = ParseByteArray(source, "g_compressedSpirvCache");
  size_t decompressedSize = ParseSizeT(source, "g_spirvCacheDecompressedSize");

  // Real hash-mismatch fix -- see BuildHashRemap's own comment. Remap each
  // entry's key from XenosRecomp's own whole-container hash to the real
  // ucode-only hash the Vulkan pipeline cache actually looks shaders up by.
  // Entries with no remap match (shouldn't happen for anything that came
  // through xenos_batch_convert.py's isolation loop, since every successful
  // conversion is recorded) are left with their original hash and a warning
  // -- kept in the cache rather than silently dropped, since a stale/wrong
  // key is a real, visible failure mode (just never matches at runtime, same
  // as before this fix), not a crash risk.
  RemapResult remapResult = BuildHashRemap(argc > 3 ? argv[3] : nullptr);
  size_t remapped = 0;
  size_t withVertexLocations = 0;
  for (Entry& e : entries) {
    auto it = remapResult.hashRemap.find(e.hash);
    if (it != remapResult.hashRemap.end()) {
      e.hash = it->second;
      ++remapped;
    } else if (!remapResult.hashRemap.empty()) {
      std::fprintf(stderr,
                   "xenos_cache_unpack: WARNING no hash remap found for entry 0x%llX -- "
                   "keeping XenosRecomp's own container hash, this entry will not match "
                   "any real draw at runtime\n",
                   (unsigned long long)e.hash);
    }
    // keyed by e.hash AFTER remapping above, since
    // vertexLocations was built keyed by the same real ucode hash (see
    // BuildHashRemap/RemapResult's own comments).
    auto locIt = remapResult.vertexLocations.find(e.hash);
    if (locIt != remapResult.vertexLocations.end()) {
      e.vertexLocations = locIt->second;
      if (locIt->second[0] != kRenutVertexLocationUnset) ++withVertexLocations;
    }
    auto texCoordIt = remapResult.vertexTexCoordSemanticIndex.find(e.hash);
    if (texCoordIt != remapResult.vertexTexCoordSemanticIndex.end()) {
      e.vertexTexCoordSemanticIndex = texCoordIt->second;
    }
  }
  if (!remapResult.hashRemap.empty()) {
    std::fprintf(stderr, "xenos_cache_unpack: remapped %zu/%zu entries to their real ucode hash "
                 "(%zu with real vertex locations)\n",
                 remapped, entries.size(), withVertexLocations);
  }

  std::vector<uint8_t> smolvBlob(decompressedSize);
  size_t zstdResult = ZSTD_decompress(smolvBlob.data(), smolvBlob.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(zstdResult) || zstdResult != decompressedSize) {
    std::fprintf(stderr, "xenos_cache_unpack: ZSTD_decompress failed (%s)\n", ZSTD_getErrorName(zstdResult));
    return 1;
  }

  // Decode each shader's smol-v-encoded SPIR-V slice back to real SPIR-V
  // (uint32_t words), and lay them all out contiguously in one flat buffer.
  std::vector<uint32_t> flatSpirv;
  std::vector<Entry> outEntries;
  for (const Entry& e : entries) {
    if (e.spirvSize == 0 || e.spirvOffset + e.spirvSize > smolvBlob.size()) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX has an invalid SPIR-V slice, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }
    size_t decodedSpirvSize = smolv::GetDecodedBufferSize(smolvBlob.data() + e.spirvOffset, e.spirvSize);
    if (decodedSpirvSize == 0) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX smol-v size query failed, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }
    std::vector<uint32_t> spirv(decodedSpirvSize / sizeof(uint32_t));
    if (!smolv::Decode(smolvBlob.data() + e.spirvOffset, e.spirvSize, spirv.data(), decodedSpirvSize)) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX smol-v decode failed, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }

    Entry outEntry = e;
    outEntry.spirvOffset = flatSpirv.size();
    outEntry.spirvSize = spirv.size();
    outEntries.push_back(outEntry);
    flatSpirv.insert(flatSpirv.end(), spirv.begin(), spirv.end());
  }

  std::ofstream out(argv[2]);
  out << "// Generated by tools/xenos_cache_unpack.cpp from XenosRecomp's real compiled shader cache.\n";
  out << "// Plain, decompressed SPIR-V words -- no zstd/smol-v dependency needed to consume this.\n";
  out << "#include \"renut_engine/renut_xenos_shader_cache.h\"\n\n";
  out << "const uint32_t g_renutXenosSpirvWords[] = {\n";
  for (size_t i = 0; i < flatSpirv.size(); ++i) {
    out << flatSpirv[i] << ",";
    if ((i % 16) == 15) out << "\n";
  }
  out << "\n};\n\n";
  out << "const RenutXenosShaderCacheEntry g_renutXenosShaderCacheEntries[] = {\n";
  for (const Entry& e : outEntries) {
    out << "\t{ 0x" << std::hex << e.hash << std::dec << "ULL, " << e.spirvOffset << ", " << e.spirvSize
        << ", " << e.specConstantsMask << ", { ";
    for (size_t i = 0; i < kRenutMaxVertexLocations; ++i) {
      out << uint32_t(e.vertexLocations[i]) << (i + 1 < kRenutMaxVertexLocations ? ", " : "");
    }
    out << " }, { ";
    for (size_t i = 0; i < kRenutMaxVertexLocations; ++i) {
      out << uint32_t(e.vertexTexCoordSemanticIndex[i]) << (i + 1 < kRenutMaxVertexLocations ? ", " : "");
    }
    out << " } },\n";
  }
  out << "};\n\n";
  out << "const size_t g_renutXenosShaderCacheEntryCount = " << outEntries.size() << ";\n";

  std::fprintf(stderr, "xenos_cache_unpack: wrote %zu shaders (%zu SPIR-V words) to %s\n", outEntries.size(),
               flatSpirv.size(), argv[2]);
  return 0;
}
