// nuttool-gui: application shell (browser, document tabs, background tasks, dialogs).
#pragma once

#include "catalog.hpp"
#include "common.hpp"
#include "image.hpp"
#include "nbpatch.hpp"
#include "tools.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace nut::gui {

class App;

std::string toUtf8(const fs::path& p);
fs::path fromUtf8(const std::string& s);
// Forward-slash form, for storing relative paths.
std::string toUtf8Generic(const fs::path& p);
inline fs::path fromUtf8Generic(const std::string& s) { return fromUtf8(s); }
std::string formatSize(uint64_t bytes);

// A picture for the overlay (reNut's renderer uploads it with the next frame).
struct GpuImage {
    ImTextureID tex = ImTextureID_Invalid;
    int width = 0, height = 0;
    GpuImage() = default;
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    ~GpuImage();
    ImTextureID id() const { return tex; }
};

class Document {
public:
    virtual ~Document() = default;
    virtual std::string title() const = 0;
    virtual void draw(App& app) = 0;
    virtual bool dirty() const { return false; }
    // Saves to `path` (or `saveAs` when given). Report problems with app.error(); return success.
    virtual bool save(App& app, const fs::path* saveAs) { (void)app; (void)saveAs; return true; }
    virtual bool canSave() const { return false; }
    // Whether the tab leaves its view see-through, for the game to draw what it shows (a model in the studio).
    virtual bool showsGame() const { return false; }

    fs::path path;
    std::string uid; // unique per open tab
    std::string key; // identity used to avoid opening the same thing twice (file path, "tex:<name>", ...)
};

// Builds a document for a file. Runs on a worker thread: no rendering calls inside.
std::unique_ptr<Document> loadDocument(const fs::path& path, const fs::path& assetsRoot);

// Background work. The worker returns a closure that runs on the UI thread when it is done.
struct Task {
    std::string label;
    bool modal = false;
    bool readOnly = false; // writes nothing, so it may be abandoned at exit
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::mutex m;
    double progress = -1; // < 0: indeterminate
    std::string step;
    std::function<void()> onDone;
    std::string error;
    std::thread thread;

    void report(double p, const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        progress = p;
        step = s;
    }
};

struct BrowserItem {
    enum Kind { File, Texture, Model, Audio, Script };
    std::string label;
    std::string tooltip;
    fs::path path;
    Kind kind = File;
    std::string key;    // texture/model/wave bank name for catalog items
    std::string lower;  // label + tooltip, lower-case, for searching
    std::string mod;    // the mod whose assets it's one of (the Mods group), else empty
};

struct BrowserGroup {
    std::string name;
    std::vector<BrowserItem> items;
    std::vector<BrowserGroup> children;
};

class App {
public:
    // `assets`: the game's folder; `settings`: the folder NutTool keeps its settings and script in.
    App(SDL_Window* window, const fs::path& assets, const fs::path& settings);
    ~App();

    void frame();             // one UI frame, inside the overlay's frame
    void updateScale();       // re-reads the monitor's content scale (DPI)
    float scale() const { return uiScale_; }
    bool quitRequested() const { return quit_; }
    void requestQuit();       // asks about unsaved documents first
    void clearQuit() { quit_ = false; }
    // Another mod's workspace is in use: every tab closes and the asset lists are read again. False (nothing done)
    // while a tab has unsaved changes.
    bool reloadForWorkspace();
    const std::string& modAuthor() const { return mod_.author; }
    // Menus of the host's own (reNut's Mod menu), drawn in the menu bar before Help, and its items at the bar's right end.
    std::function<void()> hostMenus;
    std::function<void()> hostMenusRight;

