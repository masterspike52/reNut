#include <rex/ppc.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/runtime.h>
#include "renut_logging.h"
#include "renut_engine/nativevk_phase0.h"
#include "renut_engine/shader_dump.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#endif

REXCVAR_DEFINE_BOOL(dump_guest_shaders, false, "Nuts&Bolts/Graphics",
	"Dump guest shader blobs to a 'shaders' folder next to the exe as they are "
	"created. Writes <vs|ps>_<hash>.bin (the whole pFunction) and "
	"<vs|ps>_<hash>.ucode (microcode only). Each unique shader is written once.");

namespace {

constexpr uint32_t kMaxBlobBytes = 1u << 20;
constexpr uint32_t kMinHeaderBytes = 12;

std::mutex g_dumpMutex;
std::unordered_set<uint64_t> g_dumped;

// Real ShaderContainer layout (see XenosRecomp/XenosRecomp/shader.h's
// ShaderContainer/Shader structs, confirmed via direct source read):
//   +0x00 flags, +0x04 virtualSize, +0x08 physicalSize, +0x0C fieldC,
//   +0x10 constantTableOffset, +0x14 definitionTableOffset,
//   +0x18 shaderOffset, +0x1C field1C, +0x20 field20  (36 bytes)
// then, at container+shaderOffset, a Shader struct:
//   +0x00 physicalOffset, +0x04 size, +0x08 field8, +0x0C fieldC,
//   +0x10 field10, +0x14 interpolatorInfo  (24 bytes)
//
// Real bug fixed here: words[1] ("headerSize") is actually
// ShaderContainer::virtualSize, NOT a microcode start offset -- confirmed
// via direct hex inspection of captured containers plus XenosRecomp's own
// shader_recompiler.cpp:1559 (`shaderData + shaderContainer->virtualSize +
// shader->physicalOffset`). Xenos microcode instructions actually start
// virtualSize + shader->physicalOffset bytes into the container (past the
// embedded constant table's literal float/bool/loop constant blocks, which
// physicalOffset skips over -- confirmed real, non-zero physicalOffset
// values of 48/64 bytes on real captured shaders, exactly matching a
// stray leading block of float-looking bytes this project's own ucode_analyze
// tool was silently misparsing as garbage instructions). words[2]
// ("ucodeSize") is ShaderContainer::physicalSize, which similarly
// overcounts by physicalOffset bytes -- the real microcode length is
// Shader::size, read from the Shader struct itself, not physicalSize.
constexpr uint32_t kShaderStructOffsetPhysicalOffset = 0;
constexpr uint32_t kShaderStructOffsetSize = 4;
constexpr uint32_t kShaderStructBytes = 24;

// Resolves the real microcode (ucode) byte range within a captured
// ShaderContainer blob. Returns false if the container's shaderOffset (or
// anything it points at) doesn't fit within the blob's own reported size,
// in which case the caller should treat the blob as unparseable (matches
// this file's existing "implausible blob" skip behavior).
bool ResolveRealUcodeRange(const uint8_t* blob, uint32_t virtualSize, uint32_t physicalSize,
                           const uint8_t*& ucodeOut, uint32_t& ucodeSizeOut)
{
	const uint32_t* words = reinterpret_cast<const uint32_t*>(blob);
	const uint32_t shaderOffset = RENUT_BSWAP32(words[6]);  // ShaderContainer::shaderOffset, +0x18
	if (shaderOffset < 36 || uint64_t(shaderOffset) + kShaderStructBytes > uint64_t(virtualSize)) {
		return false;
	}
	const uint8_t* shaderStruct = blob + shaderOffset;
	const uint32_t physicalOffset =
		RENUT_BSWAP32(*reinterpret_cast<const uint32_t*>(shaderStruct + kShaderStructOffsetPhysicalOffset));
	const uint32_t size =
		RENUT_BSWAP32(*reinterpret_cast<const uint32_t*>(shaderStruct + kShaderStructOffsetSize));
	if (size == 0 || uint64_t(physicalOffset) + size > uint64_t(physicalSize)) {
		return false;
	}
	ucodeOut = blob + virtualSize + physicalOffset;
	ucodeSizeOut = size;
	return true;
}

// factored out of dumpVertexDeclUsage so it can be
// called from dumpVertexShaderCreate_hook -- see g_shaderHandleToUcodeHash's
// own comment for why. Computes the SAME ucode-only hash
// (post-ResolveRealUcodeRange) that must match the real Vulkan pipeline's
// ucode_data_hash(), from a raw pFunction pointer. Returns false if the
// blob doesn't parse as plausible.
bool computeShaderUcodeHash(uint32_t pFunction, uint64_t& outHash)
{
	if (!pFunction) return false;

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	const uint8_t* blob = base + pFunction;
	const uint32_t* words = reinterpret_cast<const uint32_t*>(blob);
	const uint32_t headerSize = RENUT_BSWAP32(words[1]);
	const uint32_t ucodeSize = RENUT_BSWAP32(words[2]);
	if (headerSize < kMinHeaderBytes || headerSize > kMaxBlobBytes ||
		ucodeSize == 0 || ucodeSize > kMaxBlobBytes) {
		return false;
	}

	const uint8_t* ucode = blob + headerSize;
	uint32_t realUcodeSize = ucodeSize;
	if (const uint8_t* realUcode; ResolveRealUcodeRange(blob, headerSize, ucodeSize, realUcode, realUcodeSize)) {
		ucode = realUcode;
	} else {
		realUcodeSize = ucodeSize;
	}
	outHash = XXH3_64bits(ucode, realUcodeSize);
	return true;
}

// A real, parsed Xbox 360 D3DVERTEXELEMENT9-equivalent entry (12 bytes on
// this platform, not PC D3D9's 8 -- see VertexDeclElement's own comment at
// its parse site for the real struct layout, confirmed via UnleashedRecomp's
// GuestVertexElement).
struct VertexDeclElement { uint16_t offset; uint32_t type; uint8_t usage; uint8_t usageIndex; };

constexpr uint32_t kVertexDeclElementStride = 12;
constexpr uint32_t kMaxVertexDeclElements = 32;  // scan cap, real decls are far smaller
constexpr uint32_t kMaxPlausibleVertexDeclElements = 16;
constexpr uint8_t kMaxPlausibleVertexDeclUsage = 13;  // D3DDECLUSAGE_SAMPLE, the highest real value

// MUST be called immediately when pElements is known
// fresh (at CreateVertexDeclaration time) -- see g_pendingDeclElements's own
// comment for why deferring this read to SetVertexDeclaration time was
// fatally broken (the array is frequently caller-stack-allocated and long
// overwritten by the time SetVertexDeclaration fires, sometimes seconds/many
// calls later). Returns an empty vector if the array doesn't parse as
// plausible (either structurally -- no terminator found within the scan cap
// -- or semantically -- an out-of-range usage byte, a strong signal we're
// reading unrelated memory rather than a real declaration).
std::vector<VertexDeclElement> parsePlausibleVertexDecl(uint32_t pElements)
{
	if (pElements < 0x10000000u || pElements >= 0x90000000u) return {};

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	std::vector<VertexDeclElement> elements;
	bool foundTerminator = false;
	for (uint32_t count = 0; count < kMaxVertexDeclElements; ++count) {
		const uint8_t* e = base + pElements + count * kVertexDeclElementStride;
		const uint16_t streamField = (uint16_t(e[0]) << 8) | e[1];
		if (streamField == 0x00FFu || (streamField & 0xFF) == 0xFFu) {
			foundTerminator = true;
			break;
		}
		elements.push_back({
			uint16_t((uint16_t(e[2]) << 8) | e[3]),
			// Real struct layout (confirmed via UnleashedRecomp's
			// GuestVertexElement, same XDK): stream:u16@0, offset:u16@2,
			// type:u32@4 (a packed Xenos hardware fetch-format code, e.g.
			// FLOAT3 = 0x2A23B9 -- NOT a small enum), method:u8@8,
			// usage:u8@9, usageIndex:u8@10, pad:u8@11.
			(uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7],
			e[9],
			e[10],
		});
	}

	// Real safety fix: the consumer (build_synthetic_container.py)
	// treats any captured file as ground truth, overriding its own heuristic
	// fallback -- writing a garbage capture is WORSE than writing nothing,
	// it would poison a shader the heuristic might have gotten right. Reject
	// anything that doesn't look like a real declaration rather than trust
	// it blindly: no terminator found within the scan cap, too many
	// elements, or any out-of-range usage byte (valid D3DDECLUSAGE is 0-13).
	if (!foundTerminator || elements.empty() || elements.size() > kMaxPlausibleVertexDeclElements) {
		return {};
	}
	for (const VertexDeclElement& e : elements) {
		if (e.usage > kMaxPlausibleVertexDeclUsage) return {};
	}
	return elements;
}

bool writeFile(const std::filesystem::path& path, const uint8_t* data, size_t size)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		RNUT_ERROR("shader dump: could not open {}", path.string());
		return false;
	}
	file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	if (!file) {
		RNUT_ERROR("shader dump: write failed for {}", path.string());
		return false;
	}
	return true;
}

