#include "script.hpp"

#if NUTTOOL_LUA

#include "assetdb.hpp"
#include "commands.hpp"
#include "container.hpp"
#include "media.hpp"
#include "model.hpp"
#include "modelimport.hpp"
#include "ops.hpp"

// Lua is built as C++ (vcpkg lua[cpp]): its errors unwind as exceptions, so destructors run.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace nut::script {
namespace {

struct Context {
    const Options* opt = nullptr;
};

Context& context(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "nut.context");
    auto* c = static_cast<Context*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *c;
}

void out(lua_State* L, const std::string& s) {
    auto& c = context(L);
    if (c.opt->output) c.opt->output(s);
}

// Calls `f`, turning C++ exceptions into Lua errors.
template <class F> int guarded(lua_State* L, F&& f) {
    std::string err;
    try {
        return f();
    } catch (std::exception& e) {
        err = e.what();
    }
    lua_pushstring(L, err.c_str());
    return lua_error(L);
}

// A path from a script: absolute; or relative to the assets folder when it exists there, else to
// the script's folder (when the script is a file), else to the current folder.
fs::path resolve(lua_State* L, const char* p) {
    fs::path path = fs::path(std::u8string(reinterpret_cast<const char8_t*>(p)));
    if (path.is_absolute()) return path;
    const Options& opt = *context(L).opt;
    if (!opt.assets.empty() && fs::exists(longPath(opt.assets / path))) return opt.assets / path;
    if (!opt.scriptDir.empty()) return opt.scriptDir / path;
    return path;
}

