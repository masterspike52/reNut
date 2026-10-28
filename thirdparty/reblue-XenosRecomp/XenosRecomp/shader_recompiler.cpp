#include "shader_recompiler.h"
#include "shader_common.h"

#ifdef RENUT_RECOMP
// Stands in the vertex shader's parameter list for the kill output (hasVertexKill).
static constexpr std::string_view KILL_OUTPUT_PLACEHOLDER = "/*renut kill output*/";
// Stands in a pixel shader's locals for what its depth-like fetches keep (renutTapLanes).
static constexpr std::string_view TAP_UV_PLACEHOLDER = "/*renut tap coordinates*/";
#endif

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
#ifdef RENUT_RECOMP
    // N&B vertex formats are patched in at runtime from the vertex declaration, so the
    // host input layout decodes every element to float4 (normalized/DEC3N/half included).
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "float4", // BLENDINDICES
    "float4", // NORMAL
    "float4", // PSIZE
    "float4", // TEXCOORD
    "float4", // TANGENT
    "float4", // BINORMAL
#else
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "uint4", // BLENDINDICES
#ifdef REBLUE_RECOMP
    // BD IA-decodes SNORM normals to float; swapFloats() undoes the engine int16-pair swap, DEC3N goes through tfetchR11G11B10().
    "float4", // NORMAL
#else
    "uint4", // NORMAL
#endif
    "float4", // PSIZE
    "float4", // TEXCOORD
#ifdef REBLUE_RECOMP
    "float4", // TANGENT
    "float4", // BINORMAL
#else
    "uint4", // TANGENT
    "uint4", // BINORMAL
#endif
#endif
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
#if defined(RENUT_RECOMP)
    #define RENUT_VERTEX_LOCATION_ROW(usage, index, location) { DeclUsage::usage, index, location },
    RENUT_VERTEX_INPUT_LOCATIONS(RENUT_VERTEX_LOCATION_ROW)
    #undef RENUT_VERTEX_LOCATION_ROW
#elif defined(REBLUE_RECOMP)
    #define REBLUE_VERTEX_LOCATION_ROW(usage, index, location) { DeclUsage::usage, index, location },
    REBLUE_VERTEX_INPUT_LOCATIONS(REBLUE_VERTEX_LOCATION_ROW)
    #undef REBLUE_VERTEX_LOCATION_ROW
#else
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::Color, 0, 8 },
    { DeclUsage::BlendIndices, 0, 9 },
    { DeclUsage::BlendWeight, 0, 10 },
    { DeclUsage::Color, 1, 11 },
    { DeclUsage::TexCoord, 4, 12 },
    { DeclUsage::TexCoord, 5, 13 },
    { DeclUsage::TexCoord, 6, 14 },
    { DeclUsage::TexCoord, 7, 15 },
    { DeclUsage::Position, 1, 15 },
#endif
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 },
#ifdef RENUT_RECOMP
    // N&B also passes vectors through NORMAL/TANGENT/BINORMAL interpolators.
    { DeclUsage::Normal, 0 },
    { DeclUsage::Normal, 1 },
    { DeclUsage::Normal, 2 },
    { DeclUsage::Tangent, 0 },
    { DeclUsage::Binormal, 0 },
#endif
};

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "3D", 
    "Cube" 
};

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

void ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
    }
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
#ifdef RENUT_RECOMP
    if (!instr.isMiniFetch)
    {
        lastFullVertexFetch = instr;
        // Only a fetch whose constant, stride and format are in the microcode can read memory:
        // shaders bound to a declaration get theirs patched in by D3DVertexShader_Bind (zeros
        // here), and take their elements from the input layout.
        const bool baked = instr.stride != 0 && instr.format != 0;
        const bool byVertexIndex = instr.srcRegister == 0 && instr.srcSwizzle == 0 && !vertexIndexOverwritten;
        lastFullFetchBaked = baked;
        lastFullFetchByVertexIndex = !baked || byVertexIndex;
        lastFullFetchBoundByOtherIndex = !baked && !byVertexIndex;
    }

    // A bound declaration's element read by another index: the input layout cannot deliver it,
    // and the microcode has no format or stride for vfetchMemory. vfetchBound reads the element
    // as the host describes it from the declaration, per input (the input's slot in vertexInputs).
    if (auto element = vertexElements.find(address);
        lastFullFetchBoundByOtherIndex && element != vertexElements.end())
    {
        const VertexFetchInstruction& full = lastFullVertexFetch;
        size_t inputIndex = 0;
        while (inputIndex < vertexInputs.size() &&
            !(vertexInputs[inputIndex].usage == uint8_t(element->second.usage) &&
                vertexInputs[inputIndex].usageIndex == uint8_t(element->second.usageIndex)))
            ++inputIndex;
        if (inputIndex == vertexInputs.size())
        {
            VertexInput input{};
            input.usage = uint8_t(element->second.usage);
            input.usageIndex = uint8_t(element->second.usageIndex);
            input.fetchIndex = uint8_t(full.constIndex * 3 + full.constIndexSelect);
            input.offsetDwords = uint16_t(instr.offset);
            vertexInputs.push_back(input);
        }

        if (inputIndex < MAX_BOUND_FETCHES)
        {
            vertexInputs[inputIndex].flags |= 4;

            std::string sourceSwizzle;
            for (size_t i = 0; i < 4; i++)
            {
                const auto swizzle = getDestSwizzle(instr.dstSwizzle, i);
                if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
                    sourceSwizzle += SWIZZLES[uint32_t(swizzle)];
            }

            if (instr.isPredicated)
            {
                indent();
                println("if ({}p0)", instr.predicateCondition ? "" : "!");
                indent();
                out += "{\n";
                ++indentation;
            }

            if (!sourceSwizzle.empty())
            {
                indent();
                print("r{}.", instr.dstRegister);
                printDstSwizzle(instr.dstSwizzle, false);
                println(" = vfetchBound(g_BoundFetch({0}), g_VertexFetchSlot(g_BoundFetch({0}).x), r{1}.{2}, {3}, {4}, {5}).{6};",
                    inputIndex, full.srcRegister, SWIZZLES[full.srcSwizzle], full.isIndexRounded ? "true" : "false",
                    instr.signedRfModeAll ? "true" : "false", int32_t(instr.expAdjust), sourceSwizzle);
            }

            printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

            if (instr.isPredicated)
            {
                --indentation;
                indent();
                out += "}\n";
            }
            return;
        }
        fmt::println(stderr, "vfetch by computed index from bound input {} beyond the {} described",
            inputIndex, MAX_BOUND_FETCHES);
    }

    // Baked microcode can fetch two elements under one semantic: Kazooie's feather layer reads its
    // 8_8_8_8 bone weights and its signed 2_10_10_10 normal both as TEXCOORD0, each at its own
    // offset. The input layout has one element per semantic, the first fetch's (vertexInputs), so
    // a later fetch of the semantic at another offset or in another format would read that one.
    bool otherElementOfSemantic = false;
    if (auto element = vertexElements.find(address); element != vertexElements.end() && lastFullFetchBaked)
    {
        for (const VertexInput& existing : vertexInputs)
        {
            otherElementOfSemantic |= !(existing.flags & 4) &&
                existing.usage == uint8_t(element->second.usage) &&
                existing.usageIndex == uint8_t(element->second.usageIndex) &&
                (existing.offsetDwords != uint16_t(instr.offset) || existing.format != uint8_t(instr.format));
        }
    }

    // Not a per-vertex stream attribute the input layout provides: no vertex element for this
    // fetch (bone/instance data read with a computed index, or a stream read by vertex index in a
    // shader without a vertex declaration), a vertex element fetched by an index other than the
    // vertex index (meshes instanced by index arithmetic), or another element of a semantic (above).
    // It reads the host's per-draw copy of the buffer the fetch constant describes (see
    // vfetchMemory in shader_common.h).
    if (vertexElements.find(address) == vertexElements.end() || !lastFullFetchByVertexIndex ||
        otherElementOfSemantic)
    {
        std::string sourceSwizzle;
        for (size_t i = 0; i < 4; i++)
        {
            const auto swizzle = getDestSwizzle(instr.dstSwizzle, i);
            if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
                sourceSwizzle += SWIZZLES[uint32_t(swizzle)];
        }

        if (instr.isPredicated)
        {
            indent();
            println("if ({}p0)", instr.predicateCondition ? "" : "!");
            indent();
            out += "{\n";
            ++indentation;
        }

        if (!sourceSwizzle.empty())
        {
            const VertexFetchInstruction& full = lastFullVertexFetch;
            const uint32_t fetchIndex = full.constIndex * 3 + full.constIndexSelect;
            if (fetchIndex >= 64)
                vertexFetchMemoryMask |= 1u << (fetchIndex - 64);
            else
                fmt::println(stderr, "vfetch from constant {} outside the vertex fetch range", fetchIndex);
            indent();
            print("r{}.", instr.dstRegister);
            printDstSwizzle(instr.dstSwizzle, false);
            println(" = vfetchMemory(g_VertexFetchSlot({}), r{}.{}, {}, {}, {}, {}, {}, {}, {}, {}).{};",
                full.constIndex * 3 + full.constIndexSelect, full.srcRegister, SWIZZLES[full.srcSwizzle],
                full.isIndexRounded ? "true" : "false", int32_t(instr.offset), uint32_t(full.stride),
                uint32_t(instr.format), instr.formatCompAll ? "true" : "false", instr.numFormatAll ? "true" : "false",
                instr.signedRfModeAll ? "true" : "false", int32_t(instr.expAdjust), sourceSwizzle);
        }

        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }
