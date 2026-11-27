// Part models for reNut's Lua mods: a shipped part's streamed model and collision copied under a
// new name, with new geometry, in a stream bundle of the mod's own (<mod>/bundles/Bundle/50/<id>),
// which reNut loads with the game's common bundles.
#pragma once

#include "streambundle.hpp"

#include <array>
#include <optional>

namespace nut::renutmodel {

constexpr std::string_view kModelPrefix = "aid_model_banjox_vehicleparts_";
constexpr std::string_view kHavokPrefix = "aid_havok_banjox_vehicleparts_";
constexpr uint32_t kRecordModel = 0x124;  // a part record's model asset id

// The model of a part record (its model asset id at 0x124), as a template name ("gadgets_horn"),
// looked up in `stream` (the game's common stream bundle); nothing when it isn't there.
std::optional<std::string> recordModel(const fs::path& stream, const Bytes& record);

struct Built {
    std::vector<streambundle::Entry> entries;  // the model and its collision
    std::vector<std::string> notes;
};

// A .glb mesh file: the node taken, and the template's meshes it goes into (the others are hidden).
struct GlbMesh {
    std::string node;
    std::vector<int> into;
    std::vector<float> flat;  // u, v (and u2, v2): one flat colour from these texture spots
    bool matte = false;       // the material's sheen (specular, velvet, reflection) turned off
};

// The template's model and collision from `stream`, renamed to `id`, with `meshFile` (OBJ or FBX,
// objects named like model-export writes them: group0_mesh3; or a .glb with `glb`) applied to the
// model. The collision is the template's, or `collision`'s (another part model, such as body_light_wedge),
// or none with "none".
Built build(const fs::path& stream, const std::string& templateName, const std::string& id, const Bytes& meshFile,
            const GlbMesh& glb = {}, const std::string& collision = {});

constexpr std::string_view kAttachPrefix = "aid_avatarhavokdata_banjox_vehicleblock_";
constexpr uint32_t kRecordAttach = 0x12C;  // a part record's footprint asset id

// The footprint of a part record (its cells and attach points, the asset at 0x12C) from `stream`,
// renamed to `id`, with the faces `attachable` names able to take other parts: "all", "none",
// "top", "bottom", "sides", "top+sides", or a list of directions such as "-x,-y,+z"; the others can't.
streambundle::Entry buildFootprint(const fs::path& stream, const Bytes& record, const std::string& id,
                                   const std::string& attachable, std::string* note);

// The faces an --attachable rule opens, in the face records' order (+y, +z, -y, -z, -x, +x).
std::array<bool, 6> faceRule(const std::string& attachable);

// A one-cell footprint made from scratch, `id`'s own: its faces in `attachable` take other parts,
// and it collides as `shape` ("cube", "wedge" or "corner", as the game's body blocks do).
streambundle::Entry newFootprint(const std::string& id, const std::string& shape, const std::string& attachable, std::string* note);

// The mod's stream bundle: <mod>/bundles/Bundle/50/<id of aid_streambundle_banjox_<bundle>>.
fs::path modBundlePath(const fs::path& mod, const std::string& bundle);

// Adds `entries` to the mod's stream bundle (replacing those with the same ids), made when missing.
// Refused when `bundle` gives the id of one of the game's (`gameStream`'s folder holds them; empty:
// not checked).
fs::path addToMod(const fs::path& mod, const std::string& bundle, const std::vector<streambundle::Entry>& entries,
                  const fs::path& gameStream);

} // namespace nut::renutmodel
