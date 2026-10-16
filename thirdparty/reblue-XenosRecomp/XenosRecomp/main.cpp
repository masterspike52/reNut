#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#include <mutex>
#include <vector>

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    uint32_t specConstantsMask = 0;
#ifdef RENUT_RECOMP
    uint32_t vertexFetchMemoryMask = 0;
    uint32_t textureFetchMask = 0;
    uint32_t memExportConstants[8] = {};
    uint32_t floatConstants[8] = {};
    std::vector<ShaderRecompiler::VertexInput> vertexInputs;
    // The file (and container index) the shader came from, naming its HLSL dump.
    std::string sourceName;
#endif
};

// Per-shader recompile failures, collected from the parallel loop and reported before exit.
struct ShaderFailure
{
    XXH64_hash_t hash;
    std::string reason;
};

static std::mutex g_failureMutex;
static std::vector<ShaderFailure> g_failures;

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
#ifdef RENUT_RECOMP
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path] [optional: HLSL dump dir]");
#else
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]");
#endif
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

#ifdef RENUT_RECOMP
    std::string hlslDumpDir;
    if (argc > 4)
        hlslDumpDir = argv[4];
    else if (const char* env = std::getenv("XENOS_RECOMP_HLSL_DUMP"))
        hlslDumpDir = env;
#endif

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;
#ifdef RENUT_RECOMP
            int containerIndex = 0;
#endif

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
                    dataSize <= (fileSize - i) &&
                    shaderContainer->field1C == 0 &&
                    shaderContainer->field20 == 0)
                {
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
#ifdef RENUT_RECOMP
                        std::string stem = file.path().stem().string();
                        if (containerIndex > 0)
                            stem += fmt::format(".{}", containerIndex);
                        shader.first->second.sourceName = std::move(stem);
#endif
                        foundAny = true;
                    }

#ifdef RENUT_RECOMP
                    ++containerIndex;
#endif
                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

#ifdef RENUT_RECOMP
        if (!hlslDumpDir.empty())
            std::filesystem::create_directories(hlslDumpDir);
#endif

        std::atomic<uint32_t> progress = 0;

        std::for_each(std::execution::par_unseq, shaders.begin(), shaders.end(), [&](auto& hashShaderPair)
            {
                auto& shader = hashShaderPair.second;
                const XXH64_hash_t hash = hashShaderPair.first;

                auto recordFailure = [hash](const char* reason)
                {
                    std::lock_guard<std::mutex> lock(g_failureMutex);
                    g_failures.push_back({hash, reason});
                };

                thread_local ShaderRecompiler recompiler;
                recompiler = {};
                recompiler.recompile(shader.data, include);

                shader.specConstantsMask = recompiler.specConstantsMask;
#ifdef RENUT_RECOMP
                shader.vertexFetchMemoryMask = recompiler.vertexFetchMemoryMask;
                shader.textureFetchMask = recompiler.textureFetchMask;
                std::copy(std::begin(recompiler.memExportConstants), std::end(recompiler.memExportConstants),
                    std::begin(shader.memExportConstants));
                std::copy(std::begin(recompiler.floatConstants), std::end(recompiler.floatConstants),
                    std::begin(shader.floatConstants));
                shader.vertexInputs = recompiler.vertexInputs;

                if (!hlslDumpDir.empty())
                {
                    auto path = std::filesystem::path(hlslDumpDir) /
                        (shader.sourceName.empty()
                            ? fmt::format("{}_{:016X}", recompiler.isPixelShader ? "ps" : "vs", hash)
                            : shader.sourceName);
                    path += ".hlsl";

                    std::string contents = fmt::format(
                        "// {} shader  hash=0x{:016X}  specConstants=0x{:X}\n",
                        recompiler.isPixelShader ? "pixel" : "vertex",
                        hash, recompiler.specConstantsMask);
                    contents.append(recompiler.out);
                    writeAllBytes(path.string().c_str(), contents.data(), contents.size());
                }
#endif

                thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
                shader.dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false);
                if (shader.dxil == nullptr)
                {
                    recordFailure("dxc-dxil-compile-failed");
                    return;
                }
                if (*(reinterpret_cast<uint32_t*>(shader.dxil->GetBufferPointer()) + 1) == 0)
                {
                    recordFailure("dxil-not-signed");
                    return;
                }
#endif

                IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
                if (spirv == nullptr)
                {
                    recordFailure("dxc-spirv-compile-failed");
                    return;
                }

                if (!smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo))
                {
                    spirv->Release();
                    recordFailure("smolv-encode-failed");
                    return;
                }

                spirv->Release();

                size_t currentProgress = ++progress;
                if ((currentProgress % 10) == 0 || (currentProgress == shaders.size() - 1))
                    fmt::println("Recompiling shaders... {}%", currentProgress / float(shaders.size()) * 100.0f);
            });

        if (!g_failures.empty())
        {
            fmt::println(stderr, "Recompile failures ({}):", g_failures.size());
            for (const auto& failure : g_failures)
                fmt::println(stderr, "  hash=0x{:016X} reason={}", failure.hash, failure.reason);
            return 2;
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
#ifdef REBLUE_RECOMP
        f.println("#include \"gpu/shaders/shader_cache.h\"");
#else
        f.println("#include \"shader_cache.h\"");
#endif
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;
#ifdef RENUT_RECOMP
        StringBuffer vertexInputs;
        size_t vertexInputCount = 0;
#endif

        for (auto& [hash, shader] : shaders)
        {
#ifdef RENUT_RECOMP
            const uint32_t* me = shader.memExportConstants;
            const uint32_t* fc = shader.floatConstants;
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, 0x{:X}, 0x{:X}, {{ 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X} }}, "
                "{{ 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X} }}, {}, {} }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0, spirv.size(), shader.spirv.size(), shader.specConstantsMask,
                shader.vertexFetchMemoryMask, shader.textureFetchMask,
                me[0], me[1], me[2], me[3], me[4], me[5], me[6], me[7],
                fc[0], fc[1], fc[2], fc[3], fc[4], fc[5], fc[6], fc[7],
                vertexInputCount, shader.vertexInputs.size());
            for (const auto& input : shader.vertexInputs)
            {
                vertexInputs.println("\t{{ {}, {}, {}, {}, {}, {}, {} }},", input.usage, input.usageIndex, input.fetchIndex,
                    input.format, input.flags, input.strideDwords, input.offsetDwords);
            }
            vertexInputCount += shader.vertexInputs.size();
#else
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {} }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0, spirv.size(), shader.spirv.size(), shader.specConstantsMask);
#endif

            if (shader.dxil != nullptr)
            {
                dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
            }
            
            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
        }

        f.println("}};");

#ifdef RENUT_RECOMP
        // An empty array is not valid C++; the extra entry is never indexed.
        f.println("ShaderVertexInput g_shaderVertexInputs[] = {{");
        f.out += vertexInputs.out;
        f.println("\t{{ 0, 0, 0, 0, 0, 0, 0 }},");
        f.println("}};");
#endif

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
        spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        f.println("const size_t g_shaderCacheEntryCount = {};", shaders.size());
#ifdef RENUT_RECOMP
        // Entries carry vertexFetchMemoryMask, textureFetchMask, memExportConstants,
        // floatConstants and their range of g_shaderVertexInputs; 6: shaders with the
        // screen-space viewport epilogue and the rewritten memExport.
        f.println("extern const uint32_t g_shaderCacheLayout = 6;");
#endif

        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        recompiler.recompile(readAllBytes(input, fileSize).get(), include);
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