std::string utf8(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

Bytes stringToBytes(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    return Bytes(reinterpret_cast<const uint8_t*>(s), reinterpret_cast<const uint8_t*>(s) + n);
}

// ---- print / nut.* functions ------------------------------------------------------------------

int l_print(lua_State* L) {
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    out(L, line + "\n");
    return 0;
}

// nut.run(command, args...) -> exit code, output
int l_run(lua_State* L) {
    std::vector<std::string> args;
    for (int i = 1; i <= lua_gettop(L); ++i) args.push_back(luaL_checkstring(L, i));
    std::string text;
    int code;
    {
        cli::Sink sink = [&](const std::string& s) { text += s; };
        cli::SinkScope scope(sink);
        code = cli::run(args);
    }
    lua_pushinteger(L, code);
    lua_pushlstring(L, text.data(), text.size());
    return 2;
}

int l_exists(lua_State* L) {
    lua_pushboolean(L, fs::exists(longPath(resolve(L, luaL_checkstring(L, 1)))));
    return 1;
}

int l_read(lua_State* L) {
    return guarded(L, [&] {
        Bytes b = readFile(resolve(L, luaL_checkstring(L, 1)));
        lua_pushlstring(L, reinterpret_cast<const char*>(b.data()), b.size());
        return 1;
    });
}

int l_write(lua_State* L) {
    return guarded(L, [&] {
        fs::path p = resolve(L, luaL_checkstring(L, 1));
        Bytes b = stringToBytes(L, 2);
        if (p.has_parent_path()) fs::create_directories(longPath(p.parent_path()));
        writeFile(p, b);
        return 0;
    });
}

// nut.list(dir [, recursive]) -> paths (relative to dir), files only
int l_list(lua_State* L) {
    return guarded(L, [&] {
        fs::path dir = resolve(L, luaL_optstring(L, 1, "."));
        bool rec = lua_toboolean(L, 2);
        std::vector<std::string> out2;
        if (rec) {
            for (auto& e : fs::recursive_directory_iterator(longPath(dir)))
                if (e.is_regular_file()) out2.push_back(utf8(fs::relative(e.path(), longPath(dir))));
        } else {
            for (auto& e : fs::directory_iterator(longPath(dir)))
                if (e.is_regular_file()) out2.push_back(utf8(e.path().filename()));
        }
        std::sort(out2.begin(), out2.end());
        lua_createtable(L, int(out2.size()), 0);
        for (size_t i = 0; i < out2.size(); ++i) {
            lua_pushstring(L, out2[i].c_str());
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    });
}

int l_hash(lua_State* L) {
    auto id = assetdb::idFromName(luaL_checkstring(L, 1));
    if (!id) return luaL_error(L, "not an asset name");
    lua_pushinteger(L, lua_Integer(*id));
    return 1;
}

// nut.wavebank(data) -> {name, version, waves = {{name, format, channels, rate, bits, samples, seconds, loop_start, loop_length, size}}}
// or nil when `data` is not a wave bank
int l_wavebank(lua_State* L) {
    Bytes d = stringToBytes(L, 1);
    if (!media::isWaveBank(d)) {
        lua_pushnil(L);
        return 1;
    }
    return guarded(L, [&] {
        media::WaveBank wb = media::parseWaveBank(d);
        static const char* formats[4] = {"pcm", "xma", "adpcm", "wma"};
        lua_createtable(L, 0, 3);
        lua_pushstring(L, wb.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, wb.version);
        lua_setfield(L, -2, "version");
        lua_createtable(L, int(wb.waves.size()), 0);
        for (size_t i = 0; i < wb.waves.size(); ++i) {
            const auto& w = wb.waves[i];
            lua_createtable(L, 0, 10);
            lua_pushstring(L, w.name.c_str());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, formats[w.formatTag & 3]);
            lua_setfield(L, -2, "format");
            lua_pushinteger(L, w.channels);
            lua_setfield(L, -2, "channels");
            lua_pushinteger(L, w.sampleRate);
            lua_setfield(L, -2, "rate");
            lua_pushinteger(L, w.bitsPerSample);
            lua_setfield(L, -2, "bits");
            lua_pushinteger(L, w.durationSamples);
            lua_setfield(L, -2, "samples");
            lua_pushnumber(L, w.sampleRate ? double(w.durationSamples) / w.sampleRate : 0.0);
            lua_setfield(L, -2, "seconds");
            lua_pushinteger(L, w.loopStart);
            lua_setfield(L, -2, "loop_start");
            lua_pushinteger(L, w.loopLength);
            lua_setfield(L, -2, "loop_length");
            lua_pushinteger(L, lua_Integer(w.data.size()));
            lua_setfield(L, -2, "size");
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        lua_setfield(L, -2, "waves");
        return 1;
    });
}

// ---- File objects -------------------------------------------------------------------------------

constexpr const char* kFile = "nut.File";

struct File {
    fs::path path;
    Loaded l;
    bool changed = false;
};

File& checkFile(lua_State* L, int idx = 1) {
    File* f = *static_cast<File**>(luaL_checkudata(L, idx, kFile));
    if (!f) luaL_error(L, "the file is closed");
    return *f;
}

caff::Caff& checkBundle(lua_State* L, File& f) {
    if (!f.l.caff) luaL_error(L, "%s is not a bundle", utf8(f.path).c_str());
    return *f.l.caff;
}

int l_open(lua_State* L) {
    // A File holds a whole asset (often tens of MB) but looks tiny to Lua's collector: free the
    // ones no longer referenced before loading another, so loops over many files stay small.
    lua_gc(L, LUA_GCCOLLECT);
    return guarded(L, [&] {
        auto* f = new File;
        f->path = resolve(L, luaL_checkstring(L, 1));
        try {
            f->l = loadAsset(f->path);
        } catch (...) {
            delete f;
            throw;
        }
        auto** ud = static_cast<File**>(lua_newuserdatauv(L, sizeof(File*), 0));
        *ud = f;
        luaL_setmetatable(L, kFile);
        return 1;
    });
}

// f:close() (also __gc and __close, for `local f <close> = nut.open(...)`): frees the asset now.
int f_gc(lua_State* L) {
    auto** ud = static_cast<File**>(luaL_checkudata(L, 1, kFile));
    delete *ud;
    *ud = nullptr;
    return 0;
}

int f_tostring(lua_State* L) {
    File* f = *static_cast<File**>(luaL_checkudata(L, 1, kFile));
    if (!f) lua_pushstring(L, "nut.File(closed)");
    else lua_pushfstring(L, "nut.File(%s, %s)", utf8(f->path).c_str(), kindName(f->l.kind));
    return 1;
}

int f_kind(lua_State* L) {
    File& f = checkFile(L);
    lua_pushstring(L, f.l.caff ? "bundle" : f.l.sb ? "streambundle" : kindName(f.l.kind));
    return 1;
}

int f_path(lua_State* L) {
    lua_pushstring(L, utf8(checkFile(L).path).c_str());
    return 1;
}

// f:symbols() -> {{index, name, asset, section, size}, ...}
int f_symbols(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    lua_createtable(L, int(c.symbols.size()), 0);
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, lua_Integer(i + 1));
        lua_setfield(L, -2, "index");
        lua_pushstring(L, c.symbolName(i).c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, assetdb::baseAssetName(c.symbolName(i)).c_str());
        lua_setfield(L, -2, "asset");
        lua_pushstring(L, c.sectionOf(c.symbols[i]).name.c_str());
        lua_setfield(L, -2, "section");
        lua_pushinteger(L, c.symbols[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

// f:find(name [, section]) -> index or nil; matches the symbol name or its asset name
int f_find(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    std::string name = luaL_checkstring(L, 2);
    const char* sec = luaL_optstring(L, 3, nullptr);
    for (size_t i = 0; i < c.symbols.size(); ++i) {
        if (sec && c.sectionOf(c.symbols[i]).name != sec) continue;
        if (c.symbolName(i) == name || assetdb::baseAssetName(c.symbolName(i)) == name) {
            lua_pushinteger(L, lua_Integer(i + 1));
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

size_t symbolArg(lua_State* L, caff::Caff& c, int idx) {
    lua_Integer i = luaL_checkinteger(L, idx);
    if (i < 1 || size_t(i) > c.symbols.size()) luaL_error(L, "no symbol %d", int(i));
    return size_t(i - 1);
}

int f_read(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    Bytes b = c.symbolData(symbolArg(L, c, 2));
    lua_pushlstring(L, reinterpret_cast<const char*>(b.data()), b.size());
    return 1;
}

int f_write(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    size_t s = symbolArg(L, c, 2);
    Bytes b = stringToBytes(L, 3);
    return guarded(L, [&] {
        c.replaceSymbolData(s, b);
        f.changed = true;
        return 0;
    });
}

std::vector<size_t> modelSymbols(caff::Caff& c) {
    std::vector<size_t> out2;
    for (size_t i = 0; i < c.symbols.size(); ++i)
        if (model::isModelSymbol(c, i)) out2.push_back(i);
    return out2;
}

size_t modelArg(lua_State* L, caff::Caff& c, int idx) {
    std::string name = luaL_checkstring(L, idx);
    for (size_t s : modelSymbols(c)) {
        std::string n = assetdb::baseAssetName(c.symbolName(s));
        if (n == name || n == "aid_model_" + name) return s;
    }
    luaL_error(L, "no model '%s'", name.c_str());
    return 0;
}

int f_models(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    auto ms = modelSymbols(c);
    lua_createtable(L, int(ms.size()), 0);
    for (size_t i = 0; i < ms.size(); ++i) {
        lua_pushstring(L, assetdb::baseAssetName(c.symbolName(ms[i])).c_str());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

int f_textures(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    auto ts = ops::findTextures(c);
    lua_createtable(L, int(ts.size()), 0);
    for (size_t i = 0; i < ts.size(); ++i) {
        lua_pushstring(L, ts[i].name.c_str());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

void pushStrings(lua_State* L, const std::vector<std::string>& v) {
    lua_createtable(L, int(v.size()), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        lua_pushstring(L, v[i].c_str());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
}

// f:model(name) -> summary table
int f_model(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    size_t s = modelArg(L, c, 2);
    return guarded(L, [&] {
        model::Model m = model::parse(c, s);
        size_t verts = 0, tris = 0;
        for (auto& me : m.meshes) verts += me.vertices.size(), tris += me.indices.size() / 3;
        lua_createtable(L, 0, 10);
        lua_pushstring(L, m.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, lua_Integer(m.meshes.size()));
        lua_setfield(L, -2, "meshes");
        lua_pushinteger(L, lua_Integer(verts));
        lua_setfield(L, -2, "vertices");
        lua_pushinteger(L, lua_Integer(tris));
        lua_setfield(L, -2, "triangles");
        lua_pushinteger(L, lua_Integer(m.groups.size()));
        lua_setfield(L, -2, "groups");
        lua_pushinteger(L, lua_Integer(m.nodes.size()));
        lua_setfield(L, -2, "nodes");
        lua_pushinteger(L, lua_Integer(m.switches.size()));
        lua_setfield(L, -2, "switches");
        std::vector<std::string> bones;
        for (auto& b : m.bones) bones.push_back(b.name);
        pushStrings(L, bones);
        lua_setfield(L, -2, "bones");
        pushStrings(L, m.textures);
        lua_setfield(L, -2, "textures");
        pushStrings(L, m.materials);
        lua_setfield(L, -2, "materials");
        return 1;
    });
}

// f:colours(model) -> {{material, constant, r, g, b, a, uses}, ...}: colour constants, one entry
// per distinct material + constant + value (as the GUI groups them)
int f_colours(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    size_t s = modelArg(L, c, 2);
    return guarded(L, [&] {
        model::Model m = model::parse(c, s);
        struct Group {
            std::string material, name;
            std::array<float, 4> v;
            int uses = 0;
        };
        std::vector<Group> groups;
        for (auto& me : m.meshes)
            for (auto& k : me.constants) {
                if (k.symbol < 0 || lower(k.name).find("colour") == std::string::npos) continue;
                auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.material == me.material && g.name == k.name && g.v == k.value; });
                if (it == groups.end()) groups.push_back({me.material, k.name, k.value, 1});
                else ++it->uses;
            }
        lua_createtable(L, int(groups.size()), 0);
        for (size_t i = 0; i < groups.size(); ++i) {
            const Group& g = groups[i];
            lua_createtable(L, 0, 7);
            lua_pushstring(L, g.material.c_str());
            lua_setfield(L, -2, "material");
            lua_pushstring(L, g.name.c_str());
            lua_setfield(L, -2, "constant");
            const char* comp[4] = {"r", "g", "b", "a"};
            for (int q = 0; q < 4; ++q) {
                lua_pushnumber(L, g.v[size_t(q)]);
                lua_setfield(L, -2, comp[q]);
            }
            lua_pushinteger(L, g.uses);
            lua_setfield(L, -2, "uses");
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    });
}

// f:set_colour(model, material, constant, r, g, b [, a]) -> uses changed. `constant` may omit "c:".
int f_set_colour(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    size_t s = modelArg(L, c, 2);
    std::string material = luaL_checkstring(L, 3), constant = luaL_checkstring(L, 4);
    if (!startsWith(constant, "c:")) constant = "c:" + constant;
    float r = float(luaL_checknumber(L, 5)), g = float(luaL_checknumber(L, 6)), b = float(luaL_checknumber(L, 7));
    bool hasA = !lua_isnoneornil(L, 8);
    float a = hasA ? float(luaL_checknumber(L, 8)) : 0;
    return guarded(L, [&] {
        model::Model m = model::parse(c, s);
        std::map<int, Bytes> data;
        int uses = 0;
        for (auto& me : m.meshes)
            for (auto& k : me.constants) {
                if (me.material != material || k.name != constant || k.symbol < 0) continue;
                Bytes& d = data.try_emplace(k.symbol, c.symbolData(size_t(k.symbol))).first->second;
                float v[4] = {r, g, b, hasA ? a : k.value[3]};
                for (int q = 0; q < 4; ++q) {
                    uint32_t bits;
                    std::memcpy(&bits, &v[q], 4);
                    putBe32(&d[k.offset + 4 * uint32_t(q)], bits);
                }
                ++uses;
            }
        for (auto& [sym, d] : data) c.replaceSymbolData(size_t(sym), d);
        if (uses) f.changed = true;
        lua_pushinteger(L, uses);
        return 1;
    });
}

// f:import_obj(model, obj or fbx path) -> {edited, rebuilt, hidden, notes}
int f_import_obj(lua_State* L) {
    File& f = checkFile(L);
    caff::Caff& c = checkBundle(L, f);
    size_t s = modelArg(L, c, 2);
    fs::path obj = resolve(L, luaL_checkstring(L, 3));
    return guarded(L, [&] {
        model::ImportReport r = model::importMeshFile(c, s, readFile(obj));
        f.changed = true;
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, r.edited);
        lua_setfield(L, -2, "edited");
        lua_pushinteger(L, r.rebuilt);
        lua_setfield(L, -2, "rebuilt");
        lua_pushinteger(L, r.hidden);
        lua_setfield(L, -2, "hidden");
        pushStrings(L, r.notes);
        lua_setfield(L, -2, "notes");
        return 1;
    });
}

// Stream bundles: f:entries() -> {{index, id, size}}, f:entry(i) -> bytes, f:set_entry(i, bytes)
int f_entries(lua_State* L) {
    File& f = checkFile(L);
    if (!f.l.sb) return luaL_error(L, "%s is not a stream bundle", utf8(f.path).c_str());
    auto& e = f.l.sb->entries;
    lua_createtable(L, int(e.size()), 0);
    for (size_t i = 0; i < e.size(); ++i) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, lua_Integer(i + 1));
        lua_setfield(L, -2, "index");
        lua_pushinteger(L, e[i].id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, lua_Integer(e[i].data.size()));
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

size_t entryArg(lua_State* L, File& f, int idx) {
    if (!f.l.sb) luaL_error(L, "%s is not a stream bundle", utf8(f.path).c_str());
    lua_Integer i = luaL_checkinteger(L, idx);
    if (i < 1 || size_t(i) > f.l.sb->entries.size()) luaL_error(L, "no entry %d", int(i));
    return size_t(i - 1);
}

int f_entry(lua_State* L) {
    File& f = checkFile(L);
    size_t i = entryArg(L, f, 2);
    const Bytes& b = f.l.sb->entries[i].data;
    lua_pushlstring(L, reinterpret_cast<const char*>(b.data()), b.size());
    return 1;
}

// f:entry_name(i) -> the entry's asset name ("wavebank_DialogJamJars", "aid_texture_..."), "" if unknown
int f_entry_name(lua_State* L) {
    File& f = checkFile(L);
    size_t i = entryArg(L, f, 2);
    return guarded(L, [&] {
        lua_pushstring(L, ops::entryName(f.l.sb->entries[i]).c_str());
        return 1;
    });
}

int f_set_entry(lua_State* L) {
    File& f = checkFile(L);
    size_t i = entryArg(L, f, 2);
    f.l.sb->entries[i].data = stringToBytes(L, 3);
    f.changed = true;
    return 0;
}

// f:save([path]) -> path written. In place, the original is backed up as <file>.bak once.
int f_save(lua_State* L) {
    File& f = checkFile(L);
    const char* to = luaL_optstring(L, 2, nullptr);
    return guarded(L, [&] {
        fs::path out2 = to ? resolve(L, to) : prepareOutput(f.path, "");
        if (f.l.caff) saveCaff(out2, *f.l.caff, f.l.xctd);
        else if (f.l.sb) writeFile(out2, f.l.sb->serialize());
        else writeFile(out2, f.l.raw);
        f.changed = false;
        lua_pushstring(L, utf8(out2).c_str());
        return 1;
    });
}

void stopHook(lua_State* L, lua_Debug*) {
    auto& c = context(L);
    if (c.opt->cancel && c.opt->cancel->load()) luaL_error(L, "stopped");
}

} // namespace

bool available() { return true; }

bool run(const std::string& code, const std::string& chunkName, const Options& opt) {
    lua_State* L = luaL_newstate();
    if (!L) {
        if (opt.output) opt.output("error: out of memory\n");
        return false;
    }
    Context ctx{&opt};
    lua_pushlightuserdata(L, &ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "nut.context");
    luaL_openlibs(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    // File metatable
    luaL_newmetatable(L, kFile);
    static const luaL_Reg fileMethods[] = {
        {"kind", f_kind},         {"path", f_path},           {"symbols", f_symbols},   {"find", f_find},
        {"read", f_read},         {"write", f_write},         {"models", f_models},     {"textures", f_textures},
        {"model", f_model},       {"colours", f_colours},     {"set_colour", f_set_colour}, {"import_obj", f_import_obj},
        {"entries", f_entries},   {"entry", f_entry},         {"entry_name", f_entry_name}, {"set_entry", f_set_entry},
        {"save", f_save},         {"close", f_gc},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    luaL_setfuncs(L, fileMethods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, f_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, f_gc);
    lua_setfield(L, -2, "__close");
    lua_pushcfunction(L, f_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    static const luaL_Reg nutFunctions[] = {
        {"run", l_run}, {"open", l_open}, {"exists", l_exists}, {"read", l_read}, {"write", l_write}, {"list", l_list}, {"hash", l_hash}, {"wavebank", l_wavebank}, {nullptr, nullptr},
    };
    lua_newtable(L);
    luaL_setfuncs(L, nutFunctions, 0);
    lua_pushstring(L, utf8(opt.assets).c_str());
    lua_setfield(L, -2, "assets");
    lua_pushstring(L, utf8(opt.scriptDir).c_str());
    lua_setfield(L, -2, "script_dir");
    lua_pushstring(L, "0.2.0");
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "nut");

    lua_createtable(L, int(opt.args.size()), 1);
    lua_pushstring(L, chunkName.c_str());
    lua_rawseti(L, -2, 0);
    for (size_t i = 0; i < opt.args.size(); ++i) {
        lua_pushstring(L, opt.args[i].c_str());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    lua_setglobal(L, "arg");

    if (opt.cancel) lua_sethook(L, stopHook, LUA_MASKCOUNT, 10000);

    bool ok = true;
    try {
        if (luaL_loadbuffer(L, code.data(), code.size(), ("@" + chunkName).c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            if (opt.output) opt.output(std::string("error: ") + (msg ? msg : "?") + "\n");
            ok = false;
        }
    } catch (std::exception& e) {
        if (opt.output) opt.output(std::string("error: ") + e.what() + "\n");
        ok = false;
    }
    lua_close(L);
    return ok;
}

} // namespace nut::script

#else

namespace nut::script {
bool available() { return false; }
bool run(const std::string&, const std::string&, const Options& opt) {
    if (opt.output) opt.output("error: this build of NutTool has no Lua (vcpkg port \"lua\" with feature \"cpp\")\n");
    return false;
}
} // namespace nut::script

#endif