void dumpShaderBlob(PPCRegister& r3, const char* tag)
{
	if (!REXCVAR_GET(dump_guest_shaders)) return;

	const uint32_t pFunction = r3.u32;
	if (!pFunction) return;

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	const uint8_t* blob = base + pFunction;
	const uint32_t* words = reinterpret_cast<const uint32_t*>(blob);

	const uint32_t flags = RENUT_BSWAP32(words[0]);
	const uint32_t headerSize = RENUT_BSWAP32(words[1]);
	const uint32_t ucodeSize = RENUT_BSWAP32(words[2]);

	if (headerSize < kMinHeaderBytes || headerSize > kMaxBlobBytes ||
		ucodeSize == 0 || ucodeSize > kMaxBlobBytes) {
		RNUT_WARN("shader dump: implausible {} blob at {:#x} (header {}, ucode {}) - skipped",
			tag, pFunction, headerSize, ucodeSize);
		return;
	}

	const size_t total = static_cast<size_t>(headerSize) + ucodeSize;

	// Real microcode range (see ResolveRealUcodeRange's header comment):
	// headerSize/ucodeSize (virtualSize/physicalSize) mark
	// the whole container's virtual+physical regions, not where real Xenos
	// instructions start/how long they run -- that needs shaderOffset ->
	// Shader::physicalOffset/size. Falls back to the (known-wrong, but
	// previously the only available) virtualSize-relative slice if the
	// container doesn't parse as expected, so a genuinely malformed/unusual
	// blob still produces SOME .ucode file rather than none.
	const uint8_t* ucode = blob + headerSize;
	uint32_t realUcodeSize = ucodeSize;
	if (const uint8_t* realUcode; ResolveRealUcodeRange(blob, headerSize, ucodeSize, realUcode, realUcodeSize)) {
		ucode = realUcode;
	} else {
		realUcodeSize = ucodeSize;
	}

	// Real bug found+fixed: this used to hash the whole blob
	// (header+ucode, XXH3_64bits(blob, total)), NOT the same value as
	// computeShaderUcodeHash() above or the real runtime
	// Shader::ucode_data_hash() (pipeline_cache.cpp:
	// XXH3_64bits(host_address, dword_count*4), ucode-only). Confirmed via
	// direct measurement: with the old formula, 0/163 distinct vertex
	// shaders drawn in an extended real play session ever matched anything
	// in the native shader cache (pixel shaders, unaffected by this specific
	// bug, matched ~96%) -- every file this function ever wrote under
	// shaders/vs_*.bin was named/keyed by a hash nothing downstream (the
	// native pipeline cache's runtime lookup) could ever produce again.
	// Existing captures in shaders/ from before this fix are permanently
	// mis-hashed and need a fresh capture session to benefit.
	const uint64_t hash = XXH3_64bits(ucode, realUcodeSize);
	{
		std::lock_guard<std::mutex> lock(g_dumpMutex);
		if (!g_dumped.insert(hash).second) return;
	}

	const std::filesystem::path outDir =
		rex::filesystem::GetExecutableFolder() / "shaders";

	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) {
		RNUT_ERROR("shader dump: cannot create {} ({})", outDir.string(), ec.message());
		return;
	}

	char stem[32];
	std::snprintf(stem, sizeof(stem), "%s_%016llx", tag,
		static_cast<unsigned long long>(hash));

	if (!writeFile(outDir / (std::string(stem) + ".bin"), blob, total)) return;
	if (!writeFile(outDir / (std::string(stem) + ".ucode"), ucode, realUcodeSize)) return;

	RNUT_INFO("shader dump: {} (flags {:#x}, header {} B, ucode {} B) from {:#x}",
		stem, flags, headerSize, ucodeSize, pFunction);
}

}  // namespace