#endif

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    auto findResult = vertexElements.find(address);
    assert(findResult != vertexElements.end());

#if defined(RENUT_RECOMP)
    // The host input layout already delivers decoded float4 elements. Record the element as the
    // fetch describes it (mini fetches take the constant and stride of the last full fetch).
    {
        const VertexFetchInstruction& full = instr.isMiniFetch ? lastFullVertexFetch : instr;
        VertexInput input{};
        input.usage = uint8_t(findResult->second.usage);
        input.usageIndex = uint8_t(findResult->second.usageIndex);
        input.fetchIndex = uint8_t(full.constIndex * 3 + full.constIndexSelect);
        input.format = uint8_t(instr.format);
        input.flags = uint8_t((instr.formatCompAll ? 1 : 0) | (instr.numFormatAll ? 2 : 0));
        input.strideDwords = uint8_t(full.stride);
        input.offsetDwords = uint16_t(instr.offset);
        bool known = false;
        for (const VertexInput& existing : vertexInputs)
            known |= existing.usage == input.usage && existing.usageIndex == input.usageIndex;
        if (!known)
            vertexInputs.push_back(input);
    }
#elif defined(REBLUE_RECOMP)
    // Wrap each 16-bit-packed semantic in swapFloats() (per-usage mask); TEXCOORD also runs sintTexcoord() for raw-int bindings.
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedNormals, ");
        break;
    case DeclUsage::Tangent:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedTangents, ");
        break;
    case DeclUsage::Binormal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedBinormals, ");
        break;
    case DeclUsage::BlendWeight:
        print("swapFloats(g_SwappedBlendWeights, ");
        break;
    case DeclUsage::TexCoord:
        print("sintTexcoord(g_SintTexcoords, swapFloats(g_SwappedTexcoords, ");
        break;
    case DeclUsage::Position:
        print("swapFloats(g_SwappedPositions, ");
        break;
    }
#else
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(");
        break;

    case DeclUsage::TexCoord:
        print("tfetchTexcoord(g_SwappedTexcoords, ");
        break;
    }
#endif

    print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));

#if defined(RENUT_RECOMP)
#elif defined(REBLUE_RECOMP)
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        print(", {}))", uint32_t(findResult->second.usageIndex));
        break;

    case DeclUsage::TexCoord:
        print(", {}), {})", uint32_t(findResult->second.usageIndex),
              uint32_t(findResult->second.usageIndex));
        break;

    case DeclUsage::BlendWeight:
    case DeclUsage::Position:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }
#else
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        out += ')';
        break;

    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }
#endif

    out += '.';
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
#ifdef RENUT_RECOMP
    textureFetchMask |= 1u << (instr.constIndex & 31);
    if (!isPixelShader && instr.opcode != FetchOpcode::SetTextureLod && instr.dstRegister == 0)
        vertexIndexOverwritten = true;  // see recompile(const AluInstruction&)

    // setTexLOD stores the LOD that following useRegLod fetches sample at.
    if (instr.opcode == FetchOpcode::SetTextureLod)
    {
        indent();
        if (instr.isPredicated)
            print("if ({}p0) ", instr.predCondition ? "" : "!");
        println("lod = r{}.{};", instr.srcRegister, SWIZZLES[instr.srcSwizzle & 0x3]);
        return;
    }
#endif

    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights
#ifdef RENUT_RECOMP
        && !(instr.opcode == FetchOpcode::GetTextureComputedLod && isPixelShader)
#endif
        )
        return;

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    std::string constName;
    const char* constNamePtr = nullptr;
#ifdef UNLEASHED_RECOMP
    bool subtractFromOne = false;
#endif

    auto findResult = samplers.find(instr.constIndex);
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;

    #ifdef UNLEASHED_RECOMP
        subtractFromOne = hasMtxPrevInvViewProjection && strcmp(constNamePtr, "sampZBuffer") == 0;
    #endif
    }
    else
    {
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }

