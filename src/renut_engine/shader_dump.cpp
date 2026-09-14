// =============================================================================
// Guest shader dumping.
//
// Captures the D3D9 shader blobs the XEX hands to D3DDevice_CreateVertexShader
// (0x8264E8B0) and D3DDevice_CreatePixelShader (0x8264E6A8). D3D9 is statically
// linked into the XEX, so despite the rex_ names in renut_gpu_funcs.toml these
// are ordinary recompiled guest functions and a midasm hook fires normally.
// Both hooks sit on the function's first instruction (`mflr r12`), so r3 still
// holds the incoming pFunction argument.
//
// Creation is the cheap chokepoint: it runs once per shader at load, unlike the
// Set*Shader path which runs per draw.
//
// pFunction is self-describing. Confirmed against both decompiled creators --
// each reads the same three leading words and copies exactly two ranges out of
// them (XMemCpy of pFunction[1] bytes from pFunction, then memcpy of
// pFunction[2] bytes from pFunction + pFunction[1]):
//
//   [0] flags -- bit 0 selects the extended (872-byte) shader object over 40
//   [1] header size in bytes; the microcode begins at pFunction + [1]
//   [2] microcode size in bytes
//
// So header + microcode is the entire blob, and one register is enough to dump
// it. All three words are big-endian; the dumped bytes are written exactly as
// the guest holds them, which is the byte order Xenos ucode tooling expects.
//
// This is distinct from the SDK's `dump_shaders` cvar, which dumps translated
// host shaders down in the GPU backend. This dumps the original guest blob.
//
// Purely observational: neither hook returns or redirects.
// =============================================================================

#include <rex/ppc.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/runtime.h>
#include "renut_logging.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

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

// Both sizes are bounded by what the guest is willing to allocate and copy; a
// real shader is a few KiB at most. Anything past this is not a shader blob.
constexpr uint32_t kMaxBlobBytes = 1u << 20;

// The three leading words are read unconditionally, so the header can never be
// smaller than they are.
constexpr uint32_t kMinHeaderBytes = 12;

std::mutex g_dumpMutex;
std::unordered_set<uint64_t> g_dumped;

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
	// Disabled by default: one bool check per shader creation.
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

	const uint8_t* ucode = blob + headerSize;
	const size_t total = static_cast<size_t>(headerSize) + ucodeSize;

	// The game recreates the same shaders across loads; hash the blob so each
	// distinct one is written once per launch.
	const uint64_t hash = XXH3_64bits(blob, total);
	{
		std::lock_guard<std::mutex> lock(g_dumpMutex);
		if (!g_dumped.insert(hash).second) return;
	}

	// Always "shaders" alongside the exe, so dumps land next to the build being
	// run rather than wherever the working directory happens to point.
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

	// .bin is the complete pFunction the game passed; .ucode is just the Xenos
	// microcode, which is what a shader disassembler wants.
	if (!writeFile(outDir / (std::string(stem) + ".bin"), blob, total)) return;
	if (!writeFile(outDir / (std::string(stem) + ".ucode"), ucode, ucodeSize)) return;

	RNUT_INFO("shader dump: {} (flags {:#x}, header {} B, ucode {} B) from {:#x}",
		stem, flags, headerSize, ucodeSize, pFunction);
}

}  // namespace

// D3DDevice_CreateVertexShader @ 0x8264E8B0 -- r3 = pFunction at entry.
void dumpVertexShader_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "vs");
}

// D3DDevice_CreatePixelShader @ 0x8264E6A8 -- r3 = pFunction at entry.
void dumpPixelShader_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "ps");
}