void dumpCodeBytesOnce();

void dumpPixelShader_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "ps");
}

void dumpVertexShaderSet_hook(PPCRegister& r3, PPCRegister& r4);
void dumpVertexDeclarationSet_hook(PPCRegister& r3, PPCRegister& r4);
void dumpVertexDeclarationCreated_hook(PPCRegister& r3);
void dumpVertexShaderCreate_hook(PPCRegister& r3);
void dumpVertexShaderCreated_hook(PPCRegister& r3);

namespace {

REXCVAR_DEFINE_BOOL(renut_dump_vertex_decl, false, "Nuts&Bolts/Graphics",
	"Dump real D3DVERTEXELEMENT9 arrays passed to CreateVertexDeclaration, for "
	"native-renderer Phase 1 shader-container reconstruction.");

std::mutex g_declMutex;
std::unordered_set<uint32_t> g_declLogged;

// sub_8264EA90 (the guest function this project's
// "CreateVertexDeclaration" hook sits at the true entry of, confirmed via
// direct recompiled-PPC-code inspection) allocates a NEW handle object and
// returns it in r3 -- SetVertexDeclaration (0x82205700) later receives that
// returned HANDLE, not the original D3DVERTEXELEMENT9[] array pointer this
// hook captures. Treating the handle as if it WERE the elements pointer
// (the previous behavior) meant SetVertexDeclaration's dump path walked
// garbage/handle-object memory as if it were a raw element array -- the
// real cause of "vertdecl DIAG: bail pElements range" firing on effectively
// every call. Fix: capture the true elements pointer here, capture the
// returned handle at the function's real return point (0x8264EAFC, a
// second, separate hook -- see dumpVertexDeclarationCreated_hook), and
// build a handle -> elements-pointer map so SetVertexDeclaration can look
// up the real array by the handle it actually receives.
//
// This game creates vertex declarations from MULTIPLE THREADS concurrently
// (confirmed -- captured log shows CreateVertexDeclaration hooks firing
// from several distinct thread IDs). A single shared
// g_pendingDeclElements meant thread A's entry-hook value could be
// overwritten by thread B's own Create call before thread A's OWN exit hook
// read it back, silently pairing thread A's handle with thread B's elements
// pointer -- exactly matching what the real captured data showed: most
// captures had wildly implausible offsets/types (reading whatever unrelated
// array thread B had created), while occasional ones came out clean.
// thread_local makes each thread's entry->exit pairing immune to any other
// thread's concurrent Create calls (safe because a single thread cannot
// reenter CreateVertexDeclaration on itself before its own call returns).
//
// Even with thread_local, captures STILL came back corrupted -- proof:
// the exact same decl address
// (0x7018ef00) parsed to a DIFFERENT element count at different points in
// the same session, which is impossible for real static data. Root cause:
// pElements is very often a CALLER-STACK-ALLOCATED array (a local variable
// in whatever guest function builds the declaration), valid only for the
// duration of the CreateVertexDeclaration call itself -- by the time
// SetVertexDeclaration fires (which can be far later), that stack memory
// has been overwritten by unrelated calls. Storing just the raw pointer and
// re-reading it later was fundamentally broken for stack-sourced arrays;
// only the ONE declaration living in the game's static data segment
// (0x82xxxxxx) ever came back clean. Fix: parse the array's CONTENT
// immediately here, while guaranteed fresh, and store the parsed data
// itself (not a pointer to re-read later).
thread_local std::vector<VertexDeclElement> g_pendingDeclElements;
std::unordered_map<uint32_t, std::vector<VertexDeclElement>> g_declHandleToElements;

std::mutex g_vsMutex;
uint64_t g_lastBoundVertexShaderUcodeHash = 0;
// SetVertexDeclaration and SetVertexShader do NOT
// have a fixed call order -- confirmed via real captured log, this game
// calls SetVertexDeclaration BEFORE SetVertexShader, so tracking only "the
// last bound vertex shader" and dumping from the declaration hook alone
// meant dumpVertexDeclUsageForBoundShader() always ran with no shader
// bound yet, silently hitting its early-return every single time (the
// real cause of shaders_vertdecl/ being completely empty, independent of
// the separate ucode-offset bug fixed elsewhere in this file). Track the
// last bound declaration too, and dump from BOTH hooks -- whichever one
// fires second for a given pair has both pieces of state available.
std::vector<VertexDeclElement> g_lastBoundVertexDeclElements;
std::unordered_set<uint64_t> g_vertDeclDumped;

// the SAME handle-vs-real-pointer confusion as
// g_declHandleToElements above, but for vertex shaders. sub_8264E8B0 (the
// guest function dumpVertexShaderCreate_hook already sits at the true entry
// of) ALSO allocates a new handle object and returns it in r3 -- confirmed via the same
// recompiled-code inspection technique, its two return paths (success at
// loc_8264E96C, failure at loc_8264E8E4) converge at loc_8264E998 with r3
// already holding the final handle (or 0). SetVertexShader (0x8222A0A8)
// receives that handle in r4, not the raw shader-blob pointer this hook's
// own entry capture sees -- so dumpVertexDeclUsage's pFunction argument was
// always the handle, explaining the "implausible blob" bails once pElements
// started resolving correctly.
//
// thread_local for the same real, confirmed reason as g_pendingDeclElements
// above (multi-threaded CreateVertexShader calls cross-contaminating a
// shared pending value).
//
// Same stack-staleness problem as g_pendingDeclElements -- storing the
// raw pFunction pointer for later
// re-reading was unsafe if it's ever stack-sourced. Store the already-
// computed ucode hash instead (0 = invalid/not yet known), computed
// immediately at CreateVertexShader time via computeShaderUcodeHash().
thread_local uint64_t g_pendingShaderUcodeHash = 0;
std::unordered_map<uint32_t, uint64_t> g_shaderHandleToUcodeHash;

}  // namespace