#ifdef UNLEASHED_RECOMP
    if (instr.constIndex == 0 && instr.dimension == TextureDimension::Texture2D)
    {
        indent();
        print("pixelCoord = getPixelCoord({}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(2);
        out += ");\n";
    }
#endif

#ifdef REBLUE_RECOMP
    // Stashed before the fetch statement: the kernel reuses UV registers as
    // fetch destinations (r6.y = tfetch2D(..., r6.yw)).
    const bool shadowTap = hasShadowTexture && !instr.isPredicated &&
        instr.opcode == FetchOpcode::TextureFetch &&
        instr.dimension == TextureDimension::Texture2D &&
        strcmp(constNamePtr, "ShadowTexture") == 0 &&
        instr.offsetX == 0 && instr.offsetY == 0 && shadowTapUVCount < 8;
    if (shadowTap)
    {
        indent();
        print("shadowTapUV[{}] = ", shadowTapUVCount);
        printSrcRegister(2);
        out += ";\n";
    }
#endif

#ifdef RENUT_RECOMP
    // A depth-like fetch (renutShadowCompare): a pixel shader's 2D fetch at a normalized
    // coordinate, without an offset or a register LOD, whose X lands in some lane. Its coordinate
    // is kept before the fetch, whose destination may be the coordinate's register.
    bool renutTap = false;
    if (isPixelShader && instr.opcode == FetchOpcode::TextureFetch &&
        instr.dimension == TextureDimension::Texture2D && !instr.texCoordDenorm && !instr.useRegLod &&
        instr.offsetX == 0 && instr.offsetY == 0)
    {
        for (uint32_t i = 0; i < 4; i++)
            renutTap = renutTap || getDestSwizzle(instr.dstSwizzle, i) == FetchDestinationSwizzle::X;
    }
    if (renutTap)
    {
        indent();
        out += "renutTapCoord = ";
        printSrcRegister(2);
        out += ";\n";
    }
#endif

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {
    #ifdef UNLEASHED_RECOMP
        if (subtractFromOne)
            out += "1.0 - ";
    #endif

        out += "tfetch";
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
#ifdef RENUT_RECOMP
    case FetchOpcode::GetTextureComputedLod:
    {
        out += "getCompTexLod";
        break;
    }
#endif
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
    #ifdef RENUT_RECOMP
        // No 1D descriptor heap: the host uploads 1D textures as 2D with a height of 1.
        dimension = "2D";
    #else
        dimension = "1D";
    #endif
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "3D";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

#ifdef UNLEASHED_RECOMP
    if (bicubic)
        out += "Bicubic";
#endif

#ifdef RENUT_RECOMP
    // Vertex shaders have no derivatives, so they always sample an explicit LOD (base level
    // unless setTexLOD supplied one).
    const bool explicitLod = instr.opcode == FetchOpcode::TextureFetch && (!isPixelShader || instr.useRegLod);
    if (explicitLod)
        out += "Level";
#endif

    print("({0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
#ifdef RENUT_RECOMP
    // texCoordDenorm fetches address texels rather than normalized coordinates.
    if (instr.dimension == TextureDimension::Texture1D)
    {
        if (instr.texCoordDenorm)
            print("texelToUV1D({}_Texture2DDescriptorIndex, ", constNamePtr);
        else
            out += "float2(";
        printSrcRegister(1);
        out += instr.texCoordDenorm ? ")" : ", 0.5)";
    }
    else if (instr.texCoordDenorm && instr.dimension != TextureDimension::TextureCube)
    {
        print("texelToUV{0}({1}_Texture{0}DescriptorIndex, ", dimension, constNamePtr);
        printSrcRegister(componentCount);
        out += ')';
    }
    else
#endif
    printSrcRegister(componentCount);

    switch (instr.dimension)
    {
#ifdef RENUT_RECOMP
    case TextureDimension::Texture1D:
        print(", float2({}, 0.0)", instr.offsetX * 0.5f);
        break;
    case TextureDimension::Texture3D:
        print(", float3({}, {}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f, instr.offsetZ * 0.5f);
        break;
#endif
    case TextureDimension::Texture2D:
        print(", float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
        break;
    case TextureDimension::TextureCube:
        out += ", cubeMapData";
        break;
    }

#ifdef RENUT_RECOMP
    if (explicitLod)
        out += instr.useRegLod ? ", lod" : ", 0.0";
#endif

    out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

#ifdef RENUT_RECOMP
    for (uint32_t i = 0; i < 4; i++)
    {
        const FetchDestinationSwizzle swizzle = getDestSwizzle(instr.dstSwizzle, i);
        const uint32_t lane = instr.dstRegister * 4 + i;
        if (swizzle == FetchDestinationSwizzle::X && renutTap)
        {
            renutTapLanes.insert(lane);
            renutTapDeclared.insert(lane);
            indent();
            println("renutTapUV_{0}_{1} = renutTapCoord; renutTapTexture_{0}_{1} = {2}_Texture2DDescriptorIndex; "
                "renutTapSampler_{0}_{1} = {2}_SamplerDescriptorIndex;", instr.dstRegister, i, constNamePtr);
        }
        else if (swizzle <= FetchDestinationSwizzle::One)
        {
            renutTapLanes.erase(lane);
        }
    }
#endif

#ifdef REBLUE_RECOMP
    if (hasShadowTexture)
    {
        for (size_t i = 0; i < 4; i++)
        {
            if (getDestSwizzle(instr.dstSwizzle, i) <= FetchDestinationSwizzle::One)
                shadowTapSlots.erase(uint32_t(instr.dstRegister * 4 + i));
        }
        if (shadowTap)
        {
            for (size_t i = 0; i < 4; i++)
            {
                if (getDestSwizzle(instr.dstSwizzle, i) == FetchDestinationSwizzle::X)
                    shadowTapSlots[uint32_t(instr.dstRegister * 4 + i)] = shadowTapUVCount;
            }
            ++shadowTapUVCount;
        }
    }
#endif

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
#ifdef RENUT_RECOMP
    // A write to r0.x replaces the vertex index (see vertexIndexOverwritten). Export instructions
    // write both results to the export register instead.
    if (!isPixelShader && !instr.exportData &&
        ((instr.vectorDest == 0 && !instr.vectorDestRelative && (instr.vectorWriteMask & 1) != 0) ||
            (instr.scalarDest == 0 && !instr.scalarDestRelative && (instr.scalarWriteMask & 1) != 0)))
        vertexIndexOverwritten = true;

    // oDepth is a float SV_Depth: only the X lane of a depth export exists.
    if (isPixelShader && instr.exportData && ExportRegister(instr.vectorDest) == ExportRegister::PSDepth &&
        ((instr.vectorWriteMask | instr.scalarWriteMask) & ~0b1u) != 0)
    {
        AluInstruction depthInstr = instr;
        depthInstr.vectorWriteMask &= 0b1;
        depthInstr.scalarWriteMask &= 0b1;
        recompile(depthInstr);
        return;
    }
#endif

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent(); 
        out += "{\n";
        ++indentation;
    }

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

#ifdef RENUT_RECOMP
    // laneMask selects vector operand lanes explicitly instead of following the write mask.
    auto op = [&](size_t operand, uint32_t laneMask = 0)
#else
    auto op = [&](size_t operand)
#endif
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
#ifdef RENUT_RECOMP
            else
            {
                // An ALU instruction reads at most two constants: its two relative flags go to its
                // first and second constant operand, counted in operand order.
                uint32_t constantsBefore = 0;
                if (operand > VECTOR_0 && !instr.src1Select)
                    ++constantsBefore;
                if (operand > VECTOR_1 && !instr.src2Select)
                    ++constantsBefore;
                const bool relative = constantsBefore == 0 ? instr.const0Relative : instr.const1Relative;
                const char* offset = relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "";

                auto findResult = float4Constants.find(reg);
                const bool literal = !relative && literalConstants.count(reg) != 0;
                // Everything else reads the register file; an indexed read may reach any register.
                if (relative)
                    std::fill(std::begin(floatConstants), std::end(floatConstants), ~0u);
                else if (!literal)
                    floatConstants[(reg & 0xFF) / 32] |= 1u << (reg % 32);

                if (literal)
                {
                    regFormatted = fmt::format("c{}", reg);
                }
                else if (findResult != float4Constants.end() && findResult->second->registerCount > 1)
                {
                    regFormatted = fmt::format("{}({}{})", reinterpret_cast<const char*>(constantTableData + findResult->second->name),
                        reg - findResult->second->registerIndex, offset);
                }
                else if (findResult != float4Constants.end() && !relative)
                {
                    regFormatted = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                }
                else
                {
                    // Most N&B shaders have no constant table: read the register file directly.
                    regFormatted = fmt::format("g_C({}{})", reg, offset);
                }
            }
#else
            else
            {
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        assert(!instr.const0Relative && !instr.const1Relative);
                        regFormatted = constantName;
                    }
                }
                else
                {
                    assert(!instr.const0Relative && !instr.const1Relative);
                    regFormatted = fmt::format("c{}", reg);
                }
            }
#endif

            std::string result;

            if (negate)
                result += '-';

            if (abs)
                result += "abs(";

            result += regFormatted;
            result += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                #ifdef RENUT_RECOMP
                    if (laneMask != 0)
                    {
                        mask = laneMask;
                        break;
                    }
                #endif
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1)
                        result += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                result += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                result += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                result += ")";

            return result;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "oDepth";
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

        #ifdef RENUT_RECOMP
            // Point size has no host equivalent. Memexport (N&B's vertex-to-memory passes): eA
            // and eM# are locals, and each eM# write stores through memExport (shader_common.h).
            case ExportRegister::VSPointSizeEdgeFlagKillVertex:
                exportRegister = "oPts";
                if (((instr.vectorWriteMask | instr.scalarWriteMask) & 0b100) != 0)
                    hasVertexKill = true;
                break;
            case ExportRegister::ExportAddress:
            {
                exportRegister = "eA";
                // The stream constant is the last operand of the multiply-add that writes eA (see
                // memExportConstants), read directly: the first or second constant operand, by the
                // constants before it, without its relative flag.
                const uint32_t constantsBefore = (instr.src1Select ? 0u : 1u) + (instr.src2Select ? 0u : 1u);
                const bool streamRelative = constantsBefore == 0 ? instr.const0Relative : instr.const1Relative;
                if (instr.vectorOpcode == AluVectorOpcode::Mad && !instr.src3Select && !streamRelative &&
                    literalConstants.count(instr.src3Register) == 0)
                {
                    memExportConstants[(instr.src3Register & 0xFF) / 32] |= 1u << (instr.src3Register % 32);
                }
                else
                {
                    fmt::println(stderr, "eA not written from a stream constant register (opcode {})",
                        uint32_t(instr.vectorOpcode));
                }
                break;
            }
            case ExportRegister::ExportData0:
                exportRegister = "eM0";
                break;
            case ExportRegister::ExportData1:
                exportRegister = "eM1";
                break;
            case ExportRegister::ExportData2:
                exportRegister = "eM2";
                break;
            case ExportRegister::ExportData3:
                exportRegister = "eM3";
                break;
            case ExportRegister::ExportData4:
                exportRegister = "eM4";
                break;
        #endif

            default:
            {
                auto findResult = interpolators.find(instr.vectorDest);
                assert(findResult != interpolators.end());
                exportRegister = findResult->second;
                break;
            }
            }
        }
    }

#ifdef RENUT_RECOMP
    const size_t vectorBlockStart = out.size();
#endif

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
    #ifdef RENUT_RECOMP
        println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(VECTOR_0, 0b1000));
    #else
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0));
    #endif
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

#ifdef REBLUE_RECOMP
    // A Sgt/Sge whose sources are ShadowTexture taps re-emits per lane as a
    // filtered compare from the stashed tap UV, turning the guest's binary PCF
    // into hardware-equivalent bilinear PCF. Anything else falls through.
    bool shadowCmpRewritten = false;
    if (vectorWriteMask != 0 && !instr.exportData && hasShadowTexture &&
        (instr.vectorOpcode == AluVectorOpcode::Sgt || instr.vectorOpcode == AluVectorOpcode::Sge) &&
        instr.src1Select && !instr.src1Negate && (instr.src1Register & 0x80) == 0 &&
        instr.src2Select && !instr.src2Negate && (instr.src2Register & 0x80) == 0)
    {
        const uint32_t srcA = instr.src1Register & 0x3F;
        const uint32_t srcB = instr.src2Register & 0x3F;
        bool allTaps = instr.vectorDest != srcA && instr.vectorDest != srcB;
        for (size_t i = 0; i < 4 && allTaps; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                allTaps = shadowTapSlots.find(srcA * 4 + (((instr.src1Swizzle >> (i * 2)) + i) & 0x3)) != shadowTapSlots.end();
        }
        if (allTaps)
        {
            for (size_t i = 0; i < 4; i++)
            {
                if (((vectorWriteMask >> i) & 0x1) == 0)
                    continue;
                const uint32_t compA = ((instr.src1Swizzle >> (i * 2)) + i) & 0x3;
                const uint32_t compB = ((instr.src2Swizzle >> (i * 2)) + i) & 0x3;
                indent();
                println("r{}.{} = shadowCmp2D(ShadowTexture_Texture2DDescriptorIndex, shadowTapUV[{}], r{}.{});",
                    instr.vectorDest, SWIZZLES[i], shadowTapSlots[srcA * 4 + compA], srcB, SWIZZLES[compB]);
            }
            shadowCmpRewritten = true;
        }
    }

    if (vectorWriteMask != 0 && !shadowCmpRewritten)