    // ---- services used by documents
    std::shared_ptr<GpuImage> upload(const image::Rgba& img, bool nearest = true);
    void status(const std::string& msg);
    void error(const std::string& msg);
    void log(const std::string& msg);
    const fs::path& assetsRoot() const { return root_; }
    // The title update's folder (chosen in the File menu, else found beside the assets folder); empty when none.
    fs::path updateRoot() const;
    bool makeBackups() const { return backups_; }
    // Writes a file the user is editing in place: one-time "<file>.bak" first when enabled.
    void backupBeforeWrite(const fs::path& p);
    void openPath(const fs::path& p);
    // Catalog items (available once indexing has finished).
    void openTexture(const std::string& name);
    void openModel(const std::string& name);
    // A wave bank; `filter` pre-fills its wave filter.
    void openAudio(const std::string& name, const std::string& filter = {});
    // A game script, actor goal list, script table or state table.
    void openScript(const std::string& name);
    // An asset of the Mods group: from its mod's own files, not the game's.
    void openModAsset(const BrowserItem& it);
    // Opens whatever the catalog knows `name` as: a script, model, texture or wave bank. False when it knows none.
    bool openAsset(const std::string& name);
    std::shared_ptr<const Catalog> catalog() const { return catalog_; }
    // Opens a catalog item now, or as soon as indexing finishes ("tex:" / "model:" / "audio:" / "script:" + name).
    void openWhenIndexed(const std::string& key);
    // Animation to play in the next model tab that opens (--anim on the command line).
    void setStartupAnimation(const std::string& name) { startupAnim_ = name; }
    std::string takeStartupAnimation() { return std::exchange(startupAnim_, {}); }
    void setStartupReplacement(const std::string& name) { startupReplace_ = name; }
    // --script <file>: opens the script window and runs the file once the assets folder is known.
    void setStartupScript(const fs::path& p) { startupScript_ = p; }
    void setStartupFbx(const fs::path& p) { startupFbx_ = p; }
    fs::path takeStartupFbx() { return std::exchange(startupFbx_, {}); }
    std::string takeStartupReplacement() { return std::exchange(startupReplace_, {}); }
    // Opens an assets folder, or a single file (from the command line or drag and drop).
    void openAny(const fs::path& p);

    using FileCallback = std::function<void(const fs::path&)>;
    struct Filter { std::string name, pattern; };
    void openFileDialog(const std::string& title, std::vector<Filter> filters, FileCallback cb, const fs::path& start = {});
    void saveFileDialog(const std::string& title, std::vector<Filter> filters, FileCallback cb, const fs::path& start = {});
    void openFolderDialog(const std::string& title, FileCallback cb, const fs::path& start = {});

    void runTask(const std::string& label, bool modal, std::function<std::function<void()>(Task&)> work, bool readOnly = false);
    // For work that writes nothing: at exit it is not waited for.
    void runReadOnlyTask(const std::string& label, bool modal, std::function<std::function<void()>(Task&)> work);
    bool busy() const { return !tasks_.empty(); }

    // ---- the command-line tool's commands (tools.cpp)
    using CommandDone = std::function<void(int code, const std::string& output)>;
    // Runs a nuttool command line on a worker thread, as the command-line tool does. Its output goes to the
    // log (unless `logOutput` is false); `done`, else the status bar, gets the exit code and output.
    void runCommand(const std::string& label, std::vector<std::string> args, CommandDone done = {}, bool logOutput = true);
    // Tools > Export from a file, for `file`; Tools > Export a model, for `model` in `file`.
    void openExportTools(const fs::path& file);
    void openModelExport(const fs::path& file, const std::string& model);
    // reNut > New part: the part's tab opens its Save as reNut part dialog when it is drawn.
    void requestPartDialog(const std::string& part) { partDialog_ = part; }
    bool takePartDialog(const std::string& part);
    void showLog() { showLog_ = true; }
    // The model tab's sections beyond View, Replace, Rig & weights and Animation, shown from View in the menu bar.
    struct ModelPanels {
        bool parts = false, materials = false, fur = false, meshes = false, textures = false, skeleton = false, materialNames = false,
             warnings = false;
    } modelPanels;

private:
    void loadSettings();
    void saveSettings();
    void setAssetsRoot(const fs::path& p);
    void scanAssets();
    void collectGlyphs();
    void buildCatalog();
    void addCatalogGroups();
    void openDocument(const std::string& key, const std::string& label, std::function<std::unique_ptr<Document>()> load);
    void rebuildFonts();

