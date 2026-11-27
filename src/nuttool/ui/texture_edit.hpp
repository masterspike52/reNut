// Texture changes across every copy of a catalog texture: made in memory, then written together.
#pragma once

#include "catalog.hpp"
#include "container.hpp"

#include <functional>
#include <set>

namespace nut::gui {

class App;
struct Task;

class TextureEdits {
public:
    // Imports `img` into every copy of `t`: each bundle, each stream bundle entry, and the streamed
    // level 0 of a split texture. All copies take it or none does: on a failure the copies already
    // changed are put back and the error is thrown.
    // Returns what the import did to the first copy ("imported 7 levels, resized from...").
    std::string import(const TextureInfo& t, const ops::TextureImage& img);
    // Removes the mipmaps of every copy of `t` (streamed top levels are not touched); the same
    // all-or-nothing rule. Returns whether any copy changed.
    bool removeMips(const TextureInfo& t);
    // Level 0 of `t` as it is now (merged from the stream bundle when split).
    image::Rgba currentImage(const TextureInfo& t);
    // Writes the changed files (each backed up first) and forgets every loaded file. Returns how
    // many were written; `written` collects their paths.
    int write(App& app, std::set<fs::path>* written = nullptr);
    // Decompressed size of the files loaded so far.
    uint64_t loadedBytes() const { return loadedBytes_; }
    // Whether an import changed a texture's size (the menus may need fixMenus).
    bool resizedAny() const { return resized_; }

private:
    struct File {
        Loaded l;
        bool dirty = false;
    };
    File& load(const fs::path& p);
    // Runs `change` on every copy of `t`, undoing them all when one throws. It gets the file with
    // the copy's streamed level 0 (`withTops`, bundle copies of split textures) and returns
    // whether it changed the copy.
    void eachCopy(const TextureInfo& t, bool withTops, const std::function<bool(caff::Caff&, const ops::TexRef&, File*)>& change);

    std::map<fs::path, File> files_;
    uint64_t loadedBytes_ = 0;
    bool resized_ = false;
};

// Fits the menus to the textures whose size differs from their .bak backups (ops::adaptMenus over
// the catalog's menu bundles), so their images keep their size on screen. Runs on a worker thread;
// notes go to the log. Returns the number of menu elements adjusted.
int fixMenus(App& app, const Catalog& cat, Task* task);

struct BulkImportResult {
    int imported = 0, unchanged = 0, failed = 0, unmatched = 0, files = 0;
    int menuElements = 0; // adjusted by fixMenus after textures changed size
};
// Imports every image directly in `folder` whose name (without the extension) is a texture's
// name, as Export PNG names them, into every copy of that texture. Array textures also take one
// image per slice, named <texture>_slice0, <texture>_slice1... (as exported). Images identical to the
// texture are skipped, so re-importing unedited exports changes nothing. Each texture is all or
// nothing; failures are logged and the rest carry on. Runs on a worker thread.
BulkImportResult bulkImport(App& app, const Catalog& cat, const fs::path& folder, Task& task);

} // namespace nut::gui
