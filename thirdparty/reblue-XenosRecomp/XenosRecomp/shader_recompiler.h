#pragma once

#if defined(RENUT_RECOMP) && (defined(REBLUE_RECOMP) || defined(UNLEASHED_RECOMP))
#error RENUT_RECOMP cannot be combined with another game's recompiler define.
#endif

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
#ifdef RENUT_RECOMP
    // Structured if/else emission: instruction indices where a "} else {" is
    // emitted (then-branch close is part of the event, not ifEndLabels).
    std::unordered_set<uint32_t> elseLabels;
#endif
    uint32_t specConstantsMask = 0;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

#ifdef RENUT_RECOMP
    // Registers defined by the definition table; they are locals, not constant buffer reads.
    std::unordered_set<uint32_t> literalConstants;
    uint32_t loopDepth = 0;
    // Mini vertex fetches reuse the fetch constant, stride and index of the last full fetch.
    VertexFetchInstruction lastFullVertexFetch{};
    // Set once an ALU or texture instruction has written r0.x, which holds the vertex index when
    // the shader starts. A vertex element fetched by any other index (instanced meshes compute it
    // from the vertex index) reads memory rather than the per-vertex input layout. Vertex fetch
    // results do not count: the fetches of a clause all index by the value they start with.
    bool vertexIndexOverwritten = false;
    // Whether the last full vertex fetch (whose index mini fetches reuse) indexed by the vertex index.
    bool lastFullFetchByVertexIndex = true;
    // Whether the last full vertex fetch has its constant, stride and format in the microcode
    // (bundle meshes, drawn with no declaration) rather than patched in by D3DVertexShader_Bind.
    bool lastFullFetchBaked = false;
    // Whether the last full vertex fetch reads an element of the bound declaration (format and
    // stride patched in by D3DVertexShader_Bind, zeros in the microcode) by an index other than
    // the vertex index: N&B's leaf cards read their per-card data at the vertex index / 4. The
    // host describes the declaration's element for it (vfetchBound in shader_common.h).
    bool lastFullFetchBoundByOtherIndex = false;
    // Inputs vfetchBound can describe: SharedConstants holds one descriptor per vertexInputs slot.
    static constexpr size_t MAX_BOUND_FETCHES = 16;
    // Vertex fetch constants read through vfetchMemory, bit i = fetch index 64 + i. The host
    // copies these buffers for each draw.
    uint32_t vertexFetchMemoryMask = 0;
    // Texture fetch constants the shader samples, bit i = fetch constant i.
    uint32_t textureFetchMask = 0;
    // Float constant registers the shader uses as memexport stream constants, bit i = register i:
    // the XDK compiler gives each export stream a constant (XGRAPHICS::DoMemExport, 0x828E0D80,
    // through CP_AllocExportConst) and adds it to the scaled element index as it writes eA, so it
    // is the last operand of that instruction. The host reads them to find the memory each draw
    // exports to.
    uint32_t memExportConstants[8] = {};
    // Float constant registers the shader reads, bit i = register i; all of them when it indexes
    // the register file (a0 / aL). The host uploads and compares only these for each draw.
    uint32_t floatConstants[8] = {};
    // The vertex shader writes the kill flag (oPts.z): the XDK compiler then gives it a kill export
    // mode, and the GPU drops a primitive whose vertices all have a non-zero flag (N&B's D3D never
    // sets PA_CL_CLIP_CNTL.vtx_kill_or). The grass kills its blades where the density map is
    // below their threshold.
    bool hasVertexKill = false;
    // Each vertex element the shader fetches, as its vfetch describes it: what a vertex
    // declaration would say. N&B draws bundle meshes with no declaration set; the host builds
    // their input layout from this.
    struct VertexInput
    {
        uint8_t usage;
        uint8_t usageIndex;
        uint8_t fetchIndex;    // Xenos vertex fetch constant 0..95
        uint8_t format;        // GPUVERTEXFORMAT
        uint8_t flags;         // 1 signed, 2 integer, 4 fetched by vfetchBound (see above)
        uint8_t strideDwords;
        uint16_t offsetDwords;
    };
    std::vector<VertexInput> vertexInputs;
    // Addresses of the dp4 / dp3 instructions whose results reach the position export, emitted
    // as dotPrecise (see findPositionDots), and of the ALU instruction being recompiled.
    std::unordered_set<uint32_t> positionDots;
    uint32_t aluAddress = 0;
    // Depth-like texture fetches, for renutShadowCompare (shader_common.h). A register lane
    // (register * 4 + lane) a fetch's X lands in gets the fetch's coordinate, texture and sampler
    // in variables of its own (renutTapUV_<register>_<lane> and so on), set where the fetch runs:
    // of fetches in branches, the one that ran. renutTapLanes holds the lanes whose value is such
    // a fetch's until something else writes them; renutTapDeclared every lane that has them.
    std::unordered_set<uint32_t> renutTapLanes;
    std::unordered_set<uint32_t> renutTapDeclared;
#endif

#ifdef REBLUE_RECOMP
    bool hasShadowTexture = false;
    uint32_t shadowTapUVCount = 0;
    std::unordered_map<uint32_t, uint32_t> shadowTapSlots;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

#ifdef RENUT_RECOMP
    void recompileConstants(const uint8_t* shaderData);
    void findPositionDots(const be<uint32_t>* code, uint32_t instrSize);
#endif

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