#elif defined(RENUT_RECOMP)
    // A Sgt / Sge with a depth-like fetch's result as one side in every lane it writes (N&B's
    // shadow taps: `taps >= reference`, or `reference > tap` in its deferred passes) re-emits each
    // lane as renutShadowCompare, from the fetch's kept coordinate. Its result is the same compare
    // unless the host flags the texture as a shadow map to filter.
    // Only compares of several taps at once, as N&B's filter kernels make: the result of a lone
    // one (its deferred passes) is taken as a yes or no (cndgt), where a filtered one would only
    // grow the shadow.
    bool renutCompareRewritten = false;
    const uint32_t writtenLanes = (vectorWriteMask & 1u) + ((vectorWriteMask >> 1) & 1u) +
        ((vectorWriteMask >> 2) & 1u) + ((vectorWriteMask >> 3) & 1u);
    if (isPixelShader && writtenLanes >= 2 && !instr.exportData &&
        !instr.vectorDestRelative && !renutTapLanes.empty() &&
        (instr.vectorOpcode == AluVectorOpcode::Sgt || instr.vectorOpcode == AluVectorOpcode::Sge))
    {
        // The tap lane each written lane reads, if that source is a plain register holding one.
        auto tapLane = [&](bool select, uint32_t reg, bool negate, uint32_t swizzle, uint32_t lane) -> int
            {
                if (!select || negate || (reg & 0x80) != 0)
                    return -1;
                const uint32_t key = (reg & 0x3F) * 4 + (((swizzle >> (lane * 2)) + lane) & 0x3);
                return renutTapLanes.count(key) != 0 ? int(key) : -1;
            };
        for (uint32_t side = 0; side < 2 && !renutCompareRewritten; side++)
        {
            const bool tapFirst = side == 0;
            int taps[4] = { -1, -1, -1, -1 };
            bool allTaps = true;
            for (uint32_t i = 0; i < 4 && allTaps; i++)
            {
                if (((vectorWriteMask >> i) & 0x1) == 0)
                    continue;
                taps[i] = tapFirst
                    ? tapLane(instr.src1Select, instr.src1Register, instr.src1Negate, instr.src1Swizzle, i)
                    : tapLane(instr.src2Select, instr.src2Register, instr.src2Negate, instr.src2Swizzle, i);
                allTaps = taps[i] >= 0;
            }
            if (!allTaps)
                continue;

            // Bit 0: the tap is the first operand; bit 1: the compare includes equality.
            const uint32_t mode = (tapFirst ? 1u : 0u) | (instr.vectorOpcode == AluVectorOpcode::Sge ? 2u : 0u);
            // One call for the instruction's lanes; the lanes it does not write get zeros.
            std::string textures, samplers, uvs, tapValues, references, lanes;
            uint32_t laneMask = 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                const char* separator = i ? ", " : "";
                if (taps[i] < 0)
                {
                    textures += fmt::format("{}0u", separator);
                    samplers += fmt::format("{}0u", separator);
                    uvs += ", 0.0";
                    tapValues += fmt::format("{}0.0", separator);
                    references += fmt::format("{}0.0", separator);
                    continue;
                }
                const uint32_t reg = uint32_t(taps[i]) / 4, component = uint32_t(taps[i]) % 4;
                textures += fmt::format("{}renutTapTexture_{}_{}", separator, reg, component);
                samplers += fmt::format("{}renutTapSampler_{}_{}", separator, reg, component);
                uvs += fmt::format(", renutTapUV_{}_{}", reg, component);
                tapValues += fmt::format("{}{}", separator, op(tapFirst ? VECTOR_0 : VECTOR_1, 1u << i));
                references += fmt::format("{}{}", separator, op(tapFirst ? VECTOR_1 : VECTOR_0, 1u << i));
                lanes += SWIZZLES[i];
                laneMask |= 1u << i;
            }
            indent();
            println("r{0}.{1} = renutShadowCompareLanes(uint4({2}), uint4({3}){4}, float4({5}), float4({6}), {7}u, {8}u).{1};",
                instr.vectorDest, lanes, textures, samplers, uvs, tapValues, references, mode, laneMask);
            renutCompareRewritten = true;
        }
    }

    if (vectorWriteMask != 0 && !renutCompareRewritten)
#else
    if (vectorWriteMask != 0)