void dumpVertexDeclaration_hook(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
	PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10)
{
	(void)r4; (void)r5; (void)r6; (void)r7; (void)r8; (void)r9; (void)r10;
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	dumpCodeBytesOnce();

	const uint32_t pElements = r3.u32;
	if (pElements < 0x10000000u || pElements >= 0x90000000u) {
		return;
	}

	// Parse immediately -- see g_pendingDeclElements's own comment for
	// why deferring this read is
	// fatally broken for (very common) stack-sourced element arrays.
	std::vector<VertexDeclElement> elements = parsePlausibleVertexDecl(pElements);

	bool alreadyLogged;
	{
		std::lock_guard<std::mutex> lock(g_declMutex);
		g_pendingDeclElements = elements;
		alreadyLogged = !g_declLogged.insert(pElements).second;
	}
	if (alreadyLogged || elements.empty()) return;

	std::string line = "renut vertex decl:";
	for (const VertexDeclElement& e : elements) {
		char entry[64];
		std::snprintf(entry, sizeof(entry), " off=%u type=%06x usage=%u usageIdx=%u",
			e.offset, e.type, e.usage, e.usageIndex);
		line += entry;
	}

	RNUT_INFO("{} (from {:#x}, {} elements)", line, pElements, elements.size());
}

// Fires at guest address 0x8264EAFC, sub_8264EA90's real return point
// (confirmed via recompiled-code inspection: r3 there already holds either
// the freshly-allocated handle or 0 on allocation failure -- both paths
// converge on this label before the epilogue runs). Pairs the elements
// pointer captured at function entry (dumpVertexDeclaration_hook, above)
// with the handle this function is about to return, so
// dumpVertexDeclarationSet_hook can later resolve the real elements array
// from the handle SetVertexDeclaration actually receives.
void dumpVertexDeclarationCreated_hook(PPCRegister& r3)
{
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;

	const uint32_t handle = r3.u32;
	if (handle < 0x10000000u || handle >= 0x90000000u) return;

	std::lock_guard<std::mutex> lock(g_declMutex);
	if (!g_pendingDeclElements.empty()) {
		g_declHandleToElements[handle] = std::move(g_pendingDeclElements);
		g_pendingDeclElements.clear();
	}
}

