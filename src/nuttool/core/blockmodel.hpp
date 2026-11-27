// Model assets made from scratch for reNut, drawn by reNut's own part or world shaders (the shader
// blobs carry the signature reNut recognises instead of console microcode).
#pragma once

#include "common.hpp"

#include <array>
#include <optional>
#include <utility>

namespace nut::blockmodel {

// Pieces the model writers share (npcmodel.cpp writes skinned models and animations with them).
namespace detail {

// A symbol being written, with its relocated pointers.
struct Sym {
    Bytes b;
    std::vector<std::pair<uint32_t, uint32_t>> relocs; // offset, target symbol (1-based)

    uint32_t pos() const { return uint32_t(b.size()); }
    void u32(uint32_t v) {
        b.resize(b.size() + 4);
        putBe32(&b[b.size() - 4], v);
    }
    void u16(uint16_t v) {
        b.resize(b.size() + 2);
        putBe16(&b[b.size() - 2], v);
    }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void align(size_t a) {
        while (b.size() % a) b.push_back(0);
    }
    void str(const std::string& s) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    // A pointer to `value` in symbol `to`; returns where it is, for set().
    uint32_t ptr(uint32_t to, uint32_t value = 0) {
        relocs.push_back({pos(), to});
        u32(value);
        return pos() - 4;
    }
    void set(uint32_t at, uint32_t v) { putBe32(&b[at], v); }
};

// The ELF hash the game's dictionaries are sorted by (string pools hash a string with its NUL).
uint32_t elfHash(const std::string& s);

} // namespace detail

// A shape key's delta at a vertex (reNut's docs/nuttool/custom-models.md): the shape (an index into its model's table)
// and what it adds at weight 1 to the vertex's position and normal.
struct ShapeDelta {
    uint32_t shape = 0;
    std::array<float, 3> position{}, normal{};
};

// A model's shape keys: the table (the model's asset id; a world's chunks share the first's) and the
// names the deltas index. reNut blends them by the weights Lua sets.
struct ShapeTable {
    uint32_t table = 0;
    std::vector<std::string> names;
};

struct Vertex {
    std::array<float, 3> pos{};
    std::array<float, 3> normal{0, 1, 0};
    std::array<float, 4> colour{1, 1, 1, 1};  // not written for a part: its paint comes from the game
};

struct Mesh {
    std::vector<Vertex> vertices;   // at most 65535
    std::vector<uint32_t> indices;  // a triangle list
    std::vector<std::vector<ShapeDelta>> shapes;  // per vertex (empty: none)
};

// The CAFF of aid_model_banjox_vehicleparts_<id>: its rendergraph (bounds, render list with a
// lit draw and a shadow draw, GPU resources), its vertex and index data and its shaders. `shapes`
// names the mesh's shape keys (null: none).
Bytes write(const std::string& id, const Mesh& mesh, const ShapeTable* shapes = nullptr);

// A world model's vertex (32 bytes in the model): full precision position (16-bit ones are too coarse
// across a world), normal, colour (sRGB) and texture coordinate (v from the image's top).
struct WorldVertex {
    std::array<float, 3> pos{};
    std::array<float, 3> normal{0, 1, 0};
    std::array<float, 4> colour{1, 1, 1, 1};
    std::array<float, 2> uv{0, 0};
};

// A world draw's material for reNut's world shader (reNut's docs/nuttool/worlds.md): how its alpha shows,
// what light it gives off and glows with, and its faces. Its record goes after the draw's vertices.
struct WorldMaterial {
    enum Alpha : uint32_t { Opaque = 0, Cutout = 1, Blend = 2, Add = 3 };
    uint32_t alpha = Cutout;            // see-through draws (Blend, Add) go in the game's translucent bucket
    float cutoff = 0.5f, opacity = 1;   // Cutout's limit; the alpha's factor
    std::array<float, 3> emissive{};    // sRGB, unlit, over the light
    float strength = 1;                 // the emissive's factor
    float glow = -1;                    // the bloom it feeds, 0-1; below 0: full when it emits, else the game's
    bool doubleSided = false, unlit = false;
    int emissiveTexture = -1;           // the emissive's texture (fetch constant 1), an index into `textures`
    uint32_t table = 0, index = 0;      // the model's material table and its place there (Lua's names)
};

// One draw of a world model: its own vertex and index buffers, and the texture it binds to fetch
// constant 0 (an index into writeWorld's `textures`), repeating in u and v.
struct WorldDraw {
    std::vector<WorldVertex> vertices;  // at most 65535
    std::vector<uint32_t> indices;      // a triangle list
    uint32_t texture = 0;
    std::vector<std::vector<ShapeDelta>> shapes;  // per vertex (empty: none)
    std::optional<WorldMaterial> material;        // none: cut out under half alpha, as before materials
};

// Water the game draws from triangles, as its own levels' (a model's chunk 0x26, which the scene's water
// setup 0 reads as it starts): every triangle as drawn, and per cell of a 16 x 16 grid over its x/z bounds
// (z-major) the flat levels there, each its height and its triangles' corners seen from above (x, z).
struct WaterRegion {
    std::vector<std::array<float, 3>> vertices;  // three a triangle
    std::array<float, 3> lo{}, hi{};
    struct Level {
        float height = 0;
        std::vector<std::array<float, 6>> triangles;
    };
    std::vector<std::vector<Level>> cells;  // 256: cell z * 16 + cell x
};

// The CAFF of a world's model `assetName` (aid_model_...), drawn by reNut's world shaders: a lit draw
// (bucket 0) and a shadow draw (bucket 5) per draw, naming `textures` (texture assets) as texturefileN.
// `water` (null: none) goes in as its water region.
Bytes writeWorld(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures,
                 const ShapeTable* shapes = nullptr, const WaterRegion* water = nullptr);

// A sky's model, laid out as writeWorld's: drawn by reNut's sky shaders (unlit, each vertex a direction
// from the camera, at infinity), without shadow draws. The scene shows it with op_2C.
Bytes writeSky(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures);

// A bone of a skinned model: each depth's bones after the previous depth's (parents first, breadth-first),
// at most 255. `world` is its bind position in model space; bind rotations are identity.
struct SkinBone {
    std::string name;
    int parent = -1;
    std::array<float, 3> world{};
};

// What a skinned world model adds: its bones and, per draw and vertex, its two skinning words (bone
// indices, then weights; component 0 in the low byte of each big-endian word).
struct Skin {
    std::vector<SkinBone> bones;
    std::vector<std::vector<std::array<uint32_t, 2>>> words;
};

// writeWorld's layout drawn by reNut's skinned vertex shader (40-byte vertices ending in the skinning
// words): the bones in its pose and property dictionary, and listed by its render program's op 0x12.
Bytes writeSkinned(const std::string& assetName, const std::vector<WorldDraw>& draws, const std::vector<std::string>& textures,
                   const Skin& skin, const ShapeTable* shapes = nullptr);

// A draw's shape keys after its vertices in `g` (from `start`, `count` vertices of `stride` bytes), as
// reNut's docs/nuttool/custom-models.md lays them out: returns where its header's index offset is, to set when the
// indices are written (0: the draw has none).
uint32_t appendShapes(detail::Sym& g, uint32_t start, uint32_t count, uint32_t stride,
                      const std::vector<std::vector<ShapeDelta>>& shapes, const ShapeTable& table);

// A draw with shape keys, read back from a model's GPU data: its mesh and each of its shapes' deltas.
struct ShapeDraw {
    uint32_t table = 0;                              // the shape table (the model's asset id)
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uvs;           // empty for a part's vertices, which have none
    std::vector<uint16_t> indices;
    std::vector<std::string> names;                  // this draw's shapes
    std::vector<std::vector<ShapeDelta>> deltas;     // per shape, per vertex (shape: its table index)
};

// Every draw with shape keys in `gpu`, a model's GPU data symbol (the layouts write, writeWorld and
// writeSkinned use).
std::vector<ShapeDraw> readShapes(const Bytes& gpu);

namespace detail {

// Each bone's depth (0 for roots); fails unless the bones come parents first, breadth-first.
std::vector<int> boneDepths(const std::vector<SkinBone>& bones);
// The pose block ("pose" 19.12.06.0036) at a 16-byte boundary of `d` (symbol 1): header, 52-byte records
// and its name table; returns where it starts. See nameTable for `pool`.
uint32_t pose(Sym& d, const std::vector<SkinBone>& bones, uint32_t pool = 0, const std::vector<uint32_t>& names = {});
// A bone name table {count, ->entries {->name, 0, index}, 0, ->hashes}: a model's names and hashes in `d`
// itself, or (`pool` its symbol) an animation's, without hashes, at `names` in its string pool.
uint32_t nameTable(Sym& d, const std::vector<SkinBone>& bones, uint32_t pool = 0, const std::vector<uint32_t>& names = {});

} // namespace detail

} // namespace nut::blockmodel
