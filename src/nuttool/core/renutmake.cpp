#include "renutmake.hpp"

#include "assetdb.hpp"
#include "blockmodel.hpp"
#include "container.hpp"
#include "gltfobj.hpp"
#include "image.hpp"
#include "modelimport.hpp"
#include "renutworld.hpp"

#include <algorithm>
#include <cstdio>

namespace nut::renutmake {

std::string defaultBundle(const fs::path& mod) {
    std::string bundle;
    for (char ch : lower(fs::absolute(mod).filename().string())) bundle += (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ? ch : '_';
    return bundle;
}

bool addPartAssets(const fs::path& mod, const fs::path& stream, renutpart::Part& part, const PartAssets& assets, bool force,
                   const Say& say) {
    if (!renutpart::validId(part.id)) fail("a part id is 1-40 characters of a-z, 0-9 and _ (got '" + part.id + "')");
    std::vector<streambundle::Entry> entries;
    if (!assets.model.empty()) {
        std::string tpl = lower(assets.templateName);
        if (startsWith(tpl, renutmodel::kModelPrefix)) tpl.erase(0, renutmodel::kModelPrefix.size());
        if (tpl.empty()) {
            auto m = renutmodel::recordModel(stream, part.original);
            if (!m) fail(part.base + "'s model is not in " + stream.string() + "; name one with --template");
            tpl = *m;
        }
        renutmodel::Built b = renutmodel::build(stream, tpl, part.id, readFile(assets.model), assets.glb, assets.collision);
        for (auto& n : b.notes) say(n);
        entries = b.entries;
        part.model = part.id;
    }
    if (!assets.attachable.empty()) {
        std::string note;
        entries.push_back(renutmodel::buildFootprint(stream, part.original, part.id, assets.attachable, &note));
        say(note);
        part.attach = part.id;
    }
    std::string bundle = assets.bundle.empty() ? defaultBundle(mod) : lower(assets.bundle);
    if (!renutpart::validId(bundle)) fail("--bundle is 1-40 characters of a-z, 0-9 and _");
    say("wrote " + renutmodel::addToMod(mod, bundle, entries, stream).string());
    // Rebuilding a part's model or footprint keeps the script written for it.
    fs::path script = mod / "scripts" / (part.id + ".lua");
    if (fs::exists(longPath(script)) && !force) {
        say("kept " + script.string() + " (--force writes it anew); it uses them with" +
            (part.model.empty() ? std::string() : " model = \"" + part.model + "\"") +
            (part.attach.empty() ? std::string() : " attach = \"" + part.attach + "\""));
        return false;
    }
    return true;
}

void block(const fs::path& mod, const Block& b, bool force, const Say& say) {
    const std::string& id = b.id;
    if (!renutpart::validId(id)) fail("a block id is 1-40 characters of a-z, 0-9 and _ (got '" + id + "')");
    if (b.model.empty() || b.mesh.empty()) fail("a block needs a .glb model and the node whose mesh it takes");
    const std::string shape = b.shape.empty() ? "cube" : b.shape;
    const std::string attachable = b.attachable.empty() ? "all" : b.attachable;
    const std::string category = b.category.empty() ? "body" : lower(b.category);

    // Its model, from scratch: the node's mesh, drawn by reNut's own part shaders.
    const gltfobj::MeshData src = gltfobj::mesh(readFile(b.model), b.mesh);
    blockmodel::Mesh mesh;
    for (size_t i = 0; i < src.positions.size(); ++i) mesh.vertices.push_back({src.positions[i], src.normals[i]});
    mesh.indices = src.indices;
    mesh.shapes = src.shapes;
    const std::string modelName = "aid_model_banjox_vehicleparts_" + id;
    auto modelId = assetdb::idFromName(modelName);
    if (!modelId) fail("no asset id for " + modelName);
    // Its shape keys: a table of its own, its model's.
    blockmodel::ShapeTable shapes{*modelId, src.shapeNames};
    std::vector<streambundle::Entry> entries;
    entries.push_back({*modelId, blockmodel::write(id, mesh, src.shapeNames.empty() ? nullptr : &shapes)});
    say(modelName + ": " + std::to_string(mesh.vertices.size()) + " vertices, " + std::to_string(mesh.indices.size() / 3) + " triangles" +
        (src.shapeNames.empty() ? std::string() : ", " + std::to_string(src.shapeNames.size()) + " shape key(s)"));
    std::string note;
    entries.push_back(renutmodel::newFootprint(id, shape, attachable, &note));
    say(note);
    std::string bundle = b.bundle.empty() ? defaultBundle(mod) : lower(b.bundle);
    if (!renutpart::validId(bundle)) fail("--bundle is 1-40 characters of a-z, 0-9 and _");
    say("wrote " + renutmodel::addToMod(mod, bundle, entries, {}).string());

    auto quoted = [](const std::string& s) {
        std::string out = "\"";
        for (char ch : s) out += ch == '"' || ch == '\\' ? std::string("\\") + ch : std::string(1, ch);
        return out + "\"";
    };
    // Its shape keys' table, made anew each time: their names and starting weights.
    if (!src.shapeNames.empty()) {
        std::string names, weights;
        for (size_t i = 0; i < src.shapeNames.size(); ++i) {
            const auto set = b.shapeWeights.find(src.shapeNames[i]);
            const float weight = set != b.shapeWeights.end() ? set->second : i < src.shapeWeights.size() ? src.shapeWeights[i] : 0.0f;
            char number[32];
            std::snprintf(number, sizeof number, "%g", double(weight));
            names += (i ? ", " : "") + quoted(src.shapeNames[i]);
            weights += (i ? ", " : "") + std::string(number);
        }
        for (const auto& [name, weight] : b.shapeWeights)
            if (std::find(src.shapeNames.begin(), src.shapeNames.end(), name) == src.shapeNames.end())
                say("warning: no shape key \"" + name + "\" to set");
        const fs::path shapesScript = mod / "scripts" / (id + "_shapes.lua");
        fs::create_directories(longPath(shapesScript.parent_path()));
        writeText(shapesScript, "-- The shape keys of the block " + id + ", from " + b.model.filename().string() +
                                    ": written anew by renut-block each time.\n-- These start every one of it; part:set_shape(name, weight) "
                                    "sets one part's own.\n\nrenut.shapes.define{\n  model = " +
                                    quoted("banjox_vehicleparts_" + id) + ",\n  shapes = {" + names + "},\n  weights = {" + weights + "},\n}\n");
        say("wrote " + shapesScript.string());
    }

    fs::path script = mod / "scripts" / (id + ".lua");
    if (fs::exists(longPath(script)) && !force) {
        say("kept " + script.string() + " (--force writes it anew)");
        return;
    }
    const std::string name = b.name.empty() ? id : b.name;
    std::string text = "-- " + name + ": a block of its own, made with NutTool from " + b.model.filename().string() + " (" + b.mesh +
                       ").\n-- reNut's docs/tutorials/parts-and-vehicles.md describes renut.parts.add.\n\n";
    text += "renut.parts.add{\n";
    text += "  id = " + quoted(id) + ",\n";
    text += "  name = " + quoted(name) + ",\n";
    text += "  category = " + quoted(category) + ",            -- the garage category it is listed in\n";
    if (!b.type.empty()) text += "  type = " + quoted(b.type) + ",                -- the garage type it shares with blocks of that type\n";
    text += "  count = " + (b.count.empty() ? std::string("99") : b.count) + ",\n";
    text += "  mass = " + (b.mass.empty() ? std::string("10") : b.mass) + ",\n";
    text += "  health = " + (b.health.empty() ? std::string("5") : b.health) + ",\n";
    text += "  model = " + quoted(id) + ",                  -- its model and footprint, in the mod's bundles folder\n";
    text += "  attach = " + quoted(id) + ",\n";
    text += "}\n";
    fs::create_directories(longPath(script.parent_path()));
    writeText(script, text);
    say("wrote " + script.string());
    fs::path toml = mod / "mod.toml";
    if (!fs::exists(longPath(toml)))
        writeText(toml, "name = " + quoted(mod.filename().string()) + "\ndescription = \"Blocks made with NutTool.\"\nversion = \"1\"\n");
}

void playerModel(const fs::path& assets, const fs::path& mod, const fs::path& file, const std::string& id,
                 const std::string& templateName, const std::string& bundleName, const Say& say) {
    if (!renutpart::validId(id)) fail("a player model id is 1-40 characters of a-z, 0-9 and _ (got '" + id + "')");
    const Bytes in = readFile(file);
    std::string tpl = lower(templateName);
    if (startsWith(tpl, "aid_model_banjox_")) tpl.erase(0, 17);
    if (tpl.empty()) {
        // A NutTool export's root is named after its model: an FBX object name ends "\0\1Model", a glTF one is quoted.
        for (const char* name : {"actor_banjotuxedo", "actor_banjo"})
            for (const std::string& key : {std::string(name) + std::string("\0\1Model", 7), "\"" + std::string(name) + "\""})
                if (tpl.empty() && std::search(in.begin(), in.end(), key.begin(), key.end()) != in.end()) tpl = name;
        if (tpl.empty())
            fail(file.string() + " doesn't name the Banjo model it was exported from: give --template actor_banjo or actor_banjotuxedo");
    }
    const std::string from = "aid_model_banjox_" + tpl, to = "aid_model_renut_player_" + id;
    const fs::path common = assets / "Bundle" / "4f" / "685374";
    say("reading " + from + " from " + common.string() + " (the common bundle)");
    Loaded l = loadAsset(common);
    if (!l.caff) fail(common.string() + " is not the game's common bundle (is the assets folder the game's?)");
    const caff::Caff& src = *l.caff;
    // Its .data part first (the model), then .gpu and .stream.
    std::vector<size_t> syms;
    for (const char* section : {".data", ".gpu", ".stream"})
        for (size_t i = 0; i < src.symbols.size(); ++i) {
            const uint8_t s = src.symbols[i].section;
            if (s && s <= src.sections.size() && src.sections[s - 1].name == section && assetdb::baseAssetName(src.symbolName(i)) == from) {
                syms.push_back(i);
                break;
            }
        }
    if (syms.empty() || src.sections[src.symbols[syms[0]].section - 1].name != ".data") fail(from + " is not in the common bundle");
    caff::Caff c = renutworld::copyAssets(src, syms);
    renutworld::renameAssetIn(c, from, to);
    // The file is the whole model: what it left out of what the model draws by default is removed.
    const model::ImportReport r = model::importMeshFile(c, 0, in, false, true);
    say(to + ": " + from + " with " + file.filename().string() + "'s meshes (" + std::to_string(r.edited) + " edited in place, " +
        std::to_string(r.rebuilt) + " rebuilt, " + std::to_string(r.hidden) + " removed)");
    for (const std::string& n : r.notes) say("  " + n);
    // An export from Blender edits the top detail level: it's drawn at every distance.
    for (const std::string& n : model::keepTopDetailLevel(c, 0)) say("  " + n);
    // Objects with an image of their own: the texture their draw shows, copied as the player model's with the image in it.
    std::vector<streambundle::Entry> textures;
    if (!r.images.empty()) {
        const fs::path tops = model::pairedStreamBundle(common);
        std::map<std::string, std::string> names;
        int n = 0;
        for (const auto& [tex, file] : r.images) {
            const image::Rgba img = image::decodeImage(file);
            if (!img.width || !img.height) {
                say("  " + tex + ": the file's image for it can't be read, so the game's stays");
                continue;
            }
            const std::string copy = "aid_texture_renut_player_" + id + "_" + std::to_string(++n);
            std::string summary;
            for (auto& e : renutworld::textureCopy(src, tops, tex, copy, img, 1024, names, summary)) textures.push_back(std::move(e));
            say("  " + copy + ": " + tex + "'s copy showing the file's image (" + summary + ")");
        }
        if (!names.empty()) {
            try {
                const int pointed = renutworld::retextureIn(c, 0, names);
                say("  " + std::to_string(pointed) + " texture name(s) pointed at the copies (every draw using one shows its image)");
            } catch (const std::exception&) {
                fail(to + " names none of the textures the file's images are for");
            }
        }
    }
    const auto aid = assetdb::idFromName(to);
    if (!aid) fail("no asset id for " + to);
    const std::string bundle = bundleName.empty() ? defaultBundle(mod) : lower(bundleName);
    if (!renutpart::validId(bundle)) fail("--bundle is 1-40 characters of a-z, 0-9 and _");
    std::vector<streambundle::Entry> entries{{*aid, c.serialize()}};
    entries.insert(entries.end(), textures.begin(), textures.end());
    say("wrote " + renutmodel::addToMod(mod, bundle, entries, {}).string());
    say("Lua: renut.player.set_model(\"" + id + "\"), or player_model = \"" + id + "\" in renut.worlds.add");
}

} // namespace nut::renutmake