#endif
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ";

        if (instr.vectorSaturate)
            out += "saturate(";

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            print("{} + {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Mul:
            print("{} * {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
            print("max({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Min:
            print("min({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Seq:
            print("{} == {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sgt:
            print("{} > {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sge:
            print("{} >= {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sne:
            print("{} != {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Frc:
            print("frac({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Trunc:
            print("trunc({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Floor:
            print("floor({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Mad:
            print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndEq:
            print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGe:
            print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGt:
            print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
        #ifdef RENUT_RECOMP
            if (positionDots.count(aluAddress) != 0)
            {
                print("dotPrecise({}, {})", op(VECTOR_0), op(VECTOR_1));
                break;
            }
        #endif
            print("dot({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dp2Add:
            print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Cube:
        {
            // Xenos cube takes the direction from src1's swizzled lanes (z, w, x);
            // the canonical emit is src.zzxy which makes this src.xyz, but other
            // compilers emit e.g. src.yyzx (Blue Dragon), so honor the swizzle.
            const uint32_t reg = instr.src1Register & 0x3F;
            const bool srcAbs = (instr.src1Register & 0x80) != 0;
            auto lane = [&](uint32_t i) { return SWIZZLES[((instr.src1Swizzle >> (i * 2)) + i) & 0x3]; };
            print("cube({}{}r{}.{}{}{}{}{}, cubeMapData)",
                instr.src1Negate ? "-" : "", srcAbs ? "abs(" : "", reg,
                lane(2), lane(3), lane(0), lane(0), srcAbs ? ")" : "");
            break;
        }

        case AluVectorOpcode::Max4:
            print("max4({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0));
            break;

        case AluVectorOpcode::KillEq:
            print("any({} == {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGt:
            print("any({} > {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGe:
            print("any({} >= {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillNe:
            print("any({} != {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dst:
            print("dst({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;
        }

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

#ifdef REBLUE_RECOMP
    if (hasShadowTexture && vectorWriteMask != 0 && exportRegister.empty())
    {
        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                shadowTapSlots.erase(uint32_t(instr.vectorDest * 4 + i));
        }
    }
#endif
#ifdef RENUT_RECOMP
    // The lanes written no longer hold a fetch's result (a relative write may reach any).
    if (vectorWriteMask != 0 && exportRegister.empty())
    {
        if (instr.vectorDestRelative)
            renutTapLanes.clear();
        for (uint32_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                renutTapLanes.erase(uint32_t(instr.vectorDest * 4 + i));
        }
    }
#endif

#ifdef RENUT_RECOMP
    const size_t scalarBlockStart = out.size();
#endif

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0));
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            print("max({}, {})", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::Mins:
            print("min({}, {})", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
    #ifdef RENUT_RECOMP
            print("logClamped({})", op(SCALAR_0));
    #else
            print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
    #endif
            break;

    #ifdef RENUT_RECOMP
        case AluScalarOpcode::Rcpf:
            print("rcpf({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqf:
            print("rsqf({})", op(SCALAR_0));
            break;

        // The clamps as operand tests (rcpClamped in shader_common.h): a driver may drop a clamp
        // of an infinity.
        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcp:
            print("rcpClamped({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsq:
            print("rsqClamped({})", op(SCALAR_0));
            break;
    #else
        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;
    #endif

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
#if defined(REBLUE_RECOMP) || defined(RENUT_RECOMP)
            // PRED_SETINV: src==1 -> 0, src==0 -> 1, else passthrough.
            print("{0} == 1.0 ? 0.0 : ({0} == 0.0 ? 1.0 : {0})", op(SCALAR_0));
#else
            print("{0} == 0.0 ? 1.0 : {0}", op(SCALAR_0));
#endif
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0));
            break;
        }

        if (instr.scalarSaturate)
            out += ')';

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
            break;
        }

    #ifdef RENUT_RECOMP
        // Both halves of an ALU instruction read the pre-instruction registers. If the vector
        // write lands on a component the scalar operation reads, compute ps before it.
        if (!instr.exportData && instr.vectorWriteMask != 0)
        {
            uint32_t readRegister = UINT32_MAX;
            uint32_t readMask = 0;
            if (instr.scalarOpcode >= AluScalarOpcode::Mulsc0 && instr.scalarOpcode <= AluScalarOpcode::Subsc1)
            {
                readRegister = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                readMask = 1u << (instr.src3Swizzle & 0x3);
            }
            else if (instr.src3Select)
            {
                readRegister = instr.src3Register & 0x3F;
                readMask = (1u << (((instr.src3Swizzle >> 6) + 3) & 0x3)) | (1u << (instr.src3Swizzle & 0x3));
            }

            if (readRegister == instr.vectorDest && (instr.vectorWriteMask & readMask) != 0)
            {
                std::string scalarBlock = out.substr(scalarBlockStart);
                out.resize(scalarBlockStart);
                out.insert(vectorBlockStart, scalarBlock);
            }
        }
    #endif
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

#ifdef REBLUE_RECOMP
    if (hasShadowTexture && scalarWriteMask != 0 && exportRegister.empty())
    {
        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                shadowTapSlots.erase(uint32_t(instr.scalarDest * 4 + i));
        }
    }
#endif
#ifdef RENUT_RECOMP
    if (scalarWriteMask != 0 && exportRegister.empty())
    {
        if (instr.scalarDestRelative)
            renutTapLanes.clear();
        for (uint32_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                renutTapLanes.erase(uint32_t(instr.scalarDest * 4 + i));
        }
    }
#endif

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
        #ifdef RENUT_RECOMP
            if (i != 0 && exportRegister == "oDepth")
                break;
        #endif
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }

    #ifdef RENUT_RECOMP
        // Memexport: each eM# write stores its element to the stream eA addresses.
        if (exportRegister.size() == 3 && exportRegister.substr(0, 2) == "eM")
        {
            indent();
            println("memExport(eA, {}, {}, g_MemExportStream(0), g_MemExportStream(1), g_MemExportStream(2), "
                "g_MemExportStream(3));", exportRegister, exportRegister[2] - '0');
        }
    #endif
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

#ifdef RENUT_RECOMP
// N&B's projection leaves far geometry's clip z close to its w, so the depth (1 - z / w after
// the viewport) is the small difference of two large dot products. Rounded at every multiply
// and add, as dot is on the host, it wanders by about 1e-7 from vertex to vertex at a wall's
// distance: as far as the decals laid on walls (the stone arches around doors) sit in front of
// them, so they z-fight as the camera moves. Finds the dp4 / dp3 instructions whose results
// reach the position export, directly or through moves, for dotPrecise; walks the instructions
// in control flow order, which is all a write before the export needs.
void ShaderRecompiler::findPositionDots(const be<uint32_t>* code, uint32_t instrSize)
{
    struct Step
    {
        uint32_t address = 0;
        bool fetch = false;
        uint32_t fetchDest = 0;
        uint32_t fetchMask = 0;
        AluInstruction alu{};
    };
    std::vector<Step> steps;

    union
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        };
    };

    auto controlFlowCode = code;
    for (uint32_t instrAddress = 0; instrAddress < instrSize; instrAddress += 12, controlFlowCode += 3)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address, count, sequence;
            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                break;
            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                break;
            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                break;
            default:
                continue;
            }

            auto instructionCode = code + address * 3;
            for (uint32_t i = 0; i < count; i++, sequence >>= 2, instructionCode += 3)
            {
                union
                {
                    VertexFetchInstruction fetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t word0;
                        uint32_t word1;
                        uint32_t word2;
                    };
                };
                word0 = instructionCode[0];
                word1 = instructionCode[1];
                word2 = instructionCode[2];

                Step step;
                step.address = address + i;
                if ((sequence & 0x1) != 0)
                {
                    // Texture fetches keep their destination where vertex fetches do; the
                    // set-texture-state fetches write nothing.
                    step.fetch = true;
                    if (fetch.opcode < FetchOpcode::SetTextureLod)
                    {
                        step.fetchDest = fetch.dstRegister;
                        for (uint32_t lane = 0; lane < 4; lane++)
                        {
                            if (((fetch.dstSwizzle >> (lane * 3)) & 0x7) != uint32_t(FetchDestinationSwizzle::Keep))
                                step.fetchMask |= 1u << lane;
                        }
                    }
                }
                else
                {
                    step.alu = alu;
                }
                steps.push_back(step);
            }
        }
    }

    // The step that last wrote the lane of the temporary register before step 'at', or -1 when
    // it is not an ALU vector result.
    auto writer = [&](uint32_t reg, uint32_t lane, size_t at) -> ptrdiff_t
        {
            while (at-- > 0)
            {
                const Step& step = steps[at];
                if (step.fetch)
                {
                    if (step.fetchDest == reg && ((step.fetchMask >> lane) & 0x1) != 0)
                        return -1;
                    continue;
                }
                const AluInstruction& alu = step.alu;
                if (alu.exportData)
                    continue;
                if (!alu.vectorDestRelative && alu.vectorDest == reg && ((alu.vectorWriteMask >> lane) & 0x1) != 0)
                    return ptrdiff_t(at);
                if (!alu.scalarDestRelative && alu.scalarDest == reg && ((alu.scalarWriteMask >> lane) & 0x1) != 0)
                    return -1;
            }
            return -1;
        };

    // Marks the dot product behind the lane of step 'at's vector result, through moves (the
    // max of a register with itself).
    std::function<void(size_t, uint32_t, uint32_t)> trace = [&](size_t at, uint32_t lane, uint32_t depth)
        {
            const AluInstruction& alu = steps[at].alu;
            if (alu.vectorOpcode == AluVectorOpcode::Dp4 || alu.vectorOpcode == AluVectorOpcode::Dp3)
            {
                positionDots.insert(steps[at].address);
                return;
            }
            const bool move = alu.vectorOpcode == AluVectorOpcode::Max && alu.src1Select && alu.src2Select &&
                alu.src1Register == alu.src2Register && alu.src1Swizzle == alu.src2Swizzle &&
                alu.src1Negate == alu.src2Negate && (alu.src1Register & 0x80) == 0;
            if (!move || depth == 4)
                return;
            const uint32_t sourceLane = ((alu.src1Swizzle >> (lane * 2)) + lane) & 0x3;
            const ptrdiff_t source = writer(alu.src1Register & 0x3F, sourceLane, at);
            if (source >= 0)
                trace(size_t(source), sourceLane, depth + 1);
        };

    for (size_t at = 0; at < steps.size(); at++)
    {
        const AluInstruction& alu = steps[at].alu;
        if (steps[at].fetch || !alu.exportData || ExportRegister(alu.vectorDest) != ExportRegister::VSPosition)
            continue;
        const uint32_t mask = alu.vectorWriteMask & ~alu.scalarWriteMask;
        for (uint32_t lane = 0; lane < 4; lane++)
        {
            if (((mask >> lane) & 0x1) != 0)
                trace(at, lane, 0);
        }
    }
}

// N&B constants. Most of its shaders carry no constant table, so float constants are read by
// register (g_C) over the full 256-register range of both stages, named constants become aliases
// into it, samplers cover all 32 Xenos fetch slots, and bools test the unified register file.
void ShaderRecompiler::recompileConstants(const uint8_t* shaderData)
{
    constexpr uint32_t SAMPLER_COUNT = 32;

    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);
    const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

    uint32_t constantCount = 0;
    const ConstantInfo* constantInfos = nullptr;
    if (shaderContainer->constantTableOffset != NULL)
    {
        const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
        constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);
        constantCount = constantTableContainer->constantTable.constants;
        constantInfos = reinterpret_cast<const ConstantInfo*>(constantTableData + constantTableContainer->constantTable.constantInfo);
    }

    for (uint32_t i = 0; i < constantCount; i++)
    {
        const ConstantInfo& constantInfo = constantInfos[i];
        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo.name);

        switch (constantInfo.registerSet)
        {
        case RegisterSet::Float4:
            for (uint16_t j = 0; j < constantInfo.registerCount; j++)
                float4Constants.emplace(constantInfo.registerIndex + j, &constantInfo);
            break;

        case RegisterSet::Sampler:
            samplers.emplace(constantInfo.registerIndex, constantName);
            break;

        case RegisterSet::Bool:
            boolConstants.emplace(uint32_t(constantInfo.registerIndex) + (isPixelShader ? 128u : 0u), constantName);
            break;
        }
    }

    auto samplerName = [&](uint32_t index)
        {
            auto findResult = samplers.find(index);
            return findResult != samplers.end() ? std::string(findResult->second) : fmt::format("s{}", index);
        };

    out += "#ifdef __spirv__\n\n";

    println("#define g_C(INDEX) select((uint)(INDEX) < 256u, vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + min((uint)(INDEX), 255u) * 16, 0x10), 0.0)",
        shaderName);

    for (uint32_t r = 0; r < SAMPLER_COUNT; r++)
    {
        const std::string name = samplerName(r);
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            println("#define {}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                name, TEXTURE_DIMENSIONS[j], j * SAMPLER_COUNT * 4 + r * 4);
        }
        println("#define {}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
            name, std::size(TEXTURE_DIMENSIONS) * SAMPLER_COUNT * 4 + r * 4);
    }

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", shaderName, isPixelShader ? 1 : 0);
    out += "{\n";
    out += "\tfloat4 g_CArr[256] : packoffset(c0);\n";
    out += "};\n\n";
    out += "#define g_C(INDEX) select((uint)(INDEX) < 256u, g_CArr[min((uint)(INDEX), 255u)], 0.0)\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";
    for (uint32_t r = 0; r < SAMPLER_COUNT; r++)
    {
        const std::string name = samplerName(r);
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                name, TEXTURE_DIMENSIONS[j], j * (SAMPLER_COUNT / 4) + r / 4, SWIZZLES[r % 4]);
        }
        println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
            name, std::size(TEXTURE_DIMENSIONS) * (SAMPLER_COUNT / 4) + r / 4, SWIZZLES[r % 4]);
    }
    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n\n";

    for (uint32_t i = 0; i < constantCount; i++)
    {
        const ConstantInfo& constantInfo = constantInfos[i];
        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo.name);

        if (constantInfo.registerSet == RegisterSet::Float4)
        {
            if (constantInfo.registerCount > 1)
                println("#define {}(INDEX) g_C({} + (INDEX))", constantName, constantInfo.registerIndex.get());
            else
                println("#define {} g_C({})", constantName, constantInfo.registerIndex.get());
        }
        else if (constantInfo.registerSet == RegisterSet::Bool)
        {
            println("#define {} BOOL_BIT({})", constantName, uint32_t(constantInfo.registerIndex) + (isPixelShader ? 128u : 0u));
        }
    }

    out += '\n';
}
#endif

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    assert((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100);
#ifndef RENUT_RECOMP
    assert(shaderContainer->constantTableOffset != NULL);
#endif

    out += include;
    out += '\n';

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

#ifdef RENUT_RECOMP
    recompileConstants(shaderData);
#else
    const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    out += "#ifdef __spirv__\n\n";

#ifdef UNLEASHED_RECOMP
    bool isMetaInstancer = false;
    bool hasIndexCount = false;
#endif

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

#ifdef REBLUE_RECOMP
        if (isPixelShader && constantInfo->registerSet == RegisterSet::Sampler &&
            strcmp(constantName, "ShadowTexture") == 0)
        {
            hasShadowTexture = true;
        }
#endif

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;

                println("#define {}(INDEX) select((INDEX) < {}, vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16, 0x10), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10)",
                    constantName, shaderName, constantInfo->registerIndex * 16);
            }
            
