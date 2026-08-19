// Standalone offline analyzer: given a real captured .ucode file (Xenos GPU
// shader microcode, as written by shaders_ucode_hash/<vs|ps>_<hash>.ucode or
// shaders/<vs|ps>_<hash>.ucode), links the SDK's own real shader-analysis
// code (Shader::AnalyzeUcode, the same code the Vulkan pipeline cache uses
// at runtime) and prints structured JSON describing everything
// build_synthetic_container.py needs to build a XenosRecomp-compatible
// container: vertex-fetch bindings (fetch constant, stride, per-attribute
// format/offset/instruction-address), texture bindings (fetch constant to
// ALU register), interpolator count, and pixel-shader color-output mask.
//
// This is a genuinely standalone build, NOT linked against the game's
// Vulkan/PPC runtime -- see build_ucode_analyze.sh for the exact real
// source list this was proven to link against (found by iteratively
// resolving each real missing symbol, not guessed).
//
// Usage: ucode_analyze <vs|ps> <ucode_file>
// Output: one JSON object to stdout.

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

// Real definition normally lives in graphics/flags.cpp, which this tool
// deliberately does not link (it pulls in a RenderDoc integration this
// analysis-only tool has no use for) -- AnalyzeUcode's optional shader-dump
// path reads this cvar but this tool never wants it enabled, so an empty
// local stub is the real, correct value, not a placeholder standing in for
// missing logic.
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "unused by ucode_analyze");

using namespace rex::graphics;

namespace {

std::vector<uint32_t> ReadUcodeDwordsBigEndian(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		std::fprintf(stderr, "ucode_analyze: cannot open '%s'\n", path);
		std::exit(1);
	}
	const std::streamsize size = file.tellg();
	file.seekg(0);
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	file.read(reinterpret_cast<char*>(bytes.data()), size);

	// Real captures are written exactly as the guest holds them (big-endian,
	// see shader_dump.cpp's own comment on this), and Shader's constructor
	// takes ucode_source_endian explicitly -- pass the raw dwords through
	// unmodified and let the Shader constructor perform the real byte-swap
	// to native order itself, matching how the runtime path already does it.
	std::vector<uint32_t> dwords(bytes.size() / 4);
	std::memcpy(dwords.data(), bytes.data(), dwords.size() * 4);
	return dwords;
}

const char* VertexFormatName(xenos::VertexFormat format) {
	switch (format) {
		case xenos::VertexFormat::kUndefined: return "undefined";
		case xenos::VertexFormat::k_8_8_8_8: return "8_8_8_8";
		case xenos::VertexFormat::k_2_10_10_10: return "2_10_10_10";
		case xenos::VertexFormat::k_10_11_11: return "10_11_11";
		case xenos::VertexFormat::k_11_11_10: return "11_11_10";
		case xenos::VertexFormat::k_16_16: return "16_16";
		case xenos::VertexFormat::k_16_16_16_16: return "16_16_16_16";
		case xenos::VertexFormat::k_16_16_FLOAT: return "16_16_FLOAT";
		case xenos::VertexFormat::k_16_16_16_16_FLOAT: return "16_16_16_16_FLOAT";
		case xenos::VertexFormat::k_32: return "32";
		case xenos::VertexFormat::k_32_32: return "32_32";
		case xenos::VertexFormat::k_32_32_32_32: return "32_32_32_32";
		case xenos::VertexFormat::k_32_FLOAT: return "32_FLOAT";
		case xenos::VertexFormat::k_32_32_FLOAT: return "32_32_FLOAT";
		case xenos::VertexFormat::k_32_32_32_32_FLOAT: return "32_32_32_32_FLOAT";
		case xenos::VertexFormat::k_32_32_32_FLOAT: return "32_32_32_FLOAT";
		default: return "unknown";
	}
}

