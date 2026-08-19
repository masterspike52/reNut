#pragma once
// Real-data shape for the batch-converted native-renderer shader cache (see
// docs/ai/research.md "convert the important/common shaders"). Populated
// by generated/renut_xenos_shader_cache.cpp, produced at build time by
// tools/xenos_batch_convert.py + tools/xenos_cache_unpack.cpp from the
// user's real captured guest shader dump.
//
// spirvOffset/spirvSize index into g_renutXenosSpirvWords (uint32_t words,
// i.e. already real SPIR-V ready for vkCreateShaderModule -- ZSTD/smol-v
// decoding already happened at build time, not here).

#include <cstddef>
#include <cstdint>

// the compiled SPIR-V's vertex shader stage declares
// FIXED Vulkan input locations per D3D9 usage/usageIndex (XenosRecomp's own
// shader_recompiler.cpp: USAGE_LOCATIONS table, e.g. Position=0, Normal=1,
// Tangent=2, Binormal=3, TexCoord0=4, ...) -- NOT sequential 0,1,2,... in
// vfetch-instruction order. The runtime pipeline cache previously assigned
// locations purely sequentially (renut_xenos_pipeline_cache.cpp's
// next_location++), which happened to work only for the first, simplest
// proven shader (whose 2 attributes' real locations were coincidentally
// sequential) -- for real shaders with a location gap (e.g. Normal=1 then
// TexCoord0=4), this produced a real Vulkan validation error
// ("pVertexAttributeDescriptions does not have a Location 4, but
// [VK_SHADER_STAGE_VERTEX_BIT] has [Input variable, Location 4] at that
// Location") and, even when it didn't hard-fail, silently bound the wrong
// data to the wrong shader input (confirmed real cause of visible
// stretched-texture/wrong-lighting corruption). vertexLocations[i] is the
// REAL Vulkan location for the i-th attribute in vertex_bindings() iteration
// order (same order tools/ucode_analyze.cpp's JSON output uses, since both
// walk the same real Shader::AnalyzeUcode()/vertex_bindings() data) --
// computed at build time (tools/xenos_cache_unpack.cpp's
// ExtractVertexLocations) using the SAME assignment rule XenosRecomp itself
// used to compile the shader, not re-derived or guessed at runtime.
// kVertexLocationUnset marks unused slots. Pixel shaders (which have no
// vertex input stage) leave this fully unset.
//
// raised 16 -> 32, and the assignment rule changed
// from "look the usage up in XenosRecomp's USAGE_LOCATIONS table" to "the
// i-th vertex element gets location i". The old comment's claim that no
// captured shader had more than ~9 attributes was measured on too small a
// sample: re-checked across every captured shader, the real maximum is 28
// (one binding with 12 attributes plus another with 7 is typical for the
// skinned/instanced geometry here), and 198 shaders were being rejected
// outright partly because of the 16 ceiling. 32 matches both the
// maxVertexInputAttributes this GPU reports and XenosRecomp's own
// maxVertexInputAttributes cap, so the two sides cannot disagree.
//
// NOTE: 32 exceeds Vulkan's GUARANTEED minimum of 16 for
// maxVertexInputAttributes. That is deliberate and safe here because the
// runtime rejects any shader whose attribute count exceeds the device's real
// reported limit before creating a pipeline, falling back to the stock
// renderer rather than failing pipeline creation.
inline constexpr uint32_t kRenutMaxVertexLocations = 32;
inline constexpr uint8_t kRenutVertexLocationUnset = 0xFF;

// vertexLocations[] is now purely positional (see its
// own header comment above), so it can no longer double as "this attribute's
// D3D9 TexCoord usage index" the way the old usage-based location scheme
// let the runtime infer for free. XenosRecomp's own compiled HLSL still
// bakes the real usageIndex as a literal into tfetchTexcoord()'s call site
// at compile time (shader_recompiler.cpp's `recompile(VertexFetchInstruction)`),
// so THAT part needs no fix -- but renut_xenos_pipeline_cache.cpp separately
// needs the real usageIndex at pipeline-build time too, to compute
// swapped_texcoords_mask (which real TexCoord semantic bit to set based on
// the ACTUAL bound vertex format being 16-bit-per-channel). That data has
// no other source at runtime (ParsedVertexFetchInstruction carries no D3D9
// usage info at all -- see shader.h's VertexBinding::Attribute), so it's
// captured here at build time the same way vertexLocations[] is.
// kRenutTexCoordSemanticUnset marks a raw attribute that either isn't a
// TexCoord usage at all, or whose data is unavailable (pixel shaders, or a
// shader with no remap match).
inline constexpr uint8_t kRenutTexCoordSemanticUnset = 0xFF;

struct RenutXenosShaderCacheEntry {
	uint64_t hash;
	size_t spirvOffset;
	size_t spirvSize;
	uint32_t specConstantsMask;
	uint8_t vertexLocations[kRenutMaxVertexLocations];
	uint8_t vertexTexCoordSemanticIndex[kRenutMaxVertexLocations];
};

extern const uint32_t g_renutXenosSpirvWords[];
extern const RenutXenosShaderCacheEntry g_renutXenosShaderCacheEntries[];
extern const size_t g_renutXenosShaderCacheEntryCount;
