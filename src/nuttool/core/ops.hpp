// Higher-level asset operations shared by the CLI and the GUI: texture pairing and
// import, unpack/pack of containers, wave bank export, and the whole-folder jobs.
#pragma once

#include "caff.hpp"
#include "common.hpp"
#include "streambundle.hpp"
#include "texture.hpp"
#include "xui.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <set>

namespace nut::ops {

// ---- naming ------------------------------------------------------------------

std::string safeName(std::string s, size_t maxLen = 96);
bool validSection(const caff::Caff& c, const caff::Symbol& s);
std::string sectionTag(const caff::Caff& c, const caff::Symbol& s);
// "#12" (1-based index) or a case-insensitive substring of the symbol name.
std::vector<size_t> selectSymbols(const caff::Caff& c, const std::string& sel, const std::string& section);

// ---- progress reporting ---------------------------------------------------------

struct Progress {
    std::function<void(const std::string&)> log;              // informational / error lines
    std::function<void(double, const std::string&)> step;     // fraction 0..1 and a label
    const std::atomic<bool>* cancel = nullptr;

    void info(const std::string& s) const { if (log) log(s); }
    void at(double f, const std::string& label) const { if (step) step(f, label); }
    bool cancelled() const { return cancel && cancel->load(); }
};

// ---- textures ------------------------------------------------------------------

struct TexRef {
    caff::Caff* caff = nullptr;
    size_t header = 0, gpu = 0; // symbol indices
    std::string name;           // base asset name
};

std::vector<TexRef> findTextures(caff::Caff& c);
// The texture whose header or GPU symbol is `symbol`, if any.
std::optional<TexRef> textureForSymbol(caff::Caff& c, size_t symbol);
// "..._0x<hash>mip" <-> "..._0x<hash>top"; empty when `n` does not end with `from`.
std::string partnerName(const std::string& n, const char* from, const char* to);
// Strips the trailing "mip"/"top" of split texture names.
std::string fullName(const std::string& n);
texture::Header asFullChain(texture::Header h);

// A texture decoded for viewing/exporting, with the streamed top level merged in when found.
struct DecodedTexture {
    texture::Header header;            // describes `levels` as a full chain when merged
    std::vector<texture::Level> levels;
    bool merged = false;
    std::string name;                  // output name (split suffix removed when merged)
};
DecodedTexture decodeTexture(const TexRef& t, const std::vector<caff::Caff*>& streams);

void writeTextureFiles(const fs::path& dir, const std::string& name, const texture::Header& h, const std::vector<texture::Level>& levels,
                       bool dds, bool png);

struct TexStats {
    int written = 0, merged = 0, skipped = 0;
};
// `filter` (lower-case) keeps only the textures whose name contains it; `seen` skips the names
// already in it and adds the ones written.
TexStats exportTextures(const std::vector<caff::Caff*>& mains, const std::vector<caff::Caff*>& streams, const fs::path& dir, bool dds, bool png,
                        std::set<std::string>* mergedTops, const Progress* progress = nullptr, const std::string& filter = {},
                        std::set<std::string>* seen = nullptr);

// An image to import into textures: decoded pixels, or a DDS whose levels are used as they are.
struct TextureImage {
    bool dds = false;
    Bytes ddsFile;
    image::Rgba rgba;
    uint32_t width = 0, height = 0;
    // Array textures: the images of slices 1, 2...; a slice without one repeats slice 0 (`rgba`).
    std::vector<image::Rgba> extraSlices;
};
TextureImage readTextureImage(const fs::path& img); // a .dds file or an image (PNG, JPG...)

// Build GPU levels for `h`/`c` from a .dds file or an image (PNG, JPG...) of the same size.
std::vector<texture::Level> levelsFromImage(const texture::Header& h, const texture::Contents& c, const fs::path& img);
// The file extensions texture import reads, as a file dialog pattern ("png;jpg;...;dds").
const char* textureImagePattern();

// Replace a texture's pixels from a .dds file or an image (PNG, JPG...). An image of another
// size resizes the texture (2D textures; the format stays). For the mip half of a split
// texture, pass the paired stream bundle so level 0 is updated too (`streamChanged` reports
// whether it was); resizing a split texture needs it. Returns a one-line summary.
std::string importTexture(caff::Caff& c, const TexRef& t, const fs::path& img, streambundle::StreamBundle* stream, bool* streamChanged);
std::string importTexture(caff::Caff& c, const TexRef& t, const TextureImage& img, streambundle::StreamBundle* stream, bool* streamChanged);

// Drop a texture's mip chain, keeping the pixels of the levels that stay as they are. A split
// texture keeps level 1 in the bundle (the game streams level 0 into a split texture, and a split
// needs at least two levels); its stream bundle is not touched. `changed` reports whether there
// was anything to remove. Returns a one-line summary.
std::string removeMipmaps(caff::Caff& c, const TexRef& t, bool* changed);
// Removes the mipmaps (as removeMipmaps) of the textures in `file` that differ from the texture of
// the same name in `original`, the file's unedited copy, or that it does not have (every texture,
// when `original` is empty). Bundles and the CAFF entries of stream bundles are handled. `file` is
// rewritten in place, after a one-time "<file>.bak" backup, when something changed. Returns the
// number of textures changed; textures that cannot be changed are reported through progress.log.
int removeChangedMipmaps(const fs::path& file, const fs::path& original, const Progress& progress);

// ---- menus -------------------------------------------------------------------------

// Every texture of `root` whose size differs from its file's .bak backup, by lower-case name.
std::map<std::string, xui::Resize> resizedTextures(const fs::path& root, const Progress& progress, const fs::path& updateDir = {});
// The bundles of `root` (and of the title update, when given) that hold menu packages (aid_xuipackage_*).
// Reads every bundle's symbols.
std::vector<fs::path> menuBundles(const fs::path& root, const Progress& progress, const fs::path& updateDir = {});

// The title update's folder for an assets folder: `explicitDir` when given, else <assets>\..\update or
// <assets>\update when it holds update bundles. TU3 brings its own menus there (aid_bundle_banjoxdlc_titleupdatecommon).
fs::path updateFolder(const fs::path& root, const fs::path& explicitDir = {});
// The title update's bundles and stream bundles: files named by type and id (4f3ef5f9, 50...), sorted.
std::vector<fs::path> updateBundles(const fs::path& updateDir);
// Rebuilds the menu packages in `bundle` from their originals (its .bak backup when there is one)
// so the elements drawing resized textures keep their on-screen size (see xui::adaptPackage). Running
// it again gives the same result. The bundle is written, after a one-time .bak backup, only when its
// packages change; returns the number of elements adjusted. Notes go to progress.log.
int adaptMenus(const fs::path& bundle, const std::map<std::string, xui::Resize>& resized, const Progress& progress);

// ---- containers --------------------------------------------------------------------

void unpackCaff(const caff::Caff& c, bool wasXctd, const fs::path& dir);
caff::Caff packCaff(const fs::path& dir, bool* wasXctd, int* changed);
std::string entryName(const streambundle::Entry& e);
std::string entryExt(const streambundle::Entry& e);
void unpackStreamBundle(const streambundle::StreamBundle& sb, const fs::path& dir);
streambundle::StreamBundle packStreamBundle(const fs::path& dir);
// The bank (<base>.xwb) and its waves (<base>/NNN_<name>.xma/.wav). With `wav`, waves are decoded
// to 16-bit WAV (kept as they are when they cannot be). Returns the number decoded.
int exportWaveBank(const Bytes& data, const fs::path& dir, const std::string& base, bool wav = false);
Bytes precacheToUtf8(const Bytes& d);
Bytes utf8ToPrecache(const std::string& text);

// ---- whole-folder jobs -----------------------------------------------------------

struct ExtractOptions {
    bool png = false;
    std::set<std::string> only; // empty = everything; else bundles,streams,text,video,fonts,ini,misc
    // Only textures, from every bundle and stream bundle, into one "textures" folder (one copy per
    // name): PNG, plus DDS with `texturesDds`.
    bool texturesOnly = false;
    bool texturesDds = false;
    // Only what is named with this text (case-insensitive): textures, bundle symbols, stream bundle
    // entries, wave banks, text tables, videos and fonts. A filtered bundle gets the matching symbols
    // as files, without the caff.json needed to pack it again.
    std::string filter;
};
// Returns the number of failures (each is reported through progress.log).
int extractAll(const fs::path& root, const fs::path& out, const ExtractOptions& opt, const Progress& progress);

struct VerifyResult {
    int passed = 0, failed = 0;
};
VerifyResult verifyAll(const fs::path& root, const Progress& progress);

} // namespace nut::ops