void PrintJsonString(const char* s) {
	std::printf("\"%s\"", s);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 3 || (std::string_view(argv[1]) != "vs" && std::string_view(argv[1]) != "ps")) {
		std::fprintf(stderr, "usage: ucode_analyze <vs|ps> <ucode_file>\n");
		return 1;
	}
	const bool is_vertex = std::string_view(argv[1]) == "vs";
	const std::vector<uint32_t> ucode_dwords = ReadUcodeDwordsBigEndian(argv[2]);
	if (ucode_dwords.empty()) {
		std::fprintf(stderr, "ucode_analyze: empty ucode file '%s'\n", argv[2]);
		return 1;
	}

	Shader shader(is_vertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel,
	              /*ucode_data_hash=*/0, ucode_dwords.data(), ucode_dwords.size(),
	              std::endian::big);
	rex::string::StringBuffer disasm;
	shader.AnalyzeUcode(disasm);

	std::printf("{\n");
	std::printf("  \"shader_type\": %s,\n", is_vertex ? "\"vs\"" : "\"ps\"");
	std::printf("  \"writes_interpolators\": %u,\n", shader.writes_interpolators());
	std::printf("  \"writes_color_targets\": %u,\n", shader.writes_color_targets());
	std::printf("  \"writes_depth\": %s,\n", shader.writes_depth() ? "true" : "false");

	std::printf("  \"vertex_bindings\": [\n");
	const auto& vbindings = shader.vertex_bindings();
	for (size_t i = 0; i < vbindings.size(); ++i) {
		const auto& vb = vbindings[i];
		std::printf("    {\n");
		std::printf("      \"binding_index\": %d,\n", vb.binding_index);
		std::printf("      \"fetch_constant\": %u,\n", vb.fetch_constant);
		std::printf("      \"stride_words\": %u,\n", vb.stride_words);
		std::printf("      \"attributes\": [\n");
		for (size_t j = 0; j < vb.attributes.size(); ++j) {
			const auto& fi = vb.attributes[j].fetch_instr;
			std::printf("        {\n");
			std::printf("          \"data_format\": %u,\n", uint32_t(fi.attributes.data_format));
			std::printf("          \"data_format_name\": ");
			PrintJsonString(VertexFormatName(fi.attributes.data_format));
			std::printf(",\n");
			std::printf("          \"offset_words\": %d,\n", fi.attributes.offset);
			std::printf("          \"stride_words\": %u,\n", fi.attributes.stride);
			std::printf("          \"is_signed\": %s,\n", fi.attributes.is_signed ? "true" : "false");
			std::printf("          \"is_integer\": %s,\n", fi.attributes.is_integer ? "true" : "false");
			std::printf("          \"exp_adjust\": %d,\n", fi.attributes.exp_adjust);
			std::printf("          \"signed_rf_mode\": %u,\n", uint32_t(fi.attributes.signed_rf_mode));
			std::printf("          \"is_mini_fetch\": %s\n", fi.is_mini_fetch ? "true" : "false");
			std::printf("        }%s\n", (j + 1 < vb.attributes.size()) ? "," : "");
		}
		std::printf("      ]\n");
		std::printf("    }%s\n", (i + 1 < vbindings.size()) ? "," : "");
	}
	std::printf("  ],\n");

	std::printf("  \"texture_bindings\": [\n");
	const auto& tbindings = shader.texture_bindings();
	for (size_t i = 0; i < tbindings.size(); ++i) {
		const auto& tb = tbindings[i];
		// The tfetch instruction's own UV/coordinate source operand -- its
		// storage_index is the ALU register the pixel shader reads the
		// interpolator's value from, real and structured (InstructionOperand,
		// shader.h), no disassembly-text parsing needed.
		uint32_t src_reg = 0;
		bool has_src_reg = false;
		if (tb.fetch_instr.operand_count > 0 &&
		    tb.fetch_instr.operands[0].storage_source == InstructionStorageSource::kRegister) {
			src_reg = tb.fetch_instr.operands[0].storage_index;
			has_src_reg = true;
		}
		std::printf("    {\n");
		std::printf("      \"binding_index\": %zu,\n", tb.binding_index);
		std::printf("      \"fetch_constant\": %u,\n", tb.fetch_constant);
		std::printf("      \"dimension\": %u,\n", uint32_t(tb.fetch_instr.dimension));
		if (has_src_reg) {
			std::printf("      \"src_register\": %u\n", src_reg);
		} else {
			std::printf("      \"src_register\": null\n");
		}
		std::printf("    }%s\n", (i + 1 < tbindings.size()) ? "," : "");
	}
	std::printf("  ],\n");

	std::printf("  \"float_constants\": [");
	const auto& cmap = shader.constant_register_map();
	bool first_const = true;
	for (uint32_t reg = 0; reg < 256; ++reg) {
		uint32_t block_index = reg / 64;
		uint32_t bit_index = reg % 64;
		if (cmap.float_bitmap[block_index] & (uint64_t(1) << bit_index)) {
			std::printf("%s%u", first_const ? "" : ", ", reg);
			first_const = false;
		}
	}
	std::printf("],\n");
	std::printf("  \"float_dynamic_addressing\": %s,\n",
	             cmap.float_dynamic_addressing ? "true" : "false");

	std::printf("  \"bool_constants\": [");
	bool first_bool = true;
	for (uint32_t reg = 0; reg < 256; ++reg) {
		uint32_t block_index = reg / 32;
		uint32_t bit_index = reg % 32;
		if (cmap.bool_bitmap[block_index] & (uint32_t(1) << bit_index)) {
			std::printf("%s%u", first_bool ? "" : ", ", reg);
			first_bool = false;
		}
	}
	std::printf("]\n");

	std::printf("}\n");
	return 0;
}
