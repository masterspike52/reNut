// Writing models back: geometry from Wavefront OBJ, or a whole model copied from another one.
//
// The game builds each model's vertex and index buffer headers at load from the rendergraph's
// resource records (rendergraph+0xFC, read by 0x82BC32D0): vertex buffers {->header, ->.gpu offset,
// size} and index buffers {->header, ->.gpu offset, bytes, format}. A draw's index count is read
// when it draws. New geometry is appended to the model's .gpu part and those records updated.
#pragma once

#include "caff.hpp"
#include "model.hpp"
#include "streambundle.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace nut::model {

struct ImportReport {
    int edited = 0;  // meshes edited in place (same vertices and triangles)
    int rebuilt = 0; // meshes given new geometry
    int hidden = 0;  // meshes emptied by an object without faces
    std::vector<std::string> notes;
    // Textures the model draws (by name, split halves merged) that a file object's own image is for: its draw shows the
    // texture as it is (gain 1, offset 0), and the caller puts the image in the texture's place.
    std::vector<std::pair<std::string, Bytes>> images;
};

// Applies an OBJ to the model whose .data part is `symbol`. Objects are matched to meshes by the
// "mesh<N>" in their name, as model export writes them ("group<G>_mesh<N>"); N indexes Model::meshes.
//  - Same vertex and triangle count: the mesh is edited in place (positions, normals and the first
//    texture coordinate set); fields that did not change keep their exact bytes.
//  - Anything else replaces the mesh's geometry. Each new vertex takes its bone weights, extra
//    texture coordinate sets and other fields from the nearest original vertex of the mesh.
//  - An object without faces removes the mesh: its draw is taken out of its group, so the model's
//    later meshes number one lower when it is read again.
// Meshes not named in the file are left alone. Throws when the file names no mesh or does not fit.
ImportReport importObj(caff::Caff& c, size_t symbol, const std::string& objText);

// The same from a binary FBX file's mesh objects, named the same way. Skin weights on bones named
// like the model's (exactly, or ignoring case and underscores) replace the weights copied from the
// nearest original vertex; the fur shells FBX export adds are ignored. An object whose material is a
// plain colour (no texture) other than the game's has its draw drawn in it (c:basecolour, or texture gain 0 and offset).
// With `removeMissing`, the meshes the model draws by default that the file has no object for are removed.
ImportReport importFbx(caff::Caff& c, size_t symbol, const Bytes& fbxFile, bool removeMissing = false);
// The same from a .glb's mesh nodes (a skinned export brought back from Blender as glTF), at their rest pose.
ImportReport importGlb(caff::Caff& c, size_t symbol, const Bytes& glbFile, bool removeMissing = false);

// Only the top detail level is drawn, at every distance: each group drawn at one lower level only draws the draws of the
// top level's group in the same place (the levels' groups pair in order). Returns what it did.
std::vector<std::string> keepTopDetailLevel(caff::Caff& c, size_t symbol);

// Makes mesh `mesh` draw the model's texture `texture` (one of Model::textures) in place of its colour map. Draws that
// share the mesh's sampler record change with it; returns those meshes (the mesh among them).
std::vector<size_t> bindColourMap(caff::Caff& c, size_t symbol, size_t mesh, const std::string& texture);

// The render program (the model's .stream part) groups draws in blocks: {u16 12, op 0x15, 0, ->block end, u32 bucket
// mask}. Bit 0x8 of the mask is set on exactly the blocks of furred draws (their shells' bucket), so clearing it
// should keep the surface and drop the shells; the code reading it isn't found yet. Each block with fur on, or whose
// draws bind a fur volume in slot 1 (fur turned off), with its meshes.
struct FurBlock {
    int symbol = -1;   // the .stream symbol
    uint32_t at = 0;   // the block record's offset in it
    bool on = false;   // bit 0x8 set
    std::vector<size_t> meshes;
};
std::vector<FurBlock> furBlocks(const caff::Caff& c, size_t symbol);
// Sets or clears the fur bit of the block at `at` (FurBlock::at).
void setFur(caff::Caff& c, size_t symbol, uint32_t at, bool on);

// Rig override: moves the model's joints (its bind pose, which the game skins and animates from) to the FBX bones named
// like them. Orientations, hierarchy and bone count stay for the game's animations; unnamed bones follow their parent.
ImportReport importRig(caff::Caff& c, size_t symbol, const Bytes& fbxFile);

// OBJ, FBX or .glb, told apart by the file's content. With `rig` (FBX only), importRig first, then the meshes when the
// file has any named like the model's.
ImportReport importMeshFile(caff::Caff& c, size_t symbol, const Bytes& file, bool rig = false, bool removeMissing = false);

// Where copyModel gets a texture the destination lacks: the CAFF holding it (a whole texture, or
// the "mip" half of a split one) and, for a split texture, its streamed top level: a stream
// bundle entry ("<name>top"), which belongs in the destination's stream bundle.
struct TextureSource {
    std::shared_ptr<const caff::Caff> caff;
    size_t header = 0; // the texture's .data symbol in `caff`
    std::optional<streambundle::Entry> top;
};
// Called with a model texture name (split suffix removed); nothing when it cannot be found.
using FindTexture = std::function<std::optional<TextureSource>(const std::string& name)>;

struct CopyResult {
    std::vector<std::string> notes;                // differing skeletons, copied textures...
    std::vector<streambundle::Entry> streamEntries; // streamed top levels to add to the destination's stream bundle
};

// Makes the model whose .data part is `symbol` in `dst` a copy of the model `srcSymbol` in `src`
// (which may be the same CAFF): its .data, .gpu and .stream parts with their relocations, plus any
// pool strings and shared blobs it uses that `dst` lacks. The names stay, so the game loads the copy
// in the original's place.
//
// When a model loads, the game binds every texture it names (0x822D8078) and crashes on one that is
// not loaded. Textures that neither `dst` holds nor the original model used are copied into `dst`
// from `find`; their streamed top levels come back in CopyResult::streamEntries for the caller to
// add to the destination's stream bundle. Throws, changing nothing, when a texture cannot be found.
CopyResult copyModel(caff::Caff& dst, size_t symbol, const caff::Caff& src, size_t srcSymbol, const FindTexture& find = {});

// A FindTexture that looks in one CAFF (whole textures and mip halves) and, for streamed top
// levels, in a stream bundle file (normally the CAFF's paired one; may be empty).
FindTexture texturesIn(std::shared_ptr<const caff::Caff> caff, const fs::path& streamBundle);

// Lists the assets named by `names` (1-based symbol name indices) in the bundle's manifest, where the game finds them.
void addToManifest(caff::Caff& c, const std::vector<uint32_t>& names);

// The stream bundle paired with a bundle: Bundle/4f/<name> -> Bundle/50/<name>.
fs::path pairedStreamBundle(const fs::path& bundle);
// Adds the entries whose ids `sb` lacks; returns how many were added.
int addStreamEntries(streambundle::StreamBundle& sb, const std::vector<streambundle::Entry>& entries);

} // namespace nut::model