#ifdef REBLUE_RECOMP
            // BD aliases a singleton constant over an array slot; the wider registrant wins so body and cbuffer agree.
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
            {
                uint32_t reg = constantInfo->registerIndex + j;
                auto it = float4Constants.find(reg);
                if (it == float4Constants.end() || it->second->registerCount < constantInfo->registerCount)
                    float4Constants[reg] = constantInfo;
            }
#else
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);
#endif

            break;
        }

        case RegisterSet::Sampler:
        {
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

#ifdef REBLUE_RECOMP
    // BD bodies reference unnamed sampler slots; emit fallback descriptor-index defines for any slot 0..15 the table didn't name.
    for (uint32_t r = 0; r < 16; r++)
    {
        if (samplers.find(r) != samplers.end())
            continue;
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            println("#define s{}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                r, TEXTURE_DIMENSIONS[j], j * 64 + r * 4);
        }
        println("#define s{}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
            r, std::size(TEXTURE_DIMENSIONS) * 64 + r * 4);
    }
#endif

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", isPixelShader ? "Pixel" : "Vertex", isPixelShader ? 1 : 0);
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
#ifdef REBLUE_RECOMP
            // Only the alias winner gets a packoffset slot; a loser would overlap it in the cbuffer and fail DXC.
            auto winner = float4Constants.find(constantInfo->registerIndex);
            if (winner == float4Constants.end() || winner->second != constantInfo)
                continue;
#endif

            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            print("\tfloat4 {}", constantName);

            if (constantInfo->registerCount > 1)
                print("[{}]", constantInfo->registerCount.get());

            println(" : packoffset(c{});", constantInfo->registerIndex.get());

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;
                println("#define {0}(INDEX) select((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)", constantName, tailCount, tailCount - 1);
            }
        }
    }

    out += "};\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
            }

            println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
        }
    }

#ifdef REBLUE_RECOMP
    // Mirror the SPIR-V fallback: packoffset slots for any unnamed sampler index 0..15.
    for (uint32_t r = 0; r < 16; r++)
    {
        if (samplers.find(r) != samplers.end())
            continue;
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            println("\tuint s{}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                r, TEXTURE_DIMENSIONS[j], j * 4 + r / 4, SWIZZLES[r % 4]);
        }
        println("\tuint s{}_SamplerDescriptorIndex : packoffset(c{}.{});",
            r, 4 * std::size(TEXTURE_DIMENSIONS) + r / 4, SWIZZLES[r % 4]);
    }
#endif

    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
#ifdef REBLUE_RECOMP
            // Key named bools by the unified VS(0..127)/PS(128..255) bit so CF tests (also unified) resolve them.
            const uint32_t unifiedBit = uint32_t(constantInfo->registerIndex) + (isPixelShader ? 128u : 0u);
            println("\t#define {} BOOL_BIT({})", constantName, unifiedBit);
            boolConstants.emplace(unifiedBit, constantName);
#else
            println("\t#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));
            boolConstants.emplace(constantInfo->registerIndex, constantName);
#endif
        }
    }

    out += '\n';
#endif

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    out += "#ifndef __spirv__\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    out += "void main(\n";

    if (isPixelShader)
    {
        out += "\tin float4 iPos : SV_Position,\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
        {
#ifdef REBLUE_RECOMP
            const char* interpolation = (usage == DeclUsage::Color) ? "centroid " : "";
#else
            const char* interpolation = "";
#endif
            println("\tin {3}float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)], interpolation);
        }
#ifdef RENUT_RECOMP
        // A point sprite's coordinate across the sprite (0 to 1, from point_sprite_gs), which Xenos
        // puts in the generated register's zw.
        out += "\tin float2 iPointCoord : POINTCOORD,\n";
#endif

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += ",\n\tout float4 oC0 : SV_Target0";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += ",\n\tout float4 oC1 : SV_Target1";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += ",\n\tout float4 oC2 : SV_Target2";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += ",\n\tout float4 oC3 : SV_Target3";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

        #ifdef RENUT_RECOMP
            // N&B refetches the same element from several instructions (one per branch);
            // declare the input once and map every fetch address to it.
            bool alreadyDeclared = false;
            for (auto& [address, declared] : vertexElements)
            {
                if (declared.usage == vertexElement.usage && declared.usageIndex == vertexElement.usageIndex)
                    alreadyDeclared = true;
            }
            if (alreadyDeclared)
            {
                vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
                continue;
            }
        #endif

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

        #ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
        #endif

            out += '\t';

            bool locatedInput = false;
            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    locatedInput = true;
                    break;
                }
            }
            assert(locatedInput);

            println("in {0} i{1}{2} : {3}{2},", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
        }

    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID,\n";
        }
    #elif defined(RENUT_RECOMP)
        // Xenos loads the vertex index into r0.x; vfetches without a vertex element index with it.
        // The instance ID is the patch of a tessellated draw (see renutVertexRegisters).
        out += "\tin uint iVertexId : SV_VertexID,\n";
        out += "\tin uint iInstanceId : SV_InstanceID,\n";
    #endif

        out += "\tout float4 oPos : SV_Position";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);
    #ifdef RENUT_RECOMP
        // Point sprites (point_sprite_gs on the host): the sprite coordinate the pixel shader finds
        // in its generated register (0 for other primitives), and the point size.
        out += ",\n\tout float2 oPointCoord : POINTCOORD,\n\tout float oPointSize : POINTSIZE";
        // Replaced by the kill output once the body shows whether the shader writes oPts.z. Last,
        // after the interpolators: before them it would move each one a register away from where
        // the pixel shader reads it, and D3D12 refuses to link the pair.
        out += KILL_OUTPUT_PLACEHOLDER;
    #endif
    }

    out += ")\n";
    out += "{\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\t[unroll] for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
#ifdef REBLUE_RECOMP
                // BD bakes the sun-shadow receiver PCF tap offsets as shader
                // literals in 1/1024-of-the-map UV units, folded into 0.5+k
                // and 1.0+k anchors plus the raw 1/1024 frac-stagger stride.
                // Their world footprint scales with the shadow coverage box
                // (bd_shadow_distance), so re-anchor each such component on
                // g_ShadowPcfScale (SharedConstants c20.z, byte 328, =
                // max(1/distance, 1024/dimension)). Bit-exact identity at
                // scale 1.0. Gated to pixel shaders sampling ShadowTexture;
                // the +/-3/1024 window (anchors excluded) matches exactly the
                // kernel literals across every BD receiver and nothing else.
                auto shadowKernelComponent = [&](uint32_t u) -> std::string
                {
                    if (hasShadowTexture)
                    {
                        if (u == 0x3A800000)
                            return fmt::format("(asfloat(0x{:X}u) * g_ShadowPcfScale)", u);
                        float f;
                        memcpy(&f, &u, sizeof(f));
                        for (float anchor : { 0.5f, 1.0f })
                        {
                            float d = f > anchor ? f - anchor : anchor - f;
                            if (f != anchor && d <= 3.0f / 1024.0f)
                                return fmt::format("({} + (asfloat(0x{:X}u) - {}) * g_ShadowPcfScale)", anchor, u, anchor);
                        }
                    }
                    return fmt::format("asfloat(0x{:X}u)", u);
                };
                println("\tfloat4 c{} = float4({}, {}, {}, {});",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0),
                    shadowKernelComponent(value[0].get()), shadowKernelComponent(value[1].get()),
                    shadowKernelComponent(value[2].get()), shadowKernelComponent(value[3].get()));