    void drawFrame();
    void drawMenu();
    void drawBrowser(float width);
    void drawBrowserGroup(const BrowserGroup& g, bool topLevel);
    void drawDocuments();
    void drawWelcome();
    void drawStatusBar();
    void drawLog();
    void drawTasks();
    void drawExtractDialog();
    void drawModExport();
    void drawIdTool();
    void drawAbout();
    void drawCloseConfirm();
    void pumpDialogs();
    void pumpTasks();
    void saveDocument(Document& d, bool saveAs);
    // Closes the tabs `uids`, asking first when any of them has unsaved changes.
    void closeDocuments(const std::vector<std::string>& uids);
    void removeDocuments(const std::vector<std::string>& uids);
    void drawRestore();
    void drawBulkImport();
    void drawChangedMips();
    // The command-line tool's jobs (tools.cpp)
    void drawToolWindows();
    void drawConsole();
    void drawPackTools();
    void drawExportTools();
    void drawModelExport();
    void drawImportTools();
    void drawScriptTools();
    void drawFileJobs();
    void drawActorSwap();  // Tools > Replace an actor
    void drawPartPicker();
    void drawBlockTool();
    void drawWorldTool();
    bool drawNpcEditor();  // the world tool's characters; true when each can be made
    void drawVehicleTool();
    bool drawVehicleEditor();  // the vehicle tool's parts list; true when it can be made
    // Runs the command `build` makes (it throws when a path can't be passed); `writes` are files it may
    // change, refused while a tab holding one has unsaved changes and closed after.
    void runTool(const std::string& label, const std::function<std::vector<std::string>()>& build, std::vector<fs::path> writes = {});
    void commandFinished(const std::string& label, int code, const std::string& output);
    bool checkUnsaved(const std::vector<fs::path>& files);
    void reloadWritten(const std::vector<fs::path>& files);
    // The models, animations and textures in `file` (UTF-8), read in the background; null until then.
    const FileNames* fileNames(const std::string& file);
    // Scripting (script_window.cpp)
    void drawScript();
    void runScript();
    bool scriptRunning() const;
    void saveScriptText();
    fs::path scriptAutosavePath() const;

    SDL_Window* window_;
    bool quit_ = false;
    bool quitPending_ = false;

    fs::path root_;
    fs::path updateDir_; // chosen title update folder; empty finds it beside the assets folder
    fs::path settingsPath_;
    bool backups_ = true;
    bool showLog_ = false;
    bool darkTheme_ = true;
    float browserWidth_ = 270;
    std::string browserFilter_;
    bool browserFilterChanged_ = false; // the search text changed this frame
    std::vector<BrowserGroup> groups_;
    std::map<std::string, std::string> labels_; // file path -> friendly label
    std::shared_ptr<const Catalog> catalog_;
    bool indexing_ = false;
    std::vector<std::string> pendingOpen_;
    std::string startupAnim_, startupReplace_;

    std::vector<std::unique_ptr<Document>> docs_;
    std::string selectTab_;      // tab to bring to front on the next frame
    std::string currentTabUid_;  // tab shown this frame
    std::vector<std::string> closeUids_; // tabs waiting for the unsaved-changes answer
    std::function<void()> afterClose_;   // the model opened once the one open has closed
    // One model at a time: closes the open model tabs other than `key`; false when unsaved changes are asked about
    // first, `retry` running once they're saved or discarded.
    bool closeOtherModels(const std::string& key, std::function<void()> retry);
    // The Mods group: a mod's assets (its game-layout folders and unpacked package), the game's with them over it,
    // and the mod's other files.
    struct ModView {
        std::string name;
        fs::path root;  // the mod's folder or package
        std::shared_ptr<const Catalog> catalog, merged;
        std::vector<fs::path> files;
        std::string error;  // why its package couldn't be unpacked
    };
    std::vector<ModView> modViews_;
    bool collapseDoc_ = false;           // collapse every section of the shown tab this frame
    bool collapseBrowser_ = false;       // and of the asset tree
    int uidCounter_ = 0;

    std::string status_;
    bool statusError_ = false;
    std::mutex logMutex_;
    std::deque<std::string> log_;

    std::vector<std::unique_ptr<Task>> tasks_;

    struct DialogRequest {
        FileCallback cb;
        std::vector<std::string> strings;
        std::vector<SDL_DialogFileFilter> filters;
        std::string start;
    };
    std::mutex dialogMutex_;
    std::map<int, std::unique_ptr<DialogRequest>> dialogs_;
    std::deque<std::pair<int, std::string>> dialogResults_;
    int dialogCounter_ = 0;
    static inline App* instance_ = nullptr; // for SDL dialog callbacks
    static void SDLCALL dialogCallback(void* userdata, const char* const* files, int filter);
    int newDialog(FileCallback cb, std::vector<Filter> filters, const fs::path& start);