// Fires at sub_8264E8B0's true entry (0x8264E8B0). Two independent capture
// features share this one address (codegen only keeps the last-defined
// midasm_hook for a given (address, after_instruction) pair, so they can't
// be separate hook entries -- see the collision this session already found
// and fixed elsewhere in config/renut_hooks.toml):
//  1. dump_guest_shaders: writes the raw shader blob to shaders/vs_*.bin,
//     gated independently inside dumpShaderBlob() itself.
//  2. renut_dump_vertex_decl: captures the shader's ucode hash IMMEDIATELY
//     (see g_pendingShaderUcodeHash's own comment for why deferring this to
//     later is unsafe) for later pairing with the handle this function is
//     about to allocate and return.
// The dump_guest_shaders gate is independent of the ucode-hash capture
// below it. Real, deliberate change (see
// docs/ai/archive/native-renderer-rewrite-plan.md Phase 0): the ucode-hash
// capture used to early-return unless renut_dump_vertex_decl was enabled,
// same as the rest of this file's dump-to-disk paths. But this function
// (and dumpVertexShaderCreated_hook below) only computes a hash and inserts
// into a map -- no I/O -- and renut::shader_dump::TryResolveShaderUcodeHash
// (used by nativevk_phase0.cpp to measure real IM_LOAD-bypass rate) needs
// g_shaderHandleToUcodeHash populated unconditionally, not only when a user
// happens to have the unrelated vertex-decl-dump feature enabled.
void dumpVertexShaderCreate_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "vs");
	dumpCodeBytesOnce();

	uint64_t ucodeHash = 0;
	const bool parsed = computeShaderUcodeHash(r3.u32, ucodeHash);
	renut::nativevk_phase0::RecordCreateVertexShaderAttempt(parsed);
	if (!parsed) return;

	std::lock_guard<std::mutex> lock(g_vsMutex);
	g_pendingShaderUcodeHash = ucodeHash;
}