#else
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
#endif
#ifdef RENUT_RECOMP
                literalConstants.insert(definition->registerIndex + i - (isPixelShader ? 256 : 0));
#endif

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                union
                {
                    uint32_t value;
                    struct
                    {
                        int8_t x;
                        int8_t y;
                        int8_t z;
                        int8_t w;
                    };
                };

                value = definition->values[i].get();

                println("\tint4 i{} = int4({}, {}, {}, {});",
                    (definition->registerIndex - 8992) / 4 + i, x, y, z, w);
            }
#ifdef RENUT_RECOMP
            // One (register, count) header word followed by the values.
            definitions += 1;
#else
            definitions += 2;
#endif
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
        #ifdef RENUT_RECOMP
            // Exports address interpolators by register, not by position in this list.
            interpolators.emplace(uint32_t(interpolator.reg), fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        #else
            interpolators.emplace(i, fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        #endif
        }
    }

    if (!isPixelShader)
    {
    #if defined(UNLEASHED_RECOMP)
        if (!hasMtxProjection)
            out += "\toPos = 0.0;\n";
    #elif defined(REBLUE_RECOMP)
        // Always define SV_Position so a skipped position-write block doesn't leave it undef.
        out += "\toPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);
    #ifdef RENUT_RECOMP
        out += "\toPointCoord = 0.0;\n";
        out += "\toPointSize = 0.0;\n";
    #endif

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
        #ifdef RENUT_RECOMP
            // Pixel parameters are enabled when the low byte has bit 1 (the pixel position: one
            // N&B shader has only that) or bit 2 (the point sprite coordinate: N&B's rotated
            // sparkles have only that); usually both, 6. With neither the register byte is
            // meaningless (0). With bit 2 alone it can also be 0 when r0 is an interpolator,
            // which the printed check above skips.
            if (isPixelShader && (shader->fieldC & 0x6) != 0 && i == ((shader->fieldC >> 8) & 0xFF))
            {
                // xy the pixel position (x negated on back faces), zw the point sprite coordinate.
                // The position is in guest pixels: the host targets are g_ResolutionScale times
                // the guest's size, so a host pixel is a fraction of one. While tiling it is
                // relative to the draw's tile (g_PixelOrigin), as the EDRAM position is.
                out += "float4((iPos.xy / g_ResolutionScale - 0.5 - g_PixelOrigin) * float2(iFace ? 1.0 : -1.0, 1.0), iPointCoord);\n";
            }
        #else
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
                out += "float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            }
        #endif
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

#ifdef RENUT_RECOMP
    // Xenos starts a vertex shader with the vertex index (or a tessellated vertex's patch and
    // domain location) in r0 and r1.
    if (!isPixelShader)
        out += "\trenutVertexRegisters(iVertexId, iInstanceId, g_Tessellation, r0, r1);\n";
#endif

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
#ifdef RENUT_RECOMP
    out += "\tfloat lod = 0.0;\n";
    if (!isPixelShader)
    {
        // Export targets with no host output: point size and memexport address/data.
        out += "\tfloat4 oPts = 0.0;\n";
        out += "\tfloat4 eA = 0.0;\n";
        out += "\tfloat4 eM0 = 0.0;\n";
        out += "\tfloat4 eM1 = 0.0;\n";
        out += "\tfloat4 eM2 = 0.0;\n";
        out += "\tfloat4 eM3 = 0.0;\n";
        out += "\tfloat4 eM4 = 0.0;\n";
    }
#endif
    if (isPixelShader)
    {
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
#ifdef REBLUE_RECOMP
        if (hasShadowTexture)
            out += "\tfloat2 shadowTapUV[8] = (float2[8])0;\n";
#endif
#ifdef RENUT_RECOMP
        out += TAP_UV_PLACEHOLDER;
#endif
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    union
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        };
    };

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

#ifdef RENUT_RECOMP
    // Per-instruction control flow summary for the structurizer below.
    enum class CfKind : uint8_t { None, Cond, Uncond, LoopStart, LoopEnd };
    struct CfSummary
    {
        CfKind kind = CfKind::None;
        uint32_t target = 0;
    };
    std::vector<CfSummary> cfSummaries;
    std::vector<uint32_t> loopStarts;
    std::unordered_map<uint32_t, uint32_t> loopEndFor;
    uint32_t cfIndex = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::LoopStart:
                loopStarts.push_back(cfIndex);
                cfSummaries.push_back({ CfKind::LoopStart, 0 });
                break;

            case ControlFlowOpcode::LoopEnd:
                if (loopStarts.empty())
                    simpleControlFlow = false;
                else
                {
                    loopEndFor[loopStarts.back()] = cfIndex;
                    loopStarts.pop_back();
                }
                cfSummaries.push_back({ CfKind::LoopEnd, 0 });
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.direction || cfInstr.condJmp.address <= cfIndex)
                    simpleControlFlow = false;

                cfSummaries.push_back({
                    cfInstr.condJmp.isUnconditional ? CfKind::Uncond : CfKind::Cond,
                    uint32_t(cfInstr.condJmp.address) });
                break;
            }
            }

            if (cfSummaries.size() == cfIndex)
                cfSummaries.push_back({});

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);

            ++cfIndex;
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!loopStarts.empty())
        simpleControlFlow = false;

    if (simpleControlFlow)
    {
        // Recursive-descent structurizer. Xenos control flow in practice is
        // structured code lowered to forward jumps: "cjmp c, ELSE; then;
        // jmp MERGE; ELSE: else" diamonds where the compiler may retarget the
        // then-exit jump at an ENCLOSING merge point (jump chaining), plus
        // LoopStart/LoopEnd pairs. structure() walks a region [lo, hi) with
        // the continuation index "cont" (where control flows after hi): a
        // trailing unconditional jump to the continuation is a no-op after
        // structuring, a conditional jump opens an if (with an else branch
        // when the instruction before its target is an unconditional jump to
        // a legal merge point). Any other shape rejects the shader back to
        // the switch(pc) state machine.
        const uint32_t instrCount = (instrSize / 12) * 2;
        if (cfSummaries.size() > instrCount)
            cfSummaries.resize(instrCount);

        std::function<bool(uint32_t, uint32_t, uint32_t)> structure =
            [&](uint32_t lo, uint32_t hi, uint32_t cont) -> bool
        {
            uint32_t i = lo;
            while (i < hi)
            {
                const CfSummary& summary = cfSummaries[i];
                switch (summary.kind)
                {
                case CfKind::Uncond:
                {
                    if (i + 1 == hi && (summary.target == cont || summary.target == hi))
                        i = hi;
                    else
                        return false;
                    break;
                }
                case CfKind::Cond:
                {
                    const uint32_t t = summary.target;
                    if (t > hi)
                    {
                        if (t != cont)
                            return false;
                        ++ifEndLabels[hi];
                        if (!structure(i + 1, hi, cont))
                            return false;
                        i = hi;
                    }
                    // A then-exit jump targeting the cond target itself is a
                    // no-op (empty else); fall through to the plain-if path,
                    // whose trailing-jump rule deletes it.
                    else if (t < hi && t - 1 > i && cfSummaries[t - 1].kind == CfKind::Uncond &&
                             cfSummaries[t - 1].target != t)
                    {
                        const uint32_t merge = cfSummaries[t - 1].target;
                        if (merge == cont || merge == hi)
                        {
                            elseLabels.insert(t);
                            ++ifEndLabels[hi];
                            if (!structure(i + 1, t - 1, merge == cont ? cont : hi))
                                return false;
                            if (!structure(t, hi, cont))
                                return false;
                            i = hi;
                        }
                        else if (merge < hi)
                        {
                            elseLabels.insert(t);
                            ++ifEndLabels[merge];
                            if (!structure(i + 1, t - 1, merge))
                                return false;
                            if (!structure(t, merge, merge))
                                return false;
                            i = merge;
                        }
                        else
                        {
                            return false;
                        }
                    }
                    else
                    {
                        ++ifEndLabels[t];
                        if (!structure(i + 1, t, t == hi ? cont : t))
                            return false;
                        i = t;
                    }
                    break;
                }
                case CfKind::LoopStart:
                {
                    auto loopEnd = loopEndFor.find(i);
                    if (loopEnd == loopEndFor.end() || loopEnd->second >= hi)
                        return false;
                    if (!structure(i + 1, loopEnd->second, loopEnd->second))
                        return false;
                    i = loopEnd->second + 1;
                    break;
                }
                case CfKind::LoopEnd:
                    return false; // reached without its LoopStart

                default:
                    ++i;
                    break;
                }
            }
            return true;
        };

        simpleControlFlow = structure(0, instrCount, instrCount);
    }

    if (!simpleControlFlow)
    {
        ifEndLabels.clear();
        elseLabels.clear();
    }