    // Tools
    bool showExtract_ = false, showIdTool_ = false, showAbout_ = false;
    std::string extractOut_;
    fs::path lastDir_; // folder of the last file or folder picked in a dialog
    bool extractPng_ = false;
    bool extractTexturesOnly_ = false, extractDds_ = false;
    std::string extractFilter_;
    bool extractCats_[7] = {true, true, true, true, true, true, true};
    std::string idToolInput_;
    bool showModExport_ = false;
    nbpatch::Info mod_;
    int modSource_ = 0; // 0: files with a .bak, 1: differences from modOriginal_
    std::string modOriginal_, modOut_;
    std::vector<nbpatch::Change> modChanges_;
    std::vector<char> modPick_;
    bool modScanned_ = false;
    // Restoring backups
    bool showRestore_ = false, restoreScanned_ = false;
    std::vector<nbpatch::Change> restoreChanges_;
    std::vector<char> restorePick_;
    // Texture jobs over many textures
    bool showBulkImport_ = false, showChangedMips_ = false;
    std::string bulkFolder_;
    bool mipsFromOriginal_ = false; // Remove mipmaps from changed textures: compare with modOriginal_
    // Windows for the command-line tool's jobs, and the part whose reNut dialog was asked for.
    ToolWindows tools_;
    std::string partDialog_;
    // Scripting: the editor text is kept in script.lua next to settings.txt.
    struct ScriptOutput {
        std::mutex m;
        std::string text;
        bool scroll = false;
    };
    bool showScript_ = false, scriptLoaded_ = false;
    bool scriptAutosave_ = true; // off for a --script file, so it does not replace the saved editor text
    std::string scriptText_;
    std::string scriptArgs_; // the script's `arg` table, as on the command line
    fs::path scriptPath_, startupScript_, startupFbx_;
    std::shared_ptr<ScriptOutput> scriptOutput_ = std::make_shared<ScriptOutput>();
    Task* scriptTask_ = nullptr; // compared only, never dereferenced unless still in tasks_

    // Fonts and look: NutTool's own style, swapped in while it draws, and its fonts in the overlay's atlas.
    std::set<uint32_t> glyphs_;
    bool fontsDirty_ = false;
    ImGuiStyle style_;
    ImFont* font_ = nullptr;
    float uiScale_ = 1.0f;   // monitor scale x userZoom_
    float userZoom_ = 1.0f;  // View > UI size
    void setZoom(float z);
};

// Size relative to the UI font (16 at 100% scale), so layouts follow the DPI setting.
inline float em(float v) { return v * ImGui::GetFontSize() / 16.0f; }

// Small shared widgets.
// Vertical drag handle between two side-by-side panes; `width` is the left pane's width.
void splitter(const char* id, float& width, float minWidth, float maxWidth, float height);
void hexView(const char* id, const Bytes& data, float height);
void helpMarker(const char* text);
// Closes every tree node and collapsing header of the current window and its child windows.
void collapseWindowTree();
// True while a "Collapse all" is drawn: nodes that open by default call SetNextItemOpen(false) then.
bool collapsingAll();
// Suggested save name for an asset: its name without "aid_<type>_" and the game prefix, plus `ext`.
fs::path exportName(const std::string& asset, const std::string& ext);
bool inputText(const char* label, std::string& s, ImGuiInputTextFlags flags = 0);
bool inputTextWithHint(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags = 0);
bool inputTextMultiline(const char* label, std::string& s, const ImVec2& size, ImGuiInputTextFlags flags = 0);

// A path field (UTF-8) with a Browse... button, then `label` (up to any "##"). Returns true when typed in.
enum class PathPick { OpenFile, SaveFile, Folder };
bool pathField(App& app, const char* label, std::string& path, PathPick pick, std::vector<App::Filter> filters = {}, const char* hint = "");
// A text field with a list to pick from beside it (typing narrows the list), then `label`.
bool comboField(const char* label, std::string& value, const std::vector<std::string>& options, const char* hint = "", float width = 0);
// A path as the command-line tool reads its arguments (the system code page); throws when it can't be.
std::string cliPath(const fs::path& p);
// A command line's arguments: spaces separate them, double quotes keep spaces in one.
std::vector<std::string> splitCommandLine(const std::string& line);

} // namespace nut::gui