// Fires at sub_8264E8B0's real return point (0x8264E998, confirmed via
// recompiled-code inspection the same way as dumpVertexDeclarationCreated_hook
// -- both the success path (loc_8264E96C) and failure path (loc_8264E8E4)
// converge here with r3 already holding the final handle, or 0). Pairs it
// with the ucode hash computed at entry above.
void dumpVertexShaderCreated_hook(PPCRegister& r3)
{
	const uint32_t handle = r3.u32;
	if (handle < 0x10000000u || handle >= 0x90000000u) return;

	std::lock_guard<std::mutex> lock(g_vsMutex);
	if (g_pendingShaderUcodeHash != 0) {
		g_shaderHandleToUcodeHash[handle] = g_pendingShaderUcodeHash;
		g_pendingShaderUcodeHash = 0;
	}
}

// this is called from SetVertexShader AND
// SetVertexDeclaration, both genuinely hot per-draw paths -- unconditionally
// logging every bail meant ~70,000 lines/second in real play, which filled
// and rotated past the log's whole retention window (21 files x 5MB) in
// under 15 SECONDS. That silently destroyed the one-time startup-phase
// evidence (real CreateVertexDeclaration captures) this capture pipeline
// most needs to debug, and produced logs too foot-gunned to read. Each bail
// reason is now logged only the first kMaxBailLogs times (still enough to
// confirm the pattern) rather than forever.
namespace {
constexpr uint32_t kMaxBailLogs = 20;
std::atomic<uint32_t> g_bailNoElementsLogged{0};
std::atomic<uint32_t> g_bailNoHashLogged{0};
}  // namespace

// Takes ALREADY-PARSED, already-validated data -- no more re-reading
// pElements/pFunction memory here, since both are
// frequently stack-sourced and long stale by the time this runs (see
// g_pendingDeclElements's and g_pendingShaderUcodeHash's own comments).
void dumpVertexDeclUsage(const std::vector<VertexDeclElement>& elements, uint64_t ucodeHash)
{
	if (elements.empty()) {
		if (g_bailNoElementsLogged.fetch_add(1, std::memory_order_relaxed) < kMaxBailLogs) {
			RNUT_INFO("renut vertdecl DIAG: bail no elements resolved");
		}
		return;
	}
	if (ucodeHash == 0) {
		if (g_bailNoHashLogged.fetch_add(1, std::memory_order_relaxed) < kMaxBailLogs) {
			RNUT_INFO("renut vertdecl DIAG: bail no ucode hash resolved");
		}
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_declMutex);
		if (!g_vertDeclDumped.insert(ucodeHash).second) {
			return;
		}
	}

	const std::filesystem::path outDir =
		rex::filesystem::GetExecutableFolder() / "shaders_vertdecl";
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) {
		RNUT_INFO("renut vertdecl DIAG: bail create_directories failed ({})", ec.message());
		return;
	}

	char stem[48];
	std::snprintf(stem, sizeof(stem), "vs_%016llx", static_cast<unsigned long long>(ucodeHash));
	std::ofstream file(outDir / (std::string(stem) + ".vertdecl.txt"), std::ios::trunc);
	if (!file) {
		RNUT_INFO("renut vertdecl DIAG: bail ofstream failed for vs_{:016x}", ucodeHash);
		return;
	}

	for (const VertexDeclElement& e : elements) {
		file << "offset=" << e.offset << " type=" << std::hex << e.type << std::dec
			<< " usage=" << int(e.usage) << " usageIndex=" << int(e.usageIndex) << "\n";
	}

	RNUT_INFO("vertex decl usage: vs_{:016x} ({} elements)", ucodeHash, elements.size());
}