#else
    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cfInstr.condJmp.address];

                break;
            }
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }
#endif

#ifdef RENUT_RECOMP
    if (!isPixelShader)
        findPositionDots(code, instrSize);
#endif

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            #ifdef RENUT_RECOMP
                // Inner regions close first (validated nesting), then the
                // enclosing if's then-branch flips to its else-branch.
                if (elseLabels.count(pc) != 0)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                    indent();
                    out += "else\n";
                    indent();
                    out += "{\n";
                    ++indentation;
                }
            #endif
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;
            bool shouldCloseCurlyBracket = false;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
#if defined(REBLUE_RECOMP) || defined(RENUT_RECOMP)
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd ||
                                cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                // Gate the block on its boolean constant (Xenos cexec bN / cexec !bN); baseline ran it unconditionally.
                {
                    indent();
                    auto findResult = boolConstants.find(cfInstr.condExec.boolAddress);
                    if (findResult != boolConstants.end())
                        println("if ({}{})", cfInstr.condExec.condition ? "" : "!", findResult->second);
                    else
                        println("if ({}BOOL_BIT({}))", cfInstr.condExec.condition ? "" : "!", uint32_t(cfInstr.condExec.boolAddress));
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
#else
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecEnd);
#endif
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
            #ifdef RENUT_RECOMP
                // Gate the block on the predicate (Xenos exec with p0 / !p0).
                indent();
                println("if ({}p0)", cfInstr.condExecPred.condition ? "" : "!");
                indent();
                out += "{\n";
                ++indentation;
                shouldCloseCurlyBracket = true;
            #endif
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                #ifdef RENUT_RECOMP
                    // Xenos loop constants pack count/start/step into i#.xyz, and aL is restored
                    // when a nested loop exits.
                    indent();
                    out += "{\n";
                    ++indentation;
                    indent();
                    println("int aLSaved{} = aL;", loopDepth);
                    indent();
                    println("aL = i{}.y;", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    println("for (int loopIteration{0} = 0; loopIteration{0} < i{1}.x; loopIteration{0}++, aL += i{1}.z)",
                        loopDepth, uint32_t(cfInstr.loopStart.loopId));
                    ++loopDepth;
                #else
                    indent();
                #ifdef UNLEASHED_RECOMP
                    print("[unroll] ");
                #endif
                    println("for (aL = 0; aL < i{}.x; aL++)", uint32_t(cfInstr.loopStart.loopId));
                #endif
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else 
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                #ifdef RENUT_RECOMP
                    if (cfInstr.loopEnd.isPredicatedBreak)
                    {
                        indent();
                        println("if ({}p0) break;", cfInstr.loopEnd.condition ? "" : "!");
                    }
                #endif
                    --indentation;
                    indent();
                    out += "}\n";
                #ifdef RENUT_RECOMP
                    --loopDepth;
                    indent();
                    println("aL = aLSaved{};", loopDepth);
                    --indentation;
                    indent();
                    out += "}\n";
                #endif
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < i{}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    // Structured mode: the then-branch exit jump was consumed by
                    // the "} else {" emitted at the next instruction.
                    if (!simpleControlFlow)
                    {
                        println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\tcontinue;\n";
                    }
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
#if defined(REBLUE_RECOMP) || defined(RENUT_RECOMP)
                        // Bools expand to BOOL_BIT(N) expressions; negate when the jump fires on a clear bit.
                        const bool jumpIfSet = cfInstr.condJmp.condition ^ simpleControlFlow;
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ({}{})", jumpIfSet ? "" : "!", findResult->second);
                        else
                            println("if ({}BOOL_BIT({}))", jumpIfSet ? "" : "!", uint32_t(cfInstr.condJmp.boolAddress));
#else
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                        else
                            println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
#endif
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;
            
            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t code0;
                        uint32_t code1;
                        uint32_t code2;
                    };
                };
            
                code0 = instructionCode[0];
                code1 = instructionCode[1];
                code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}";
                            indent();
                            out += "else";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += '}';
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                #ifdef RENUT_RECOMP
                    aluAddress = address + i;
                #endif
                    recompile(alu);
                }

                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldReturn)
            {
            #ifdef RENUT_RECOMP
                // The Xenos alpha test (RB_COLORCONTROL function + reference) is
                // read from SharedConstants at runtime rather than baked in as a
                // spec constant, so DXIL pixel shaders need no runtime linking.
                // As coverage (RENUT_ALPHA_TO_COVERAGE) it rewrites the alpha.
                if (isPixelShader)
                {
                    indent();
                    out += "oC0.w = alphaTest(oC0.w, g_AlphaTestFunc, g_AlphaThreshold);\n";
                }
                else
            #else
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";
                    indent();
                    out += '{';

                    indent();
                    out += "\tclip(oC0.w - g_AlphaThreshold);\n";

                    indent();
                    out += "}";

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)";
                    indent();
                    out += '{';

                    indent();
                    out += "\toC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toC0.w = 0.5 + (oC0.w - g_AlphaThreshold) / max(fwidth(oC0.w), 1e-6);\n";

                    indent();
                    out += '}';
                #endif
                }
                else
            #endif
                {
                #ifdef UNLEASHED_RECOMP
                    if (!hasMtxProjection)
                #endif
                    {
                    #ifdef RENUT_RECOMP
                        // The host viewport is the draw's D3D viewport, and the position
                        // passes through, unless the viewport transform is off: then it is in
                        // pixels (after what is left of the transform), mapped here to clip
                        // space over the whole render target, y down.
                        out += "\tif (g_ScreenSpace != 0)\n\t{\n";
                        out += "\t\tfloat2 pixel = oPos.xy * g_ScreenScale + g_ScreenOffset * oPos.w;\n";
                        out += "\t\toPos.xy = pixel * float2(2.0, -2.0) / g_ViewportSize + float2(-1.0, 1.0) * oPos.w;\n";
                        out += "\t}\n";
                        // For point_sprite_gs: the exported point size (0 when the shader exports
                        // none: the render state's), negative for a killed vertex.
                        out += "\toPointSize = oPts.z != 0.0 ? -1.0 : oPts.x;\n";
                        if (hasVertexKill)
                        {
                            // A negative cull distance at every vertex culls the primitive, as
                            // the GPU drops one whose vertices are all killed. SPIR-V gets no cull
                            // distance (the device does not enable it); a NaN position drops any
                            // primitive with a killed vertex instead.
                            out += "#ifdef __spirv__\n";
                            out += "\tif (oPts.z != 0.0) oPos = asfloat(0x7FC00000u).xxxx;\n";
                            out += "#else\n";
                            out += "\toKill = oPts.z != 0.0 ? -1.0 : 0.0;\n";
                            out += "#endif\n";
                        }
                    #else
                        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
                    #endif
                    }
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }
    else
    {
        // Regions closing one past the last instruction never get visited by
        // the loop above; balance their braces here.
        auto findResult = ifEndLabels.find(pc);
        if (findResult != ifEndLabels.end())
        {
            for (uint32_t i = 0; i < findResult->second; i++)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";

    if (!isPixelShader && hasMtxProjection)
        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
#endif

#ifdef RENUT_RECOMP
    if (size_t at = out.find(KILL_OUTPUT_PLACEHOLDER); at != std::string::npos)
    {
        out.replace(at, KILL_OUTPUT_PLACEHOLDER.size(),
            hasVertexKill ? "\n#ifndef __spirv__\n\t, out float oKill : SV_CullDistance0\n#endif\n" : "");
    }
    if (size_t at = out.find(TAP_UV_PLACEHOLDER); at != std::string::npos)
    {
        // Texture 0 has no filter flags: a compare whose fetch never ran stays the game's.
        std::vector<uint32_t> lanes(renutTapDeclared.begin(), renutTapDeclared.end());
        std::sort(lanes.begin(), lanes.end());
        std::string declarations;
        if (!lanes.empty())
            declarations += "\tfloat2 renutTapCoord = 0.0;\n";
        for (uint32_t lane : lanes)
        {
            declarations += fmt::format("\tfloat2 renutTapUV_{0}_{1} = 0.0;\n\tuint renutTapTexture_{0}_{1} = 0u;\n"
                "\tuint renutTapSampler_{0}_{1} = 0u;\n", lane / 4, lane % 4);
        }
        out.replace(at, TAP_UV_PLACEHOLDER.size(), declarations);
    }
#endif

    out += "}";
}