// TEMP DIAGNOSTIC: writes a real hex-dump of a D3D9 object's
// first 64 bytes straight to a file (immune to the log's 5MB rotation,
// which was confirmed real -- the once-per-process log line this used to
// be next to never survived to be read back, rotated out before the
// session ended even though the hook body itself demonstrably ran, per
// the "vertdecl DIAG: bail pElements range 0x0" lines proving
// dumpVertexShaderSet_hook executes every call). One file per (tag,
// object address) so repeated binds of the same object don't spam.
void dumpObjectHex(const char* tag, uint32_t pObject)
{
	if (pObject < 0x10000000u || pObject >= 0x90000000u) return;
	const std::filesystem::path outDir = rex::filesystem::GetExecutableFolder() / "shaders_objdump";
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) return;
	char stem[64];
	std::snprintf(stem, sizeof(stem), "%s_%08x.txt", tag, pObject);
	const std::filesystem::path path = outDir / stem;
	if (std::filesystem::exists(path)) return;

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	const uint32_t* obj = reinterpret_cast<const uint32_t*>(base + pObject);
	std::ofstream file(path, std::ios::trunc);
	if (!file) return;
	// Real captured blob pointers (shader dump: vs_... "from 0x43785820"
	// style log lines) all land in 0x43000000-0x44ffffff with a real
	// ShaderContainer flags dword (0x102a11xx) at their own +0 -- widened
	// to 128 bytes and each field that looks like a plausible pointer gets
	// ONE extra dereference tried and reported too, in case the real blob
	// pointer needs indirection through this object rather than being a
	// direct field.
	for (int i = 0; i < 32; ++i) {
		uint32_t value = RENUT_BSWAP32(obj[i]);
		file << "[+" << std::hex << (i * 4) << "]=" << value;
		if (value >= 0x40000000u && value < 0x50000000u) {
			uint32_t deref = RENUT_BSWAP32(*reinterpret_cast<const uint32_t*>(base + value));
			file << " -> [0]=" << deref;
		}
		file << std::dec << "\n";
	}
}

namespace {

// Resolve a SetVertexDeclaration handle (r4 at 0x82205700) back to the
// already-parsed elements captured at CreateVertexDeclaration time (see
// g_declHandleToElements's own comment). Returns an empty vector if the
// handle was never seen created (e.g. renut_dump_vertex_decl was enabled
// only after this declaration was already created) or its capture didn't
// pass plausibility validation.
std::vector<VertexDeclElement> resolveDeclElements(uint32_t handle)
{
	std::lock_guard<std::mutex> lock(g_declMutex);
	auto it = g_declHandleToElements.find(handle);
	return it != g_declHandleToElements.end() ? it->second : std::vector<VertexDeclElement>{};
}

// r4 at SetVertexShader (0x8222A0A8) is the HANDLE sub_8264E8B0 returned,
// not the real shader-blob pointer -- see g_shaderHandleToUcodeHash's own
// comment.
uint64_t resolveShaderUcodeHash(uint32_t handle)
{
	std::lock_guard<std::mutex> lock(g_vsMutex);
	auto it = g_shaderHandleToUcodeHash.find(handle);
	return it != g_shaderHandleToUcodeHash.end() ? it->second : 0;
}

}  // namespace

namespace renut::shader_dump {

bool TryResolveShaderUcodeHash(uint32_t handle, uint64_t& outHash)
{
	const uint64_t hash = resolveShaderUcodeHash(handle);
	if (hash != 0) {
		outHash = hash;
		return true;
	}

	uint64_t blobHash = 0;
	if (computeShaderUcodeHash(handle, blobHash)) {
		outHash = blobHash;
		renut::nativevk_phase0::RecordUnresolvedHandleParsedAsBlob();
		return true;
	}

	dumpObjectHex("unresolved_sv", handle);
	return false;
}

}  // namespace renut::shader_dump

// Real fix (2026-08-19): this is the ONLY midasm_hook at SetVertexShader's
// guest address (0x8222A0A8) -- nativevk_phase0.cpp used to register its own
// separate phase0SetVertexShader_hook here too, but confirmed via a full
// config/renut_hooks.toml sweep that codegen only keeps the LAST-defined
// midasm_hook when two entries share the same (address, after_instruction)
// pair (the appMainDrawStart/appMainDrawend "before/after" pattern only
// works because after_instruction genuinely differs there). That silently
// dropped THIS hook entirely -- shader-declaration capture was dead the
// whole time phase0SetVertexShader_hook existed. Fixed by merging: this one
// hook now also drives Phase 0's tracking, unconditionally (before the
// dump_vertex_decl-gated code below), instead of a second midasm_hook entry.
void dumpVertexShaderSet_hook(PPCRegister& r3, PPCRegister& r4)
{
	renut::nativevk_phase0::RecordSetVertexShader(r4.u32);

	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	static std::once_flag entered;
	std::call_once(entered, [&] {
		RNUT_INFO("renut SetVertexShader hook ENTERED: r3={:#x} r4={:#x}", r3.u32, r4.u32);
	});
	dumpObjectHex("vs_obj", r4.u32);
	dumpCodeBytesOnce();
	const uint64_t ucodeHash = resolveShaderUcodeHash(r4.u32);
	std::vector<VertexDeclElement> lastDecl;
	{
		std::lock_guard<std::mutex> lock(g_vsMutex);
		g_lastBoundVertexShaderUcodeHash = ucodeHash;
		lastDecl = g_lastBoundVertexDeclElements;
	}
	// See g_lastBoundVertexDeclElements's own comment: no fixed call order
	// between this hook and SetVertexDeclaration, so
	// also try the dump here using whatever declaration was bound most
	// recently -- covers the (confirmed real) case where the declaration
	// is set first.
	dumpVertexDeclUsage(lastDecl, ucodeHash);
}

void dumpVertexDeclarationSet_hook(PPCRegister& r3, PPCRegister& r4)
{
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	static std::once_flag entered;
	std::call_once(entered, [&] {
		RNUT_INFO("renut SetVertexDeclaration hook ENTERED: r3={:#x} r4={:#x}", r3.u32, r4.u32);
	});
	dumpObjectHex("decl_obj", r4.u32);
	dumpCodeBytesOnce();
	// r4 is the HANDLE sub_8264EA90 returned, not the real elements array
	// pointer (see g_declHandleToElements's own comment) -- resolve it.
	std::vector<VertexDeclElement> realElements = resolveDeclElements(r4.u32);
	uint64_t lastShaderHash;
	{
		std::lock_guard<std::mutex> lock(g_vsMutex);
		g_lastBoundVertexDeclElements = realElements;
		lastShaderHash = g_lastBoundVertexShaderUcodeHash;
	}
	dumpVertexDeclUsage(realElements, lastShaderHash);
}

REXCVAR_DEFINE_BOOL(renut_dump_code, false, "Nuts&Bolts/Graphics",
	"One-shot dump of raw guest code bytes at known D3D9 function addresses, "
	"for offline PPC disassembly (native-renderer vertex-declaration work).");

void dumpCodeBytesOnce()
{
	static std::once_flag dumped;
	if (!REXCVAR_GET(renut_dump_code)) return;

	std::call_once(dumped, [] {
		const uint8_t* base = rex::Runtime::instance()->virtual_membase();
		const std::filesystem::path outDir = rex::filesystem::GetExecutableFolder() / "code_dump";
		std::error_code ec;
		std::filesystem::create_directories(outDir, ec);
		if (ec) {
			RNUT_ERROR("code dump: cannot create {} ({})", outDir.string(), ec.message());
			return;
		}

		struct Target {
			uint32_t address;
			const char* name;
		};
		static constexpr Target kTargets[] = {
			{0x8264EA90u, "CreateVertexDeclaration"},
			{0x8264E8B0u, "CreateVertexShader"},
			{0x8264E6A8u, "CreatePixelShader"},
			{0x8222A0A8u, "SetVertexShader"},
			{0x82205700u, "SetVertexDeclaration"},
		};
		constexpr size_t kDumpBytes = 512;

		for (const Target& t : kTargets) {
			std::ofstream file(outDir / (std::string(t.name) + ".bin"), std::ios::binary | std::ios::trunc);
			if (!file) {
				RNUT_ERROR("code dump: could not open output for {}", t.name);
				continue;
			}
			file.write(reinterpret_cast<const char*>(base + t.address),
				static_cast<std::streamsize>(kDumpBytes));
			RNUT_INFO("code dump: wrote {} bytes for {} @ {:#x} -> code_dump/{}.bin",
				kDumpBytes, t.name, t.address, t.name);
		}
	});
}
