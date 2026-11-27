// Windows for the command-line tool's jobs that have no tab of their own: they run the same command
// code (commands.cpp, or renutmake for reNut blocks) on a worker thread, with its output in the log.
#include "app.hpp"

#include "actorswap.hpp"
#include "assetdb.hpp"
#include "commands.hpp"
#include "container.hpp"
#include "gltfobj.hpp"
#include "image.hpp"
#include "model.hpp"
#include "modelimport.hpp"
#include "ops.hpp"
#include "json.hpp"
#include "renutmake.hpp"
#include "renutnpc.hpp"
#include "renutworld.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace nut::gui {

namespace {

// The part of an ImGui label that is shown: up to any "##".
std::string shownLabel(const char* label) {
    std::string s = label;
    size_t h = s.find("##");
    return h == std::string::npos ? s : s.substr(0, h);
}

// "name=weight, name=weight" as shape keys' starting weights (what isn't a name=number is skipped).
std::map<std::string, float> shapeWeightsText(const std::string& text) {
    std::map<std::string, float> out;
    for (const std::string& item : split(text, ',')) {
        const size_t eq = item.rfind('=');
        if (eq == std::string::npos || trim(item.substr(0, eq)).empty()) continue;
        out[trim(item.substr(0, eq))] = std::strtof(trim(item.substr(eq + 1)).c_str(), nullptr);
    }
    return out;
}

// A material's settings for the world tool's table, and back (an empty emissive texture: the model's own).
const char* const kMaterialAlphas[] = {"opaque", "cutout", "blend", "add"};

MaterialEdit materialEdit(const std::string& name, const renutworld::MaterialSettings& s) {
    MaterialEdit e;
    e.name = name;
    for (int i = 0; i < 4; ++i)
        if (s.alpha && *s.alpha == kMaterialAlphas[i]) e.alpha = i;
    e.cutoff = s.cutoff.value_or(0.5f);
    e.opacity = s.opacity.value_or(1.0f);
    if (s.emissive)
        for (size_t k = 0; k < 3; ++k) e.emissive[k] = (*s.emissive)[k];
    e.strength = s.strength.value_or(1.0f);
    e.autoGlow = !s.glow || *s.glow < 0;
    e.glow = e.autoGlow ? 1.0f : *s.glow;
    e.doubleSided = s.doubleSided.value_or(false);
    e.unlit = s.unlit.value_or(false);
    if (s.emissiveTexture && !s.emissiveTexture->empty()) e.emissiveTexture = toUtf8(*s.emissiveTexture);
    return e;
}

renutworld::MaterialSettings materialSettings(const MaterialEdit& e) {
    renutworld::MaterialSettings s;
    s.alpha = kMaterialAlphas[std::clamp(e.alpha, 0, 3)];
    s.cutoff = e.cutoff;
    s.opacity = e.opacity;
    s.emissive = std::array<float, 3>{e.emissive[0], e.emissive[1], e.emissive[2]};
    s.strength = e.strength;
    s.glow = e.autoGlow ? -1.0f : e.glow;
    s.doubleSided = e.doubleSided;
    s.unlit = e.unlit;
    if (!trim(e.emissiveTexture).empty()) s.emissiveTexture = fromUtf8(trim(e.emissiveTexture));
    return s;
}

// The weights as that text.
std::string shapeWeightsText(const std::map<std::string, float>& weights) {
    std::string out;
    for (const auto& [name, weight] : weights) {
        char b[48];
        std::snprintf(b, sizeof b, "=%g", double(weight));
        out += (out.empty() ? "" : ", ") + name + b;
    }
    return out;
}

std::string lastLine(const std::string& text) {
    std::string last;
    for (auto& l : split(text, '\n'))
        if (!trim(l).empty()) last = trim(l);
    return last;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

// A typed path, or an empty one when nothing was typed.
fs::path optPath(const std::string& s) { return trim(s).empty() ? fs::path() : fromUtf8(trim(s)); }

bool isUnder(const fs::path& p, const fs::path& root) {
    if (root.empty()) return false;
    std::error_code ec;
    std::string a = lower(toUtf8Generic(fs::absolute(p, ec).lexically_normal()));
    std::string b = lower(toUtf8Generic(fs::absolute(root, ec).lexically_normal()));
    if (!b.empty() && b.back() != '/') b += '/';
    return startsWith(a, b);
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) return fs::equivalent(a, b, ec);
    return lower(toUtf8Generic(a.lexically_normal())) == lower(toUtf8Generic(b.lexically_normal()));
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A button that starts a job: disabled while another runs, or until the job has what it needs.
bool runButton(const App& app, const char* label, bool ready) {
    ImGui::BeginDisabled(app.busy() || !ready);
    bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    return pressed;
}

// A bundle's paired stream bundle (Bundle\50\<same name>) when it exists, for a picker to suggest.
std::string pairedStream(const std::string& file) {
    fs::path f = optPath(file);
    if (f.empty() || !f.has_parent_path()) return {};
    fs::path sp = model::pairedStreamBundle(f);
    std::error_code ec;
    return sp != f && fs::is_regular_file(sp, ec) ? toUtf8(sp) : std::string();
}

const std::vector<std::string> kNone;
const std::vector<std::string> kAttachable = {"all", "none", "top", "bottom", "sides", "top+sides"};
const std::vector<std::string> kCategories = {"body", "gadget", "seat"};
const char* const kAxes[] = {"X", "Y", "Z", "-X", "-Y", "-Z"};
const char* const kAllFiles = "*";

} // namespace

// ---- widgets and helpers -------------------------------------------------------------

bool pathField(App& app, const char* label, std::string& path, PathPick pick, std::vector<App::Filter> filters, const char* hint) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(em(380));
    bool changed = inputTextWithHint("##path", hint, path);
    ImGui::SameLine();
    std::string text = shownLabel(label);
    if (ImGui::Button("Browse...")) {
        std::string* target = &path;
        App::FileCallback cb = [target](const fs::path& p) { *target = toUtf8(p); };
        fs::path start = optPath(path);
        if (pick == PathPick::Folder) app.openFolderDialog(text, cb, start);
        else if (pick == PathPick::SaveFile) app.saveFileDialog(text, std::move(filters), cb, start);
        else app.openFileDialog(text, std::move(filters), cb, start);
    }
    if (!text.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::PopID();
    return changed;
}

bool comboField(const char* label, std::string& value, const std::vector<std::string>& options, const char* hint, float width) {
    ImGui::PushID(label);
    float w = width > 0 ? width : em(380);
    ImGui::SetNextItemWidth(std::max(em(60), w - ImGui::GetFrameHeight()));
    bool changed = inputTextWithHint("##value", hint, value);
    ImGui::SameLine(0, 0);
    if (ImGui::BeginCombo("##pick", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
        // A value that is one of the options lists them all; other text narrows the list.
        bool exact = std::find(options.begin(), options.end(), value) != options.end();
        std::string needle = exact ? std::string() : lower(trim(value));
        int listed = 0;
        for (auto& o : options) {
            if (!needle.empty() && lower(o).find(needle) == std::string::npos) continue;
            if (++listed > 2000) {
                ImGui::TextDisabled("... type to narrow the list");
                break;
            }
            if (ImGui::Selectable(o.c_str(), o == value)) {
                value = o;
                changed = true;
            }
        }
        if (!listed) ImGui::TextDisabled(options.empty() ? "nothing to pick" : "nothing matches");
        ImGui::EndCombo();
    }
    std::string text = shownLabel(label);
    if (!text.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::PopID();
    return changed;
}

std::string cliPath(const fs::path& p) {
    std::string s;
    try {
        s = p.string();
    } catch (std::exception&) {
        s.clear();
    }
    if (s.empty() != p.empty() || fs::path(s) != p)
        fail(toUtf8(p) + ": the command-line tool can't take this path (it has characters outside the system code page)");
    return s;
}

std::vector<std::string> splitCommandLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char ch : line) {
        if (ch == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (ch == ' ' || ch == '\t')) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += ch;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

void glbNodes(const std::string& file, std::string& seen, std::vector<std::string>& nodes, std::string& error) {
    if (file == seen) return;
    seen = file;
    nodes.clear();
    error.clear();
    fs::path p = optPath(file);
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) return;
    try {
        Bytes d = readFile(p);
        if (!gltfobj::isGlb(d)) return; // OBJ and FBX files have no node to pick
        nodes = gltfobj::meshNodes(d);
        if (nodes.empty()) error = "this .glb has no node with a mesh";
    } catch (std::exception& e) {
        error = e.what();
    }
}

// ---- running commands -----------------------------------------------------------------

void App::runCommand(const std::string& label, std::vector<std::string> args, CommandDone done, bool logOutput) {
    runTask(label, true, [this, label, args, done, logOutput](Task& t) -> std::function<void()> {
        std::string out, line;
        int code = 1;
        {
            // Each finished line goes to the log as it comes, and shows in the progress popup.
            cli::Sink sink = [&](const std::string& s) {
                out += s;
                for (char ch : s) {
                    if (ch != '\n') {
                        line += ch;
                        continue;
                    }
                    if (!trim(line).empty()) {
                        if (logOutput) log(line);
                        t.report(-1, trim(line));
                    }
                    line.clear();
                }
            };
            cli::SinkScope scope(sink);
            code = cli::run(args);
        }
        if (logOutput && !trim(line).empty()) log(line);
        return [this, label, code, out, done] {
            if (done) done(code, out);
            else commandFinished(label, code, out);
        };
    });
}

void App::commandFinished(const std::string& label, int code, const std::string& output) {
    // The output is in the log already, so the status bar is set without logging it again.
    std::string last = lastLine(output);
    if (code) {
        if (startsWith(last, "error: ")) last = last.substr(7);
        status_ = label + " failed: " + (last.empty() ? "exit code " + std::to_string(code) : last);
        statusError_ = true;
        showLog_ = true;
    } else {
        status_ = last.empty() ? label + ": done" : last;
        statusError_ = false;
    }
}

void App::runTool(const std::string& label, const std::function<std::vector<std::string>()>& build, std::vector<fs::path> writes) {
    std::vector<std::string> args;
    try {
        args = build();
    } catch (std::exception& e) {
        error(label + ": " + e.what());
        return;
    }
    if (!checkUnsaved(writes)) return;
    runCommand(label, std::move(args), [this, label, writes](int code, const std::string& out) {
        commandFinished(label, code, out);
        if (code == 0) reloadWritten(writes);
    });
}

bool App::checkUnsaved(const std::vector<fs::path>& files) {
    for (auto& d : docs_)
        for (auto& f : files)
            if (d->dirty() && !d->path.empty() && samePath(d->path, f)) {
                error(d->title() + " has unsaved changes to " + toUtf8(f.filename()) + ": save or close that tab first");
                return false;
            }
    return true;
}

void App::reloadWritten(const std::vector<fs::path>& files) {
    std::vector<std::string> stale;
    bool game = false;
    for (auto& f : files) {
        if (f.empty()) continue;
        game |= isUnder(f, root_) || isUnder(f, updateRoot());
        for (auto& d : docs_)
            if (!d->dirty() && !d->path.empty() && samePath(d->path, f)) stale.push_back(d->uid);
    }
    // Tabs showing a file the command changed would save the old contents over it.
    if (!stale.empty()) removeDocuments(stale);
    if (game) {
        scanAssets();
        buildCatalog();
    }
}

const FileNames* App::fileNames(const std::string& file) {
    std::string key = trim(file);
    if (key.empty()) return nullptr;
    auto it = tools_.names.find(key);
    if (it != tools_.names.end()) return it->second->loading ? nullptr : it->second.get();
    fs::path p = fromUtf8(key);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return nullptr;
    auto names = std::make_shared<FileNames>();
    tools_.names[key] = names;
    runReadOnlyTask("Reading " + toUtf8(p.filename()), false, [names, p](Task& task) -> std::function<void()> {
        auto r = std::make_shared<FileNames>();
        try {
            Loaded l = loadAsset(p);
            std::set<std::string> models, anims, textures;
            auto scan = [&](caff::Caff& c) {
                for (size_t i = 0; i < c.symbols.size(); ++i) {
                    if (model::isModelSymbol(c, i)) models.insert(c.symbolName(i));
                    else if (model::isAnimationSymbol(c, i)) anims.insert(assetdb::baseAssetName(c.symbolName(i)));
                }
                for (auto& tex : ops::findTextures(c)) textures.insert(tex.name);
            };
            if (l.caff) {
                scan(*l.caff);
            } else if (l.sb) {
                for (size_t i = 0; i < l.sb->entries.size() && !task.cancel; ++i) {
                    task.report(double(i) / double(l.sb->entries.size()), "");
                    if (!caff::isCaff(l.sb->entries[i].data)) continue;
                    try {
                        caff::Caff c = caff::Caff::parse(l.sb->entries[i].data);
                        scan(c);
                    } catch (std::exception&) {
                    }
                }
            } else {
                r->error = toUtf8(p.filename()) + " is not a bundle or stream bundle";
            }
            r->models.assign(models.begin(), models.end());
            r->anims.assign(anims.begin(), anims.end());
            r->textures.assign(textures.begin(), textures.end());
        } catch (std::exception& e) {
            r->error = e.what();
        }
        r->loading = false;
        return [names, r] { *names = std::move(*r); };
    });
    return nullptr;
}

void App::openExportTools(const fs::path& file) {
    tools_.exFile = toUtf8(file);
    tools_.showExport = true;
}

void App::openModelExport(const fs::path& file, const std::string& model) {
    tools_.meFile = toUtf8(file);
    tools_.meModel = model;
    tools_.showModel = true;
}

bool App::takePartDialog(const std::string& part) {
    if (partDialog_.empty() || partDialog_ != part) return false;
    partDialog_.clear();
    return true;
}

void App::drawToolWindows() {
    drawConsole();
    drawPackTools();
    drawExportTools();
    drawModelExport();
    drawImportTools();
    drawScriptTools();
    drawFileJobs();
    drawActorSwap();
    drawPartPicker();
    drawBlockTool();
    drawWorldTool();
    drawVehicleTool();
}

// ---- Tools > Run a nuttool command ------------------------------------------------------------

void App::drawConsole() {
    ToolWindows& t = tools_;
    if (!t.showConsole) return;
    ImGui::SetNextWindowSize(ImVec2(em(760), em(520)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Run a nuttool command", &t.showConsole)) {
        ImGui::End();
        return;
    }
    note("Runs one of the command-line tool's commands, as nuttool <command> ... would, and shows what it prints. Put paths "
         "with spaces in double quotes, and give full paths. Commands that change files don't update open tabs: reopen them after.");
    std::vector<std::string> words = splitCommandLine(t.consoleLine);
    const cli::Command* cmd = nullptr;
    for (auto& c : cli::commands())
        if (!words.empty() && words[0] == c.name) cmd = &c;
    ImGui::SetNextItemWidth(em(170));
    if (ImGui::BeginCombo("##commands", cmd ? cmd->name : "Commands", ImGuiComboFlags_HeightLarge)) {
        for (auto& c : cli::commands()) {
            if (ImGui::Selectable(c.name, cmd == &c)) t.consoleLine = std::string(c.name) + " ";
            if (ImGui::BeginItemTooltip()) {
                ImGui::PushTextWrapPos(em(560));
                ImGui::Text("%s %s", c.name, c.args);
                ImGui::TextDisabled("%s", c.help);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-em(60));
    bool go = inputTextWithHint("##line", "info \"F:\\game\\assets\\Bundle\\4f\\685374\"", t.consoleLine, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    go |= runButton(*this, "Run", !trim(t.consoleLine).empty());
    if (cmd) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s %s", cmd->name, cmd->args);
        ImGui::PopTextWrapPos();
    }
    if (go && !busy()) {
        std::vector<std::string> args = splitCommandLine(t.consoleLine);
        if (!args.empty() && lower(args[0]) == "nuttool") args.erase(args.begin());
        if (!args.empty()) {
            t.consoleOutput += "> " + t.consoleLine + "\n";
            t.consoleScroll = true;
            runCommand("nuttool " + args[0], args, [this](int code, const std::string& out) {
                tools_.consoleOutput += out;
                if (code) tools_.consoleOutput += "(exit code " + std::to_string(code) + ")\n";
                if (tools_.consoleOutput.size() > (4u << 20)) tools_.consoleOutput.erase(0, tools_.consoleOutput.size() - (2u << 20));
                tools_.consoleScroll = true;
            }, false);
        }
    }
    ImGui::TextUnformatted("Output");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) t.consoleOutput.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(t.consoleOutput.c_str());
    if (ImGui::BeginChild("##consoleout", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(t.consoleOutput.data(), t.consoleOutput.data() + t.consoleOutput.size());
        if (t.consoleScroll) {
            ImGui::SetScrollHereY(1.0f);
            t.consoleScroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Tools > Pack and convert files ---------------------------------------------------------

void App::drawPackTools() {
    ToolWindows& t = tools_;
    if (!t.showPack) return;
    ImGui::SetNextWindowSize(ImVec2(em(680), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Pack and convert files", &t.showPack)) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText("Unpack a bundle or stream bundle");
    note("Writes any bundle (CAFF, text tables' included) as caff.json and one file per symbol, or a stream bundle as one file "
         "per entry, to edit and pack back below. Bundle and stream bundle tabs do the same for the file they show.");
    pathField(*this, "Bundle##cu", t.cuFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    pathField(*this, "Into folder##cu", t.cuDir, PathPick::Folder);
    if (runButton(*this, "Unpack bundle", !trim(t.cuFile).empty() && !trim(t.cuDir).empty())) {
        fs::path in = optPath(t.cuFile), dir = optPath(t.cuDir);
        runTool("Unpacking " + toUtf8(in.filename()), [in, dir] { return std::vector<std::string>{"caff-unpack", cliPath(in), cliPath(dir)}; });
    }
    pathField(*this, "Stream bundle##su", t.suFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    pathField(*this, "Into folder##su", t.suDir, PathPick::Folder);
    if (runButton(*this, "Unpack stream bundle", !trim(t.suFile).empty() && !trim(t.suDir).empty())) {
        fs::path in = optPath(t.suFile), dir = optPath(t.suDir);
        runTool("Unpacking " + toUtf8(in.filename()), [in, dir] { return std::vector<std::string>{"sb-unpack", cliPath(in), cliPath(dir)}; });
    }

    ImGui::SeparatorText("Bundle from an unpacked folder");
    note("Rebuilds a bundle (CAFF) from the folder a bundle tab's Unpack to folder wrote (caff.json and one file per symbol), "
         "with your edited or resized symbol files applied.");
    pathField(*this, "Unpacked folder##caff", t.caffDir, PathPick::Folder);
    pathField(*this, "Bundle to write##caff", t.caffOut, PathPick::SaveFile, {{"All files", kAllFiles}});
    ImGui::Checkbox("XCTD-wrapped##caff", &t.caffXctd);
    helpMarker("Wraps the bundle in an uncompressed XCTD container like the shipped files. The game reads plain bundles too.");
    if (runButton(*this, "Pack bundle", !trim(t.caffDir).empty() && !trim(t.caffOut).empty())) {
        fs::path dir = optPath(t.caffDir), out = optPath(t.caffOut);
        bool xctd = t.caffXctd;
        runTool("Packing " + toUtf8(out.filename()), [dir, out, xctd] {
            std::vector<std::string> a = {"caff-pack", cliPath(dir), cliPath(out)};
            if (xctd) a.push_back("--xctd");
            return a;
        }, {out});
    }

    ImGui::SeparatorText("Stream bundle from an unpacked folder");
    note("Rebuilds a stream bundle from the folder a stream bundle tab's Unpack all wrote.");
    pathField(*this, "Unpacked folder##sb", t.sbDir, PathPick::Folder);
    pathField(*this, "Stream bundle to write##sb", t.sbOut, PathPick::SaveFile, {{"All files", kAllFiles}});
    if (runButton(*this, "Pack stream bundle", !trim(t.sbDir).empty() && !trim(t.sbOut).empty())) {
        fs::path dir = optPath(t.sbDir), out = optPath(t.sbOut);
        runTool("Packing " + toUtf8(out.filename()), [dir, out] { return std::vector<std::string>{"sb-pack", cliPath(dir), cliPath(out)}; }, {out});
    }

    ImGui::SeparatorText("XCTD");
    note("Most shipped bundles are XCTD-compressed. Decompress writes the plain file; Wrap puts a file in an uncompressed XCTD container.");
    pathField(*this, "Compressed file##unxctd", t.unxctdIn, PathPick::OpenFile, {{"All files", kAllFiles}});
    pathField(*this, "Write to##unxctd", t.unxctdOut, PathPick::SaveFile, {{"All files", kAllFiles}});
    if (runButton(*this, "Decompress", !trim(t.unxctdIn).empty() && !trim(t.unxctdOut).empty())) {
        fs::path in = optPath(t.unxctdIn), out = optPath(t.unxctdOut);
        runTool("Decompressing " + toUtf8(in.filename()), [in, out] { return std::vector<std::string>{"decompress", cliPath(in), cliPath(out)}; }, {out});
    }
    pathField(*this, "Plain file##xctd", t.xctdIn, PathPick::OpenFile, {{"All files", kAllFiles}});
    pathField(*this, "Write to##xctd", t.xctdOut, PathPick::SaveFile, {{"All files", kAllFiles}});
    if (runButton(*this, "Wrap in XCTD", !trim(t.xctdIn).empty() && !trim(t.xctdOut).empty())) {
        fs::path in = optPath(t.xctdIn), out = optPath(t.xctdOut);
        runTool("Wrapping " + toUtf8(in.filename()), [in, out] { return std::vector<std::string>{"compress", cliPath(in), cliPath(out)}; }, {out});
    }
    ImGui::End();
}

// ---- Tools > Export from a file ---------------------------------------------------------------

void App::drawExportTools() {
    ToolWindows& t = tools_;
    if (!t.showExport) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export from a file", &t.showExport)) {
        ImGui::End();
        return;
    }
    note("Exports everything of one kind from a bundle, stream bundle or wave bank (.xwb) into a folder.");
    pathField(*this, "File##ex", t.exFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    if (t.exFile != t.exFileSeen) {
        t.exFileSeen = t.exFile;
        t.exStream = pairedStream(t.exFile);
    }
    pathField(*this, "Output folder##ex", t.exDir, PathPick::Folder);
    bool ready = !trim(t.exFile).empty() && !trim(t.exDir).empty();
    fs::path file = optPath(t.exFile), dir = optPath(t.exDir);

    ImGui::SeparatorText("Textures");
    ImGui::RadioButton("DDS##ex", &t.exTexFormat, 0);
    ImGui::SameLine();
    ImGui::RadioButton("DDS and PNG##ex", &t.exTexFormat, 1);
    ImGui::SameLine();
    ImGui::RadioButton("PNG only##ex", &t.exTexFormat, 2);
    pathField(*this, "Stream bundle##ex", t.exStream, PathPick::OpenFile, {{"All files", kAllFiles}}, "none");
    helpMarker("A bundle's split textures keep their full-size level in its stream bundle (Bundle\\50, same name), filled in when "
               "there is one: with it they are exported whole.");
    if (runButton(*this, "Export textures", ready)) {
        fs::path stream = optPath(t.exStream);
        int format = t.exTexFormat;
        runTool("Exporting textures", [file, dir, stream, format] {
            std::vector<std::string> a = {"tex-export", cliPath(file), cliPath(dir)};
            if (!stream.empty()) {
                a.push_back("--stream");
                a.push_back(cliPath(stream));
            }
            if (format == 1) a.push_back("--png");
            if (format == 2) a.push_back("--png-only");
            return a;
        });
    }

    ImGui::SeparatorText("Models");
    note("Every model in the file as OBJ + MTL, with its bones in <model>.bones.txt. Export a model (Tools menu) writes one model "
         "as FBX or OBJ with its textures, fur and animations.");
    ImGui::Checkbox("Every draw group##ex", &t.exAllGroups);
    helpMarker("Every level of detail and alternative part, not only those the game shows by default. Export this way to edit a "
               "model for Import OBJ / FBX or a reNut part's model: the objects keep their names (group1_mesh8).");
    ImGui::SameLine();
    ImGui::Checkbox("PNG textures##ex", &t.exModelPng);
    helpMarker("Also writes the file's textures as PNG next to the models.");
    if (runButton(*this, "Export models", ready)) {
        bool all = t.exAllGroups, png = t.exModelPng;
        runTool("Exporting models", [file, dir, all, png] {
            std::vector<std::string> a = {"model-export", cliPath(file), cliPath(dir)};
            if (all) a.push_back("--all-groups");
            if (png) a.push_back("--png");
            return a;
        });
    }

    ImGui::SeparatorText("Wave banks");
    ImGui::Checkbox("Decode to WAV##ex", &t.exWav);
    helpMarker("Writes every wave as 16-bit WAV; off keeps them as stored (.xma). Each bank is also written whole as .xwb.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(200));
    inputTextWithHint("Banks named with##ex", "every bank", t.exWaveName);
    helpMarker("For a stream bundle: only the wave banks whose name contains this text.");
    if (runButton(*this, "Export wave banks", ready)) {
        bool wav = t.exWav;
        std::string name = trim(t.exWaveName);
        runTool("Exporting wave banks", [file, dir, wav, name] {
            std::vector<std::string> a = {"wave-export", cliPath(file), cliPath(dir)};
            if (wav) a.push_back("--wav");
            if (!name.empty()) {
                a.push_back("--name");
                a.push_back(name);
            }
            return a;
        });
    }
    ImGui::End();
}

// ---- Tools > Export a model (model-fbx / model-obj) ------------------------------------------------

void App::drawModelExport() {
    ToolWindows& t = tools_;
    if (!t.showModel) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Export a model", &t.showModel)) {
        ImGui::End();
        return;
    }
    note("Writes one model of a bundle as FBX (meshes skinned to the skeleton, materials with embedded textures, animations as "
         "takes, fur shells) or OBJ (.obj, .mtl and PNG textures; no skeleton). A model tab's Export FBX / OBJ does the same for "
         "the meshes it shows.");
    pathField(*this, "Bundle##me", t.meFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    const FileNames* names = fileNames(t.meFile);
    comboField("Model##me", t.meModel, names ? names->models : kNone, names || trim(t.meFile).empty() ? "aid_model_..." : "reading the file...");
    if (names && !names->error.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", names->error.c_str());
    pathField(*this, "Write to##me", t.meOut, PathPick::SaveFile, {{"FBX model", "fbx"}, {"OBJ model", "obj"}}, "a .fbx or .obj file");
    bool obj = lower(toUtf8(optPath(t.meOut).extension())) == ".obj";

    ImGui::SeparatorText("Meshes and textures");
    ImGui::Checkbox("Every draw group##me", &t.meAllGroups);
    helpMarker("Every level of detail and alternative part, not only those the game shows by default.");
    ImGui::SameLine();
    ImGui::Checkbox("Textures##me", &t.meTextures);
    helpMarker("The materials' textures (from the bundle and its paired stream bundle): embedded in the FBX, PNG files beside the OBJ.");
    ImGui::SameLine();
    ImGui::Checkbox("Material tint##me", &t.meTint);
    helpMarker("Tints the textures by their material's colour gain and offset, as the game draws them. Off keeps them as stored.");

    ImGui::SeparatorText("Fur");
    ImGui::Checkbox("Fur shells##me", &t.meFur);
    helpMarker("Furry models get their fur as shell meshes, one per slice of the fur volume, with baked alpha textures.");
    ImGui::BeginDisabled(!t.meFur);
    ImGui::SetNextItemWidth(em(110));
    ImGui::SliderFloat("Length (%)##me", &t.meFurLength, 0.2f, 6.0f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(90));
    ImGui::SliderFloat("Tiling##me", &t.meFurTiling, 1, 16, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(90));
    ImGui::InputInt("Texture size (px)##me", &t.meFurSize, 0);
    t.meFurSize = std::clamp(t.meFurSize, 16, 4096);
    ImGui::EndDisabled();

    ImGui::SeparatorText(obj ? "Skeleton and animations (FBX only)" : "Skeleton and animations");
    ImGui::BeginDisabled(obj);
    ImGui::RadioButton("Animations named after it##me", &t.meAnims, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Every animation##me", &t.meAnims, 1);
    ImGui::SameLine();
    ImGui::RadioButton("None##me", &t.meAnims, 2);
    pathField(*this, "Animations from##me", t.meAnimsFrom, PathPick::OpenFile, {{"All files", kAllFiles}}, "the same bundle");
    helpMarker("The bundle the animations are read from: aid_anim_<model>_*, or every animation there with Every animation.");
    ImGui::Checkbox("Reorient bones##me", &t.meReorient);
    helpMarker("Turns each bone so the bone axis runs along it to its child (Blender's bones run along Y). The skin, bind pose and "
               "animations are written to match.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!t.meReorient);
    ImGui::SetNextItemWidth(em(70));
    ImGui::Combo("Bone axis##me", &t.meAxis, kAxes, IM_ARRAYSIZE(kAxes));
    ImGui::EndDisabled();
    ImGui::Checkbox("Short end bones##me", &t.meEnds);
    helpMarker("Gives each bone without children a short <bone>_end child, so Blender draws it as a stub.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!t.meEnds);
    ImGui::SetNextItemWidth(em(110));
    ImGui::SliderFloat("End length (%)##me", &t.meEndSize, 0.2f, 5.0f, "%.1f");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::Spacing();
    bool ready = !trim(t.meFile).empty() && !trim(t.meModel).empty() && !trim(t.meOut).empty();
    if (runButton(*this, "Export##me", ready)) {
        fs::path file = optPath(t.meFile), out = optPath(t.meOut), animsFrom = optPath(t.meAnimsFrom);
        if (!obj && lower(toUtf8(out.extension())) != ".fbx") out += ".fbx";
        std::string modelName = trim(t.meModel);
        ToolWindows o = t; // the options as they are now
        o.names.clear();
        runTool("Exporting " + modelName, [file, out, animsFrom, modelName, obj, o] {
            std::vector<std::string> a = {obj ? "model-obj" : "model-fbx", cliPath(file), modelName, cliPath(out)};
            if (o.meAllGroups) a.push_back("--all-groups");
            if (!o.meTextures) a.push_back("--no-textures");
            if (!o.meTint) a.push_back("--no-tint");
            if (!o.meFur) {
                a.push_back("--no-fur");
            } else {
                a.insert(a.end(), {"--fur-length", num(o.meFurLength), "--fur-tiling", num(o.meFurTiling), "--fur-size", std::to_string(o.meFurSize)});
            }
            if (!obj) {
                if (o.meAnims == 1) a.push_back("--all-anims");
                if (o.meAnims == 2) a.push_back("--no-anims");
                if (!animsFrom.empty() && o.meAnims != 2) a.insert(a.end(), {"--anims", cliPath(animsFrom)});
                if (o.meReorient) a.insert(a.end(), {"--reorient-bones", "--bone-axis", kAxes[std::clamp(o.meAxis, 0, 5)]});
                if (o.meEnds) a.insert(a.end(), {"--bone-ends", "--bone-end-size", num(o.meEndSize)});
            }
            return a;
        });
    }

    ImGui::SeparatorText("Shape keys (reNut models)");
    note("Shape keys of a model NutTool made for reNut (a world, character or block) as a .glb: each draw that has them "
         "a mesh with them as named morph targets, at the starting weights the mod's Lua gives them. With no model picked, "
         "every model in the bundle.");
    pathField(*this, "Write to##meshapes", t.meShapesOut, PathPick::SaveFile, {{"glTF binary", "glb"}}, "a .glb file");
    if (runButton(*this, "Export shape keys##me", !trim(t.meFile).empty() && !trim(t.meShapesOut).empty())) {
        fs::path file = optPath(t.meFile), out = optPath(t.meShapesOut);
        if (lower(toUtf8(out.extension())) != ".glb") out += ".glb";
        const std::string modelName = trim(t.meModel);
        runTool("Exporting shape keys", [file, out, modelName] {
            std::vector<std::string> a = {"renut-shapes-export", cliPath(file), cliPath(out)};
            if (!modelName.empty()) a.insert(a.end(), {"--model", modelName});
            return a;
        });
    }
    ImGui::End();
}

// ---- Tools > Import into a file -----------------------------------------------------------------

void App::drawImportTools() {
    ToolWindows& t = tools_;
    if (!t.showImport) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Import into a file", &t.showImport)) {
        ImGui::End();
        return;
    }
    note("Changes one file as the command line does: in place, after a one-time .bak backup, or written to another file. Texture "
         "and model tabs change every copy in the game files instead.");

    ImGui::SeparatorText("Texture");
    pathField(*this, "Bundle##ti", t.tiFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    if (t.tiFile != t.tiFileSeen) {
        t.tiFileSeen = t.tiFile;
        t.tiStream = pairedStream(t.tiFile);
    }
    const FileNames* tn = fileNames(t.tiFile);
    comboField("Texture##ti", t.tiTexture, tn ? tn->textures : kNone, "its name, or a part of it");
    if (tn && !tn->error.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tn->error.c_str());
    pathField(*this, "Image##ti", t.tiImage, PathPick::OpenFile, {{"Images", ops::textureImagePattern()}});
    helpMarker("PNG/JPG/BMP/TGA are compressed to the texture's format with a new mip chain; an image of another size resizes it. "
               "A DDS must use the texture's format.");
    pathField(*this, "Stream bundle##ti", t.tiStream, PathPick::OpenFile, {{"All files", kAllFiles}}, "none");
    helpMarker("For the mip half of a split texture: its streamed full-size level is replaced too (resizing needs it). The "
               "bundle's paired stream bundle is filled in when there is one.");
    pathField(*this, "Write the stream bundle to##ti", t.tiStreamOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the stream bundle itself");
    pathField(*this, "Write the bundle to##ti", t.tiOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the bundle itself");
    ImGui::Checkbox("XCTD-wrapped##ti", &t.tiXctd);
    bool tiReady = !trim(t.tiFile).empty() && !trim(t.tiTexture).empty() && !trim(t.tiImage).empty();
    if (runButton(*this, "Import texture", tiReady)) {
        fs::path file = optPath(t.tiFile), image = optPath(t.tiImage), stream = optPath(t.tiStream), streamOut = optPath(t.tiStreamOut),
                 out = optPath(t.tiOut);
        std::string name = trim(t.tiTexture);
        bool xctd = t.tiXctd;
        std::vector<fs::path> writes = {out.empty() ? file : out};
        if (!stream.empty()) writes.push_back(streamOut.empty() ? stream : streamOut);
        runTool("Importing " + name, [file, name, image, stream, streamOut, out, xctd] {
            std::vector<std::string> a = {"tex-import", cliPath(file), name, cliPath(image)};
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (!stream.empty()) {
                a.insert(a.end(), {"--stream", cliPath(stream)});
                if (!streamOut.empty()) a.insert(a.end(), {"--stream-out", cliPath(streamOut)});
            }
            if (xctd) a.push_back("--xctd");
            return a;
        }, writes);
    }

    ImGui::SeparatorText("Model from an OBJ or FBX");
    note("Applies an OBJ or binary FBX to one model of one file, any file (a model tab's Import OBJ / FBX changes every copy "
         "in the game files). Objects named as the export names them (group0_mesh3) edit that mesh; one without faces removes it.");
    pathField(*this, "Bundle or stream bundle##mi", t.miFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    const FileNames* mn = fileNames(t.miFile);
    comboField("Model##mi", t.miModel, mn ? mn->models : kNone, "aid_model_...");
    if (mn && !mn->error.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", mn->error.c_str());
    pathField(*this, "Mesh file##mi", t.miMesh, PathPick::OpenFile, {{"Models", "obj;fbx"}}, "a .obj or .fbx file");
    ImGui::Checkbox("Move the joints to the FBX's bones##mi", &t.miRig);
    helpMarker("Also moves the model's joints to the FBX's bones of the same names (their orientations, hierarchy and count "
               "stay). The FBX may then hold only the skeleton.");
    pathField(*this, "Write to##mi", t.miOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the file itself");
    ImGui::Checkbox("XCTD-wrapped##mi", &t.miXctd);
    if (runButton(*this, "Import model", !trim(t.miFile).empty() && !trim(t.miModel).empty() && !trim(t.miMesh).empty())) {
        fs::path file = optPath(t.miFile), mesh = optPath(t.miMesh), out = optPath(t.miOut);
        std::string name = trim(t.miModel);
        bool rig = t.miRig, xctd = t.miXctd;
        runTool("Importing " + name, [file, name, mesh, out, rig, xctd] {
            std::vector<std::string> a = {"model-import", cliPath(file), name, cliPath(mesh)};
            if (rig) a.push_back("--rig");
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (xctd) a.push_back("--xctd");
            return a;
        }, {out.empty() ? file : out});
    }

    ImGui::SeparatorText("Model swap");
    note("Replaces a model with a copy of another one, from the same file or any other; the name stays, so the game loads it "
         "in its place. Textures it needs that the file lacks are copied too, their streamed levels into the paired stream bundle.");
    pathField(*this, "Bundle or stream bundle##ms", t.msFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    const FileNames* sn = fileNames(t.msFile);
    comboField("Model to replace##ms", t.msModel, sn ? sn->models : kNone, "aid_model_...");
    pathField(*this, "Copy from##ms", t.msSource, PathPick::OpenFile, {{"All files", kAllFiles}}, "the same file");
    const FileNames* src = fileNames(trim(t.msSource).empty() ? t.msFile : t.msSource);
    comboField("Model to copy##ms", t.msSourceModel, src ? src->models : kNone, "aid_model_...");
    pathField(*this, "Write to##ms", t.msOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the file itself");
    pathField(*this, "Write the stream bundle to##ms", t.msStreamOut, PathPick::SaveFile, {{"All files", kAllFiles}},
              "the paired stream bundle itself");
    helpMarker("Where the copied textures' streamed levels go when the bundle is written elsewhere (Write to). Empty: the "
               "bundle's paired stream bundle (Bundle\\50, same name) is changed in place.");
    ImGui::Checkbox("XCTD-wrapped##ms", &t.msXctd);
    if (runButton(*this, "Swap model", !trim(t.msFile).empty() && !trim(t.msModel).empty() && !trim(t.msSourceModel).empty())) {
        fs::path file = optPath(t.msFile), source = trim(t.msSource).empty() ? file : optPath(t.msSource), out = optPath(t.msOut),
                 streamOut = optPath(t.msStreamOut);
        std::string name = trim(t.msModel), sourceName = trim(t.msSourceModel);
        bool xctd = t.msXctd;
        std::vector<fs::path> writes = {out.empty() ? file : out};
        writes.push_back(streamOut.empty() ? model::pairedStreamBundle(file) : streamOut);
        runTool("Swapping " + name, [file, name, source, sourceName, out, streamOut, xctd] {
            std::vector<std::string> a = {"model-swap", cliPath(file), name, cliPath(source), sourceName};
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (!streamOut.empty()) a.insert(a.end(), {"--stream-out", cliPath(streamOut)});
            if (xctd) a.push_back("--xctd");
            return a;
        }, writes);
    }

    ImGui::SeparatorText("Animation keys");
    note("Gives an animation new motion from a JSON timeline of poses (reNut's docs/nuttool/animations.md): bones without keys keep their own "
         "motion, and its length and frame count stay. A model tab's Replace motion does this to every copy.");
    pathField(*this, "Bundle or stream bundle##ai", t.aiFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    const FileNames* an = fileNames(t.aiFile);
    comboField("Animation##ai", t.aiAnim, an ? an->anims : kNone, "aid_anim_...");
    if (an && !an->error.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", an->error.c_str());
    pathField(*this, "Keys##ai", t.aiKeys, PathPick::OpenFile, {{"Animation timeline", "json"}}, "a .json timeline");
    pathField(*this, "Write to##ai", t.aiOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the file itself");
    ImGui::Checkbox("XCTD-wrapped##ai", &t.aiXctd);
    bool aiReady = !trim(t.aiFile).empty() && !trim(t.aiAnim).empty() && !trim(t.aiKeys).empty();
    if (runButton(*this, "Import keys", aiReady)) {
        fs::path file = optPath(t.aiFile), keys = optPath(t.aiKeys), out = optPath(t.aiOut);
        std::string name = trim(t.aiAnim);
        bool xctd = t.aiXctd;
        runTool("Importing " + name, [file, name, keys, out, xctd] {
            std::vector<std::string> a = {"anim-import", cliPath(file), name, cliPath(keys)};
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (xctd) a.push_back("--xctd");
            return a;
        }, {out.empty() ? file : out});
    }
    ImGui::End();
}

// ---- Tools > Game scripts and objparams ----------------------------------------------------------

void App::drawScriptTools() {
    ToolWindows& t = tools_;
    if (!t.showScripts) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Game scripts and objparams", &t.showScripts)) {
        ImGui::End();
        return;
    }
    note("Writes a bundle's game scripts (scene scripts, actor goals, script and state tables) or object parameters as text, one "
         "<asset>.txt each, and applies edited files back. Script and part tabs edit one asset in every bundle.");
    pathField(*this, "Bundle##sc", t.scBundle, PathPick::OpenFile, {{"All files", kAllFiles}});
    ImGui::RadioButton("Game scripts##sc", &t.scKind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Object parameters##sc", &t.scKind, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Vehicle parts only##sc", &t.scKind, 2);
    helpMarker("Vehicle parts are the parts' objparams, with named fields (mass, health, shop ratings...). Import takes any objparams files.");
    fs::path bundle = optPath(t.scBundle);
    bool scripts = t.scKind == 0;

    ImGui::SeparatorText("Export");
    ImGui::SetNextItemWidth(em(240));
    inputTextWithHint("Names containing##sc", "every asset", t.scName);
    std::string assetsHint = root_.empty() ? std::string("the bundle's assets folder") : toUtf8(root_);
    pathField(*this, "Asset names from##sc", t.scAssets, PathPick::Folder, {}, assetsHint.c_str());
    helpMarker("The assets folder whose Debug\\db_index.txt names the asset ids in the text (@aid_...). Empty: the open assets "
               "folder, else the bundle's own.");
    pathField(*this, "Folder##scdir", t.scDir, PathPick::Folder);
    if (runButton(*this, "Export##sc", !bundle.empty() && !trim(t.scDir).empty())) {
        fs::path dir = optPath(t.scDir), assets = trim(t.scAssets).empty() ? root_ : optPath(t.scAssets);
        std::string name = trim(t.scName);
        bool parts = t.scKind == 2;
        runTool(scripts ? "Exporting game scripts" : "Exporting objparams", [bundle, dir, assets, name, scripts, parts] {
            std::vector<std::string> a = {scripts ? "gamescript-export" : "objparams-export", cliPath(bundle), cliPath(dir)};
            if (parts) a.push_back("--parts");
            if (!name.empty()) a.insert(a.end(), {"--name", name});
            if (!assets.empty()) a.insert(a.end(), {"--assets", cliPath(assets)});
            return a;
        });
    }

    ImGui::SeparatorText("Import");
    ImGui::SetNextItemWidth(em(380));
    inputTextWithHint("##scin", "a .txt file, or a folder of them", t.scIn);
    ImGui::SameLine();
    if (ImGui::Button("File...##scin"))
        openFileDialog("Edited text", {{"Text", "txt"}}, [this](const fs::path& p) { tools_.scIn = toUtf8(p); }, optPath(t.scIn));
    ImGui::SameLine();
    if (ImGui::Button("Folder...##scin")) openFolderDialog("Folder of edited text", [this](const fs::path& p) { tools_.scIn = toUtf8(p); }, optPath(t.scIn));
    helpMarker("Each file is named after its asset (aid_script_....txt, aid_objparams_....txt), as Export names them; objparams "
               "fields that aren't listed keep their values.");
    pathField(*this, "Write to##scout", t.scOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the bundle itself");
    ImGui::Checkbox("XCTD-wrapped##sc", &t.scXctd);
    if (runButton(*this, "Import##sc", !bundle.empty() && !trim(t.scIn).empty())) {
        fs::path in = optPath(t.scIn), out = optPath(t.scOut);
        bool xctd = t.scXctd;
        runTool(scripts ? "Importing game scripts" : "Importing objparams", [bundle, in, out, xctd, scripts] {
            std::vector<std::string> a = {scripts ? "gamescript-import" : "objparams-import", cliPath(bundle), cliPath(in)};
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (xctd) a.push_back("--xctd");
            return a;
        }, {out.empty() ? bundle : out});
    }
    ImGui::End();
}

// ---- Tools > Inspect, extract and replace ---------------------------------------------------------

void App::drawFileJobs() {
    ToolWindows& t = tools_;
    if (!t.showFileJobs) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Inspect, extract and replace", &t.showFileJobs)) {
        ImGui::End();
        return;
    }
    note("The command line's smaller jobs, on any file in or out of the game folder. What they print goes to the log.");
    pathField(*this, "File##fj", t.fjFile, PathPick::OpenFile, {{"All files", kAllFiles}});
    fs::path file = optPath(t.fjFile);
    std::string label = toUtf8(file.filename());

    ImGui::SeparatorText("Inspect");
    if (runButton(*this, "Show info##fj", !file.empty())) {
        showLog_ = true;
        runTool("Info on " + label, [file] { return std::vector<std::string>{"info", cliPath(file)}; });
    }
    helpMarker("What the file is and what it holds: its kind, compression, sections, symbol or entry counts, a stream "
               "bundle's version, a movie's streams.");
    ImGui::SameLine();
    if (runButton(*this, "List contents##fj", !file.empty())) {
        showLog_ = true;
        runTool("Listing " + label, [file] { return std::vector<std::string>{"list", cliPath(file)}; });
    }
    helpMarker("Every symbol of a bundle (with its #index, section and size), every entry of a stream bundle and the "
               "bundles it depends on, or every wave of a wave bank (.xwb).");

    ImGui::SeparatorText("One symbol or entry");
    ImGui::SetNextItemWidth(em(380));
    inputTextWithHint("Symbol##fj", "#12, a name or part of one, or an entry's hex id", t.fjSymbol);
    helpMarker("A bundle's symbol by #index (as List contents numbers them) or by its name; a stream bundle's entry by "
               "#index or its hex id (Extract also takes a part of an entry's name).");
    ImGui::Checkbox("Only the .gpu section##fj", &t.fjGpu);
    helpMarker("For a texture or model whose name has a symbol in .data and one in .gpu: picks the .gpu one (its pixels or "
               "vertices).");
    std::string symbol = trim(t.fjSymbol);
    bool gpu = t.fjGpu;
    pathField(*this, "Extract to##fj", t.fjExtractOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "a file (a folder when several match)");
    if (runButton(*this, "Extract##fj", !file.empty() && !symbol.empty() && !trim(t.fjExtractOut).empty())) {
        fs::path out = optPath(t.fjExtractOut);
        runTool("Extracting " + symbol, [file, symbol, out, gpu] {
            std::vector<std::string> a = {"extract", cliPath(file), symbol, cliPath(out)};
            if (gpu) a.insert(a.end(), {"--section", ".gpu"});
            return a;
        });
    }
    pathField(*this, "Replacement##fj", t.fjData, PathPick::OpenFile, {{"All files", kAllFiles}}, "a file with the new bytes");
    pathField(*this, "Write to##fj", t.fjOut, PathPick::SaveFile, {{"All files", kAllFiles}}, "the file itself");
    ImGui::Checkbox("XCTD-wrapped##fj", &t.fjXctd);
    helpMarker("Wraps a rewritten bundle in an uncompressed XCTD container like the shipped files. The game reads plain bundles too.");
    if (runButton(*this, "Replace##fj", !file.empty() && !symbol.empty() && !trim(t.fjData).empty())) {
        fs::path data = optPath(t.fjData), out = optPath(t.fjOut);
        bool xctd = t.fjXctd;
        runTool("Replacing " + symbol, [file, symbol, data, out, gpu, xctd] {
            std::vector<std::string> a = {"replace", cliPath(file), symbol, cliPath(data)};
            if (!out.empty()) a.insert(a.end(), {"-o", cliPath(out)});
            if (gpu) a.insert(a.end(), {"--section", ".gpu"});
            if (xctd) a.push_back("--xctd");
            return a;
        }, {out.empty() ? file : out});
    }

    ImGui::SeparatorText("Fit chosen menus to resized textures");
    note("Tools > Fit menus to resized textures fixes every bundle with menus. This fixes only the bundles listed here, one per "
         "line, comparing textures with their .bak backups in the open assets folder and the title update folder.");
    inputTextMultiline("##fjmenus", t.fjMenuBundles, ImVec2(em(560), em(70)));
    ImGui::SameLine();
    if (ImGui::Button("Add...##fjmenus"))
        openFileDialog("Bundle with menus", {{"All files", kAllFiles}}, [this](const fs::path& p) {
            std::string& s = tools_.fjMenuBundles;
            if (!s.empty() && s.back() != '\n') s += '\n';
            s += toUtf8(p);
        }, root_);
    std::vector<fs::path> menuBundles;
    for (auto& l : split(t.fjMenuBundles, '\n'))
        if (!trim(l).empty()) menuBundles.push_back(optPath(l));
    if (root_.empty()) ImGui::TextDisabled("Open the game's assets folder first (File > Open assets folder).");
    if (runButton(*this, "Fit these menus", !root_.empty() && !menuBundles.empty())) {
        fs::path root = root_, update = updateRoot();
        runTool("Fitting menus", [root, update, menuBundles] {
            std::vector<std::string> a = {"menu-fix", cliPath(root)};
            for (auto& b : menuBundles) a.push_back(cliPath(b));
            if (!update.empty()) a.insert(a.end(), {"--update", cliPath(update)});
            return a;
        }, menuBundles);
    }

    ImGui::SeparatorText("Glyph cache from exported pages");
    note("Rebuilds a font's glyph cache (.sbp) from a folder a font tab's Export wrote (page PNGs and font.json). A font tab's "
         "Import pages does the same onto the font it shows.");
    pathField(*this, "Exported folder##fjfont", t.fjFontDir, PathPick::Folder);
    pathField(*this, "Glyph cache to write##fjfont", t.fjFontOut, PathPick::SaveFile, {{"Glyph cache", "sbp"}}, "a .sbp file");
    pathField(*this, "Based on##fjfont", t.fjFontBase, PathPick::OpenFile, {{"Glyph cache", "sbp"}}, "optional: the original .sbp");
    helpMarker("The original glyph cache: glyphs whose pixels didn't change keep its compressed data, so the file stays close "
               "to its size. Empty: every glyph is built from the pages.");
    if (runButton(*this, "Rebuild glyph cache", !trim(t.fjFontDir).empty() && !trim(t.fjFontOut).empty())) {
        fs::path dir = optPath(t.fjFontDir), out = optPath(t.fjFontOut), base = optPath(t.fjFontBase);
        runTool("Rebuilding " + toUtf8(out.filename()), [dir, out, base] {
            std::vector<std::string> a = {"font-import", cliPath(dir), cliPath(out)};
            if (!base.empty()) a.insert(a.end(), {"--base", cliPath(base)});
            return a;
        }, {out});
    }
    ImGui::End();
}

// ---- Tools > Replace an actor --------------------------------------------------------------------

namespace {

std::string shortActor(const std::string& n) {
    const std::string p = "aid_objparams_banjox_actor_";
    return startsWith(n, p) ? n.substr(p.size()) : n;
}

std::string shortAnim(const std::string& n) {
    for (const char* p : {"aid_anim_banjox_actor_", "aid_anim_banjox_", "aid_anim_"})
        if (startsWith(n, p)) return n.substr(std::string(p).size());
    return n;
}

// The actors in a combo with a search box; true when `chosen` changed.
bool actorCombo(const char* label, const actorswap::Index& ix, std::string& chosen, std::string& filter) {
    bool changed = false;
    ImGui::SetNextItemWidth(em(320));
    if (ImGui::BeginCombo(label, chosen.empty() ? "(choose)" : shortActor(chosen).c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        inputTextWithHint("##filter", "search", filter);
        const std::string f = lower(filter);
        for (const actorswap::Actor& a : ix.actors) {
            if (!f.empty() && lower(a.name).find(f) == std::string::npos) continue;
            if (ImGui::Selectable(shortActor(a.name).c_str(), a.name == chosen)) {
                chosen = a.name;
                changed = true;
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("%s\nmodel %s\ntable %s, %zu entries", a.name.c_str(), ix.nameOf(a.model).c_str(), ix.nameOf(a.table).c_str(),
                            a.entries.size());
                ImGui::EndTooltip();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

void App::drawActorSwap() {
    ToolWindows& t = tools_;
    if (!t.showActor) return;
    ImGui::SetNextWindowSize(ImVec2(em(760), em(600)), ImGuiCond_Appearing);
    if (!ImGui::Begin("Replace an actor", &t.showActor)) {
        ImGui::End();
        return;
    }
    note("Makes one of the game's actors another: it draws the other's model with its own rig, and its animation table plays the "
         "other's animations, picked entry by entry. Its moves, body and attacks stay its own: Banjo replaced by Klungo looks and "
         "animates like Klungo but plays as Banjo. The bundles are written straight away with .bak backups; Tools > Restore "
         "original files undoes it.");
    if (!t.actorIndex) {
        if (root_.empty()) ImGui::TextDisabled("Open the game's assets folder first (File > Open assets folder).");
        if (runButton(*this, "Read the game's actors", !root_.empty())) {
            fs::path root = root_, update = updateRoot();
            runReadOnlyTask("Reading the game's actors", true, [this, root, update](Task& task) -> std::function<void()> {
                auto ix = std::make_shared<actorswap::Index>(actorswap::scan(root, update, [&task](const std::string& s) { task.report(-1, s); }));
                return [this, ix] {
                    tools_.actorIndex = ix;
                    status(std::to_string(ix->actors.size()) + " actors read");
                };
            });
        }
        helpMarker("Reads every bundle once (a minute or two) for each actor, its model, its animation table and where they are.");
        ImGui::End();
        return;
    }
    const actorswap::Index& ix = *t.actorIndex;
    actorCombo("Replace##astarget", ix, t.asTarget, t.asTargetFilter);
    helpMarker("The actor that changes; the game spawns it as before. banjotux is the player's Banjo.");
    ImGui::RadioButton("With a game actor##asmode", &t.asMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("With a rigged .glb of your own##asmode", &t.asMode, 1);
    const bool own = t.asMode == 1;
    if (!own) {
        actorCombo("With##assource", ix, t.asSource, t.asSourceFilter);
        helpMarker("The actor whose model, rig and animations it takes. npc_klungo is Klungo.");
    } else {
        pathField(*this, "Model##asglb", t.asGlb, PathPick::OpenFile, {{"glTF binary", "glb"}}, "a rigged .glb with its animations");
        helpMarker("Its skinned meshes on its own skeleton (at most 255 bones), their base colour textures, and every animation "
                   "it holds for that skeleton (Blender: export glTF Binary with the armature, skinning and animations). Each "
                   "entry the actor plays gets one of its animations.");
        ImGui::SetNextItemWidth(em(120));
        ImGui::DragFloat("Scale##asglb", &t.asScale, 0.01f, 0.01f, 100.0f, "%.3g");
        helpMarker("Times the file's size; the game's units are metres (Banjo is about 1.2 tall).");
        if (t.asGlb != t.asGlbSeen) {
            t.asGlbSeen = t.asGlb;
            t.asGlbAnims.clear();
            t.asGlbError.clear();
            if (!trim(t.asGlb).empty()) {
                try {
                    t.asGlbAnims = actorswap::glbAnimations(optPath(t.asGlb));
                    if (t.asGlbAnims.empty()) t.asGlbError = "it has no animations for its skeleton";
                } catch (const std::exception& e) {
                    t.asGlbError = e.what();
                }
            }
        }
        if (!t.asGlbError.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", t.asGlbError.c_str());
    }
    const actorswap::Actor* target = ix.actor(t.asTarget);
    const actorswap::Actor* source = own ? nullptr : ix.actor(t.asSource);
    if (!target || (!own && (!source || target == source)) || (own && t.asGlbAnims.empty())) {
        ImGui::TextDisabled(target && target == source ? "Pick two different actors."
                            : own                     ? "Pick the actor and a rigged .glb with animations."
                                                      : "Pick both actors.");
        ImGui::End();
        return;
    }
    const std::string pair = t.asTarget + "|" + (own ? t.asGlb : t.asSource);
    auto match = [&] { return own ? actorswap::autoMatchGlb(*target, t.asGlbAnims) : actorswap::autoMatch(*target, *source, ix); };
    if (t.asMatched != pair) {
        t.asAnims = match();
        t.asMatched = pair;
    }
    std::string sharing;
    for (const actorswap::Actor& a : ix.actors)
        if (&a != target && a.table == target->table) sharing += (sharing.empty() ? "" : ", ") + shortActor(a.name);
    if (!sharing.empty())
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s",
                           ("Its animation table is also " + sharing + "'s: they draw the new model too.").c_str());
    if (own && t.asEvents == 1) t.asEvents = 0;
    ImGui::RadioButton("Keep its animation events##as", &t.asEvents, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(own);
    ImGui::RadioButton("Use the other's##as", &t.asEvents, 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::RadioButton("None##as", &t.asEvents, 2);
    helpMarker("Animation events are what happens at moments of an animation: footstep sounds, effects, the frame an attack hits. "
               "Keeping the actor's own keeps its gameplay timing; the other's fit the new animations; none silences them.");

    ImGui::SeparatorText("Animations");
    const std::string sourceLabel = own ? toUtf8(optPath(t.asGlb).filename()) : shortActor(source->name);
    ImGui::TextDisabled("Each entry the game plays by name gets one of %s's animations.", sourceLabel.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Match again")) t.asAnims = match();
    helpMarker("Each entry takes the animation of the same name (ignoring case and underscores), else a stand or idle. Every "
               "entry needs one: the game animates a model only with animations made for its skeleton.");
    const std::vector<std::string> lend = own ? t.asGlbAnims : actorswap::sourceAnims(*source, ix);
    size_t missing = 0;
    const float tableH = std::max(em(120), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("##asanims", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("Played");
        ImGui::TableSetupColumn("Plays now");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < target->entries.size(); ++i) {
            const actorswap::Entry& e = target->entries[i];
            if (!e.anim) continue;
            std::string& cur = t.asAnims[e.name];
            missing += cur.empty();
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", shortAnim(ix.nameOf(e.anim)).c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##anim", cur.empty() ? "(choose)" : shortAnim(cur).c_str(), ImGuiComboFlags_HeightLarge)) {
                for (const std::string& n : lend)
                    if (ImGui::Selectable(shortAnim(n).c_str(), n == cur)) cur = n;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (runButton(*this, "Replace", missing == 0)) {
        actorswap::Plan plan;
        plan.target = t.asTarget;
        if (own) {
            plan.glb = optPath(t.asGlb);
            plan.scale = t.asScale;
        } else {
            plan.source = t.asSource;
        }
        plan.anims = t.asAnims;
        plan.events = t.asEvents == 1 ? actorswap::Events::Source : t.asEvents == 2 ? actorswap::Events::None : actorswap::Events::Keep;
        // The bundles it writes: those holding the actors drawing the model, and the table.
        std::vector<fs::path> writes;
        std::vector<uint32_t> ids{target->table};
        for (const actorswap::Actor& a : ix.actors)
            if (&a == target || a.table == target->table)
                if (auto id = assetdb::idFromName(a.name)) ids.push_back(*id);
        for (uint32_t id : ids)
            if (auto h = ix.holders.find(id); h != ix.holders.end())
                for (size_t k : h->second) writes.push_back(ix.bundles[k]);
        if (checkUnsaved(writes)) {
            auto index = t.actorIndex;
            runTask("Replacing " + shortActor(plan.target), true, [this, index, plan](Task& task) -> std::function<void()> {
                std::vector<std::string> said;
                const std::vector<fs::path> files = actorswap::apply(*index, plan, [&](const std::string& s) {
                    said.push_back(s);
                    task.report(-1, s);
                });
                return [this, files, said, plan] {
                    for (const std::string& s : said) log(s);
                    tools_.actorIndex.reset();  // the bundles changed: read them again before another swap
                    tools_.asMatched.clear();
                    reloadWritten(files);
                    const std::string now = plan.glb.empty() ? shortActor(plan.source) : toUtf8(plan.glb.filename());
                    status(shortActor(plan.target) + " is now " + now + " (" + std::to_string(files.size()) + " file(s) written)");
                };
            });
        }
    }
    if (missing) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu entr%s without an animation", missing, missing == 1 ? "y" : "ies");
    }
    ImGui::End();
}

// ---- reNut ------------------------------------------------------------------------------------

void App::drawPartPicker() {
    ToolWindows& t = tools_;
    if (!t.showPartPicker) return;
    ImGui::SetNextWindowSize(ImVec2(em(520), em(480)), ImGuiCond_Appearing);
    if (!ImGui::Begin("New reNut part", &t.showPartPicker)) {
        ImGui::End();
        return;
    }
    note("A new part for reNut starts from a shipped one, which stays as it is. Pick it: its tab opens with Save as reNut part, "
         "where the new part gets its id, fields, and its own model and footprint if you want.");
    if (!catalog_) {
        ImGui::TextDisabled(indexing_ ? "Indexing the game's parts..." : "Open the assets folder first (File menu).");
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    inputTextWithHint("##partfilter", "Search parts...", t.partFilter);
    std::string needle = lower(trim(t.partFilter));
    if (ImGui::BeginChild("##parts", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (auto& p : catalog_->parts) {
            if (!needle.empty() && lower(p.name).find(needle) == std::string::npos) continue;
            std::string label = renutpart::baseName(p.name);
            for (const char* prefix : {"aid_objparams_banjoxdlc_vehicleblock_", "aid_objparams_"})
                if (startsWith(label, prefix)) label = label.substr(std::string(prefix).size());
            ImGui::PushID(p.name.c_str());
            if (ImGui::Selectable(label.c_str())) {
                requestPartDialog(p.name);
                openScript(p.name);
                t.showPartPicker = false;
            }
            ImGui::SetItemTooltip("%s", p.name.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::drawBlockTool() {
    ToolWindows& t = tools_;
    if (!t.showBlock) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("New reNut block", &t.showBlock)) {
        ImGui::End();
        return;
    }
    note("A block of its own for reNut, made from scratch (nothing taken from the game): a model of one .glb node's mesh, drawn "
         "in the part's paint colour by reNut's part shaders, a one-cell footprint, and scripts\\<id>.lua calling "
         "renut.parts.add, in a RexFabric Lua mod folder.");
    pathField(*this, "Mod folder##bl", t.blMod, PathPick::Folder, {}, "mods\\<your mod>");
    ImGui::SetNextItemWidth(em(260));
    inputText("Block id##bl", t.blId);
    std::string id = lower(trim(t.blId));
    helpMarker("1-40 characters of a-z, 0-9 and _, unique across every mod: start it with your mod's name. Vehicles keep parts by "
               "id, so don't change it once players have the block.");
    if (!id.empty() && !renutpart::validId(id)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "a-z, 0-9 and _ only, at most 40");
    }
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Garage name##bl", "the id", t.blName);
    pathField(*this, "Model##bl", t.blModel, PathPick::OpenFile, {{"Binary glTF", "glb"}}, "a .glb file");
    glbNodes(t.blModel, t.blNodesSeen, t.blNodes, t.blNodesError);
    comboField("Mesh node##bl", t.blMesh, t.blNodes, "the node whose mesh it gets");
    if (!t.blNodesError.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", t.blNodesError.c_str());

    ImGui::SeparatorText("Footprint");
    static const char* const shapes[] = {"cube", "wedge", "corner"};
    int shape = 0;
    for (int i = 0; i < 3; ++i)
        if (t.blShape == shapes[i]) shape = i;
    ImGui::SetNextItemWidth(em(140));
    if (ImGui::Combo("Collision shape##bl", &shape, shapes, IM_ARRAYSIZE(shapes))) t.blShape = shapes[shape];
    helpMarker("How it collides, as the game's body blocks do: a cube, a wedge or a corner.");
    comboField("Attachable faces##bl", t.blAttach, kAttachable, "all", em(260));
    helpMarker("The faces that take other parts: all, none, top (it goes under a block and hangs down), bottom, sides, top+sides, "
               "or a list of directions such as -x,-y,+z.");

    ImGui::SeparatorText("Garage");
    comboField("Category##bl", t.blCategory, kCategories, "body", em(260));
    helpMarker("The garage category it is listed in: body, gadget, seat...");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Type##bl", "a type of its own", t.blType);
    helpMarker("A garage type it shares: every block naming the same type is a size (shape) in it.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Mass##bl", &t.blMass, 0, 0, "%g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Health##bl", &t.blHealth, 0, 0, "%g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputInt("How many the player owns##bl", &t.blCount);
    t.blCount = std::clamp(t.blCount, 1, 99);

    ImGui::SetNextItemWidth(em(320));
    inputTextWithHint("Shape key weights##bl", "optional: name=weight, ...", t.blShapes);
    helpMarker("The mesh's shape keys (morph targets, from Blender's shape keys) come with it. These set where they start, 0 "
               "to 1 (else the .glb's own); Lua moves them with part:set_shape(name, weight), for every one of this block.");

    ImGui::SeparatorText("Mod");
    std::string bundleHint;
    try {
        if (!trim(t.blMod).empty()) bundleHint = renutmake::defaultBundle(optPath(t.blMod));
    } catch (std::exception&) {
    }
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Stream bundle name##bl", bundleHint.empty() ? "the mod folder's name" : bundleHint.c_str(), t.blBundle);
    helpMarker("The model and footprint go into the mod's own stream bundle, <mod folder>\\bundles\\Bundle\\50\\<id of "
               "aid_streambundle_banjox_<name>>, which reNut loads with the game's common bundles.");
    ImGui::Checkbox("Replace the script if it exists##bl", &t.blForce);
    helpMarker("Off: making the block again rebuilds its model and footprint and keeps the script you may have edited.");
    bool ready = !trim(t.blMod).empty() && renutpart::validId(id) && !trim(t.blModel).empty() && !trim(t.blMesh).empty();
    if (runButton(*this, "Make block", ready)) {
        renutmake::Block b;
        b.id = id;
        b.name = trim(t.blName);
        b.model = optPath(t.blModel);
        b.mesh = t.blMesh;
        b.shape = t.blShape;
        b.attachable = trim(t.blAttach);
        b.category = trim(t.blCategory);
        b.type = trim(t.blType);
        b.mass = num(t.blMass);
        b.health = num(t.blHealth);
        b.count = std::to_string(t.blCount);
        b.bundle = trim(t.blBundle);
        b.shapeWeights = shapeWeightsText(t.blShapes);
        fs::path mod = optPath(t.blMod);
        bool force = t.blForce;
        runTask("Making block " + id, true, [this, mod, b, force](Task&) -> std::function<void()> {
            renutmake::block(mod, b, force, [this](const std::string& s) { log(s); });
            return [this, mod, id = b.id] { status("Made block " + id + " in " + toUtf8(mod) + " (see the log)"); };
        });
    }
    ImGui::End();
}

namespace {

const char* const kNpcLines[] = {"greeting", "objectives", "win", "lose", "completed"};
const ImVec4 kProblem(1, 0.6f, 0.3f, 1);

// The tag a character gets when none is typed: renut_<world>_<id>, as reNut's Lua defaults it.
std::string defaultTag(const std::string& world, const std::string& npc) {
    return "renut_" + (world.empty() ? std::string() : world + "_") + npc;
}

// The highest speaker slot no character in `list` has, else 32.
int freeSpeaker(const std::vector<NpcEdit>& list) {
    for (int s = 32; s >= 1; --s) {
        bool used = false;
        for (const NpcEdit& o : list) used |= o.speaker == s;
        if (!used) return s;
    }
    return 32;
}

// The editor's character as renutnpc has it, in the world `world`.
renutnpc::Npc toNpc(const NpcEdit& e, const std::string& world) {
    renutnpc::Npc n;
    n.id = lower(trim(e.id));
    n.name = trim(e.name).empty() ? n.id : trim(e.name);
    n.model = optPath(e.model);
    n.scale = e.scale;
    n.smoothDegrees = e.smooth;
    n.colourScale = e.colourScale;
    n.position = {e.position[0], e.position[1], e.position[2]};
    n.yaw = e.yaw;
    n.rig = trim(e.rig).empty() ? std::string("auto") : trim(e.rig);
    n.idle = trim(e.idle);
    if (n.idle != "breathe") {
        // renutnpc reads a timeline's path in the system code page; npcProblems says when it can't be.
        try {
            n.idle = cliPath(optPath(n.idle));
        } catch (std::exception&) {
        }
    }
    n.tag = trim(e.tag).empty() ? defaultTag(world, n.id) : lower(trim(e.tag));
    n.speaker = e.speaker;
    for (size_t i = 0; i < 5; ++i)
        if (!trim(e.lines[i]).empty()) n.lines[kNpcLines[i]] = trim(e.lines[i]);
    if (e.challenge) {
        renutnpc::Challenge c;
        c.id = lower(trim(e.chId));
        c.name = trim(e.chName);
        c.summary = trim(e.chSummary);
        c.goal = {e.goal[0], e.goal[1], e.goal[2]};
        c.goalYaw = e.goalYaw;
        c.gate = {e.gate[0], e.gate[1]};
        c.timeLimit = e.timeLimit;
        c.tt = e.tt;
        c.jiggy = e.jiggy;
        c.notes = uint32_t(std::max(e.notes, 0));
        c.countDown = e.countDown;
        if (e.startSet) c.start = std::array<float, 3>{e.start[0], e.start[1], e.start[2]};
        if (e.startYawSet) c.startYaw = e.startYaw;
        n.challenge = c;
    }
    if (e.voiceMode == 1) n.voice = lower(trim(e.gameVoice));
    if (e.voiceMode == 2)
        for (const std::string& f : e.voiceClips)
            if (!trim(f).empty()) n.voiceClips.push_back(optPath(f));
    n.voiceVolume = e.voiceVolume;
    for (size_t i = 0; i < 5; ++i)
        if (!trim(e.sounds[i]).empty()) n.sounds[kNpcLines[i]] = optPath(e.sounds[i]);
    for (const std::string& x : split(e.exclude, ','))
        if (!trim(x).empty()) n.exclude.push_back(trim(x));
    n.armsDown = e.armsDown;
    n.shapeWeights = shapeWeightsText(e.shapes);
    if (e.enemy) {
        n.enemy = renutnpc::Enemy{e.enemyHealth, e.enemyBehaviour == 0 ? "mirror" : "none"};
        n.challenge.reset();
    }
    return n;
}

NpcEdit fromNpc(const renutnpc::Npc& n) {
    NpcEdit e;
    e.id = n.id;
    e.name = n.name;
    e.model = toUtf8(n.model);
    e.scale = n.scale;
    e.smooth = n.smoothDegrees;
    e.colourScale = n.colourScale;
    for (size_t k = 0; k < 3; ++k) e.position[k] = n.position[k];
    e.yaw = n.yaw;
    e.rig = n.rig;
    e.idle = n.idle == "breathe" ? n.idle : toUtf8(fs::path(n.idle));
    e.tag = n.tag;
    e.speaker = n.speaker;
    for (size_t i = 0; i < 5; ++i) {
        auto it = n.lines.find(kNpcLines[i]);
        if (it != n.lines.end()) e.lines[i] = it->second;
    }
    if (n.challenge) {
        const renutnpc::Challenge& c = *n.challenge;
        e.challenge = true;
        e.chId = c.id;
        e.chName = c.name;
        e.chSummary = c.summary;
        for (size_t k = 0; k < 3; ++k) e.goal[k] = c.goal[k];
        e.goalYaw = c.goalYaw;
        e.gate[0] = c.gate[0];
        e.gate[1] = c.gate[1];
        e.timeLimit = c.timeLimit;
        e.tt = c.tt;
        e.jiggy = c.jiggy;
        e.notes = int(c.notes);
        e.countDown = c.countDown;
        e.startSet = c.start.has_value();
        if (c.start)
            for (size_t k = 0; k < 3; ++k) e.start[k] = (*c.start)[k];
        e.startYawSet = c.startYaw.has_value();
        if (c.startYaw) e.startYaw = *c.startYaw;
    }
    e.voiceMode = !n.voiceClips.empty() ? 2 : !n.voice.empty() ? 1 : 0;
    if (!n.voice.empty()) e.gameVoice = n.voice;
    for (const fs::path& f : n.voiceClips) e.voiceClips.push_back(toUtf8(f));
    e.voiceVolume = n.voiceVolume;
    for (size_t i = 0; i < 5; ++i) {
        auto it = n.sounds.find(kNpcLines[i]);
        if (it != n.sounds.end()) e.sounds[i] = toUtf8(it->second);
    }
    for (const std::string& x : n.exclude) e.exclude += (e.exclude.empty() ? "" : ", ") + x;
    e.armsDown = n.armsDown;
    e.shapes = shapeWeightsText(n.shapeWeights);
    if (n.enemy) {
        e.enemy = true;
        e.enemyHealth = n.enemy->health;
        e.enemyBehaviour = n.enemy->behaviour == "mirror" ? 0 : 1;
    }
    return e;
}

// What stops character `i` of `list` being made: the loader's checks, its files, and clashes with the others.
std::vector<std::string> npcProblems(const std::vector<NpcEdit>& list, size_t i, const std::string& world) {
    const NpcEdit& e = list[i];
    const renutnpc::Npc n = toNpc(e, world);
    std::vector<std::string> out = renutnpc::problems(n);
    std::error_code ec;
    if (!n.model.empty()) {
        const std::string ext = lower(toUtf8(n.model.extension()));
        if (ext != ".dae" && ext != ".glb" && ext != ".obj") out.push_back("the model is a .dae, .glb or .obj file");
        else if (!fs::is_regular_file(n.model, ec)) out.push_back("the model file isn't there");
    }
    if (trim(e.idle) != "breathe" && !trim(e.idle).empty()) {
        fs::path idle = optPath(e.idle);
        if (!fs::is_regular_file(idle, ec)) out.push_back("the idle's timeline file isn't there");
        try {
            cliPath(idle);
        } catch (std::exception& x) {
            out.push_back(x.what());
        }
    }
    if (e.voiceMode == 2 && n.voiceClips.empty()) out.push_back("their own voice needs at least one sound file");
    for (const fs::path& f : n.voiceClips)
        if (!fs::is_regular_file(f, ec)) out.push_back("the voice clip " + toUtf8(f.filename()) + " isn't there");
    for (const auto& [line, f] : n.sounds)
        if (!fs::is_regular_file(f, ec)) out.push_back("the " + line + " sound file isn't there");
    for (size_t k = 0; k < list.size(); ++k) {
        if (k == i) continue;
        const NpcEdit& o = list[k];
        const std::string otherId = lower(trim(o.id)), otherTag = trim(o.tag).empty() ? defaultTag(world, otherId) : lower(trim(o.tag));
        if (!n.id.empty() && otherId == n.id) out.push_back("another character has this id");
        if (!n.id.empty() && otherTag == n.tag) out.push_back("another character has this tag");
        if (o.speaker == e.speaker) out.push_back("another character has speaker slot " + std::to_string(e.speaker) + " (each needs its own)");
        if (e.challenge && o.challenge && !trim(e.chId).empty() && lower(trim(o.chId)) == lower(trim(e.chId)))
            out.push_back("another character's challenge has this id");
    }
    return out;
}

} // namespace

void App::drawWorldTool() {
    ToolWindows& t = tools_;
    if (!t.showWorld) return;
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("New reNut world", &t.showWorld)) {
        ImGui::End();
        return;
    }
    note("A world of its own for reNut, from every mesh of a .glb, .obj or .dae (textured by its materials): its model, textures, sky and collision, a "
         "player start and a door home, its scene, and scripts\\renutworld_<id>.lua declaring it and its door in Showdown Town, "
         "in a RexFabric Lua mod folder. The collision's Havok class tables come from the game's car park, so an assets folder "
         "must be open.");
    if (root_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Open the game's assets folder first (File > Open assets folder).");
    pathField(*this, "Mod folder##wd", t.wdMod, PathPick::Folder, {}, "mods\\<your mod>");
    ImGui::SetNextItemWidth(em(260));
    inputText("World id##wd", t.wdId);
    std::string id = lower(trim(t.wdId));
    helpMarker("1-40 characters of a-z, 0-9 and _, unique across every mod: start it with your mod's name.");
    if (!id.empty() && !renutpart::validId(id)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "a-z, 0-9 and _ only, at most 40");
    }
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Name##wd", "the id", t.wdName);
    pathField(*this, "Model##wd", t.wdModel, PathPick::OpenFile,
              {{"glTF", "glb;gltf"}, {"Wavefront OBJ", "obj"}, {"COLLADA", "dae"}}, "a .glb, .gltf, .obj or .dae file");
    helpMarker("Every mesh of the file, in world space (1 unit is 1 metre, y up), with its materials' textures (a glTF's "
               "base colour textures, an OBJ's map_Kd from its .mtl, a .dae's diffuse textures, read beside the file) at their "
               "texture coordinates, repeating, times the vertex colours; one draw per texture. Materials without a texture show "
               "their colour; texels under half alpha are cut out.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Smoothing angle##wd", &t.wdSmooth, 0, 0, "%g");
    helpMarker("Faces meeting at less than this many degrees share a normal (look smooth).");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Scale##wd", &t.wdScale, 0, 0, "%g");
    helpMarker("The model's size, times this: for a model made in other units than the game's metres.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Vertex colour scale##wd", &t.wdColourScale, 0, 0, "%g");
    helpMarker("The vertex colours times this (at most white): 2 for 3DS rips, whose vertex colours are stored at half.");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Water##wd", "e.g. riv, suiro (optional)", t.wdWater);
    helpMarker("Objects named with _water become water you can swim in and float on. For a model whose objects you "
               "can't rename, list parts of object, material or texture names here, separated by commas: what matches "
               "is water too. Each connected piece is a body of water (8 at most), its surface at its highest point.");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Shape key weights##wd", "optional: name=weight, ...", t.wdShapes);
    helpMarker("A .glb's shape keys (morph targets) come with the world. These set where they start, 0 to 1 (else the "
               ".glb's own); Lua moves them with renut.worlds.set_shape(world, name, weight).");
    if (ImGui::TreeNode("Materials##wd")) {
        ImGui::BeginDisabled(trim(t.wdModel).empty());
        if (ImGui::Button("Read from the model##wdmat")) {
            const fs::path model = optPath(t.wdModel), saved = optPath(t.wdMod) / "materials.json";
            runReadOnlyTask("Reading the model's materials", true, [this, model, saved](Task&) -> std::function<void()> {
                std::vector<MaterialEdit> list;
                std::string note;
                try {
                    // The model's own, with the mod's saved settings over them.
                    std::map<std::string, renutworld::MaterialSettings> savedSettings;
                    std::error_code ec;
                    if (!saved.parent_path().empty() && fs::is_regular_file(saved, ec)) savedSettings = renutworld::loadMaterials(saved);
                    for (auto& [name, s] : renutworld::modelMaterials(model)) {
                        auto it = savedSettings.find(name);
                        list.push_back(materialEdit(name, it != savedSettings.end() ? it->second : s));
                    }
                    note = std::to_string(list.size()) + " material(s)" + (savedSettings.empty() ? "" : ", with materials.json's settings");
                } catch (std::exception& e) {
                    note = std::string("Couldn't read them: ") + e.what();
                }
                return [this, list, note] {
                    tools_.wdMaterials = list;
                    tools_.wdMaterialsNote = note;
                };
            });
        }
        ImGui::EndDisabled();
        helpMarker("Lists the model's materials with how the file sets them (and the mod's materials.json over them, when "
                   "there is one). Their settings go into <mod folder>\\materials.json when the world is made, and that file "
                   "is used whenever it is there (delete it for the model's own).");
        if (!t.wdMaterials.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Close the list##wdmat")) t.wdMaterials.clear(), t.wdMaterialsNote.clear();
        }
        if (!t.wdMaterialsNote.empty()) ImGui::TextWrapped("%s", t.wdMaterialsNote.c_str());
        static const char* const kAlphaNames[] = {"Opaque", "Cut out", "Blend", "Add"};
        for (size_t i = 0; i < t.wdMaterials.size(); ++i) {
            MaterialEdit& m = t.wdMaterials[i];
            ImGui::PushID(int(i));
            if (ImGui::TreeNode(m.name.c_str())) {
                ImGui::SetNextItemWidth(em(120));
                ImGui::Combo("Alpha##mat", &m.alpha, kAlphaNames, 4);
                helpMarker("How its texture's alpha shows. Opaque: not at all. Cut out: what is under the cutoff isn't drawn "
                           "(leaves, fences). Blend: see-through by its alpha (glass, water). Add: added to what is behind "
                           "(light beams, fire). See-through ones are drawn after the solid ones, casting no shadow.");
                if (m.alpha == 1) {
                    ImGui::SetNextItemWidth(em(160));
                    ImGui::SliderFloat("Cutoff##mat", &m.cutoff, 0.0f, 1.0f, "%.2f");
                }
                ImGui::SetNextItemWidth(em(160));
                ImGui::SliderFloat("Opacity##mat", &m.opacity, 0.0f, 1.0f, "%.2f");
                helpMarker("Its alpha times this: lower is more see-through (Blend, Add) or more cut out.");
                ImGui::ColorEdit3("Emissive##mat", m.emissive, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(em(120));
                ImGui::DragFloat("Strength##mat", &m.strength, 0.05f, 0.0f, 16.0f, "%.2f");
                helpMarker("Light it gives off: the colour times the strength, unlit, over its own lit colour (black: none). "
                           "Above 1 it is brighter than white, for the bloom.");
                pathField(*this, "Emissive texture##mat", m.emissiveTexture, PathPick::OpenFile,
                          {{"Images", image::canDecodeImages() ? "png;jpg;jpeg;bmp;tga" : "png"}}, "optional: the model's own");
                helpMarker("An image the emissive colour is multiplied by, at the model's texture coordinates (a lit window "
                           "mask). Empty: the model's own emissive texture, if it has one.");
                ImGui::Checkbox("Automatic glow##mat", &m.autoGlow);
                helpMarker("The game's bloom: full where it gives off light, else as the game's own surfaces. Off: set it "
                           "yourself, 0 (no glow) to 1.");
                if (!m.autoGlow) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(em(120));
                    ImGui::SliderFloat("Glow##mat", &m.glow, 0.0f, 1.0f, "%.2f");
                }
                ImGui::Checkbox("Double-sided##mat", &m.doubleSided);
                helpMarker("Both sides drawn (thin leaves, flags, fences seen from behind).");
                ImGui::SameLine();
                ImGui::Checkbox("Unlit##mat", &m.unlit);
                helpMarker("Shown at its full colour, without the game's light and shade.");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    {
        static const char* const kSides[] = {"256", "512", "1024", "2048"};
        int pick = t.wdMaxTexture <= 256 ? 0 : t.wdMaxTexture <= 512 ? 1 : t.wdMaxTexture <= 1024 ? 2 : 3;
        ImGui::SetNextItemWidth(em(120));
        if (ImGui::Combo("Largest texture##wd", &pick, kSides, 4)) t.wdMaxTexture = 256 << pick;
        helpMarker("Textures bigger than this are shrunk to it. A world with hundreds of textures needs 512 (or less) to "
                   "fit in the game's memory; a small one can keep 1024.");
    }
    pathField(*this, "Music##wd", t.wdMusic, PathPick::OpenFile, {{"Music", "ogg;mp3;flac;wav"}}, "optional: an .ogg, .mp3, .flac or .wav");
    helpMarker("A song that loops while you're in the world, instead of the level's own music. It's copied into the mod's "
               "music folder. It pauses with the game, and while a challenge's song or a Stereo plays.");
    if (!trim(t.wdMusic).empty()) {
        ImGui::SetNextItemWidth(em(160));
        ImGui::SliderFloat("Music volume##wd", &t.wdMusicVolume, 0.0f, 1.0f, "%.2f");
    }
    ImGui::TextUnformatted("Sky:");
    ImGui::SameLine();
    ImGui::RadioButton("None##wdsky", &t.wdSkyMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Panorama##wdsky", &t.wdSkyMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Gradient##wdsky", &t.wdSkyMode, 2);
    helpMarker("A sky around the camera, drawn behind everything and unlit (the game's sky slot, as its own levels set it). "
               "A panorama is a 360-degree (equirectangular) image, its top straight up; a gradient runs from its overhead "
               "colour to its horizon colour.");
    if (t.wdSkyMode == 1)
        pathField(*this, "Sky panorama##wd", t.wdSkyImage, PathPick::OpenFile,
                  {{"Images", image::canDecodeImages() ? "png;jpg;jpeg;bmp;tga" : "png"}}, "a 360-degree image");
    if (t.wdSkyMode == 2) {
        ImGui::ColorEdit3("Overhead##wdsky", t.wdSkyTop, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Horizon##wdsky", t.wdSkyHorizon, ImGuiColorEditFlags_NoInputs);
    }
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Ground size##wd", &t.wdGround, 0, 0, "%g");
    helpMarker("A flat square ground this many units wide under the model (at its lowest point); 0 for none.");
    if (t.wdGround > 0) {
        ImGui::SameLine();
        ImGui::ColorEdit3("Ground colour##wd", t.wdGroundColour, ImGuiColorEditFlags_NoInputs);
    }

    ImGui::SeparatorText("In the world");
    ImGui::Checkbox("Player start##wd", &t.wdSpawnSet);
    helpMarker("Off: 2 m above the ground nearest the middle of the model (a solid, roughly flat spot).");
    if (t.wdSpawnSet) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(260));
        ImGui::InputFloat3("##wdspawn", t.wdSpawn, "%g");
    }
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Start facing (degrees)##wd", &t.wdSpawnYaw, 0, 0, "%g");
    ImGui::Checkbox("Door home##wd", &t.wdExitSet);
    helpMarker("The door back to Showdown Town (the car park's stone arch). Off: on the ground 10-15 m from the player start, "
               "facing it (unless a facing is given).");
    if (t.wdExitSet) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(260));
        ImGui::InputFloat3("##wdexit", t.wdExit, "%g");
    }
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Door home facing (degrees)##wd", &t.wdExitYaw, 0, 0, "%g");

    ImGui::SeparatorText("In Showdown Town");
    ImGui::SetNextItemWidth(em(260));
    ImGui::InputFloat3("Its door##wd", t.wdDoor, "%g");
    helpMarker("Where the door into the world stands in Showdown Town (beside Mumbo's Motors' door by default). The script "
               "can be edited afterwards to move it.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Its door's facing (degrees)##wd", &t.wdDoorYaw, 0, 0, "%g");
    static const char* const kDoorStyles[] = {"Wall door (one-sided)", "Stone arch (two-sided)", "L.O.G.'s palace door",
                                              "Hidden (no door)"};
    ImGui::SetNextItemWidth(em(220));
    ImGui::Combo("Kind of door##wd", &t.wdDoorStyle, kDoorStyles, IM_ARRAYSIZE(kDoorStyles));
    helpMarker("Wall door: the game's world door. It is one-sided and cuts no hole, so stand it flush against a wall.\n"
               "Stone arch: the arch that leads home from every world. It is two-sided, so it can stand anywhere; a copy "
               "goes in the mod.\nL.O.G.'s palace door: the palace's door.\nHidden: no model, just the way in, for a "
               "doorway or gap already in the town.");
    if (t.wdDoorStyle <= 1) {
        pathField(*this, "Hologram image##wd", t.wdHologram, PathPick::OpenFile,
                  {{"Images", image::canDecodeImages() ? "png;jpg;jpeg;bmp;tga" : "png"}}, "optional: a PNG or JPG");
        helpMarker(t.wdDoorStyle == 0 ? "The picture the door's hologram shows (cropped to fit), on a copy of the game's world "
                                        "door in the mod's stream bundle. Empty: the game's door, showing Nutty Acres."
                                      : "The picture the arch's hologram shows (cropped to a square). Empty: the game's, "
                                        "showing Showdown Town.");
    }
    ImGui::TextUnformatted("Its door's stats (totals; -1: counted from the world's challenges)");
    helpMarker("Walking up to the door shows the world's jiggies, T.T. trophies, Jinjo tokens and notes as got/total. "
               "reNut counts what the player has, and the totals too unless you set them here.");
    static const char* const kStatLabels[] = {"Jiggies##wdstat", "T.T. trophies##wdstat", "Jinjo tokens##wdstat",
                                              "Notes##wdstat"};
    for (int i = 0; i < 4; ++i) {
        ImGui::SetNextItemWidth(em(100));
        if (ImGui::InputInt(kStatLabels[i], &t.wdStatTotals[i])) t.wdStatTotals[i] = std::max(-1, t.wdStatTotals[i]);
    }

    ImGui::SeparatorText("Characters");
    ImGui::RadioButton("None##wdnpc", &t.wdNpcMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Made here##wdnpc", &t.wdNpcMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("From a JSON file##wdnpc", &t.wdNpcMode, 2);
    helpMarker("Characters of the world's own: each stands in the world on an automatic skeleton with a looping idle, talks "
               "when the player walks up, and can give a \"reach the gate\" challenge. Made here, they are written to "
               "<mod folder>\\npcs.json when the world is made. Their scripts\\renutnpc_<world>_<id>.lua are written anew each time.");
    bool npcsReady = true;
    if (t.wdNpcMode == 1) npcsReady = drawNpcEditor();
    if (t.wdNpcMode == 2) {
        pathField(*this, "Characters file##wd", t.wdNpcs, PathPick::OpenFile, {{"Characters (JSON)", "json"}}, "npcs.json");
        helpMarker("A file {\"npcs\": [...]} as the README describes (and the editor's Save to file writes). Model and idle "
                   "paths in it are read beside the file.");
        npcsReady = !trim(t.wdNpcs).empty();
    }

    ImGui::SeparatorText("Mod");
    std::string bundleHint;
    try {
        if (!trim(t.wdMod).empty()) bundleHint = renutmake::defaultBundle(optPath(t.wdMod));
    } catch (std::exception&) {
    }
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Stream bundle name##wd", bundleHint.empty() ? "the mod folder's name" : bundleHint.c_str(), t.wdBundle);
    helpMarker("The world's scene goes into the mod's own stream bundle, which reNut loads with the game; the world itself goes "
               "into <mod folder>\\worlds, which the game loads when the player goes through its door.");
    ImGui::Checkbox("Replace the script if it exists##wd", &t.wdForce);
    bool ready = !root_.empty() && !trim(t.wdMod).empty() && renutpart::validId(id) && !trim(t.wdModel).empty() &&
                 (t.wdSkyMode != 1 || !trim(t.wdSkyImage).empty()) && npcsReady;
    if (!npcsReady && t.wdNpcMode == 1) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Fix the characters' problems first (Characters above).");
    if (runButton(*this, "Make world", ready)) {
        renutworld::World w;
        w.id = id;
        w.name = trim(t.wdName);
        w.model = optPath(t.wdModel);
        if (t.wdSpawnSet) w.spawn = std::array<float, 3>{t.wdSpawn[0], t.wdSpawn[1], t.wdSpawn[2]};
        if (t.wdExitSet) w.exit = std::array<float, 3>{t.wdExit[0], t.wdExit[1], t.wdExit[2]};
        w.door = {t.wdDoor[0], t.wdDoor[1], t.wdDoor[2]};
        w.spawnYaw = t.wdSpawnYaw;
        if (t.wdExitSet || t.wdExitYaw != 0) w.exitYaw = t.wdExitYaw;
        w.doorYaw = t.wdDoorYaw;
        static const char* const kStyles[] = {"wall", "arch", "palace", "hidden"};
        w.doorStyle = kStyles[std::clamp(t.wdDoorStyle, 0, 3)];
        for (size_t i = 0; i < 4; ++i) w.statTotals[i] = t.wdStatTotals[i];
        w.smoothDegrees = t.wdSmooth;
        w.scale = t.wdScale;
        w.ground = t.wdGround;
        w.groundColour = {t.wdGroundColour[0], t.wdGroundColour[1], t.wdGroundColour[2]};
        w.bundle = trim(t.wdBundle);
        if (t.wdDoorStyle <= 1 && !trim(t.wdHologram).empty()) w.hologram = optPath(t.wdHologram);
        if (t.wdNpcMode == 2) w.npcs = optPath(t.wdNpcs);
        if (t.wdNpcMode == 1 && !t.wdNpcList.empty()) {
            // The editor's characters go into <mod folder>\npcs.json, which the world maker reads.
            try {
                fs::path file = optPath(t.wdMod) / "npcs.json";
                std::vector<renutnpc::Npc> npcs;
                for (const NpcEdit& e : t.wdNpcList) npcs.push_back(toNpc(e, id));
                fs::create_directories(file.parent_path());
                writeText(file, renutnpc::toJson(npcs, file));
                w.npcs = file;
            } catch (std::exception& e) {
                error("Making world " + id + ": " + e.what());
                ImGui::End();
                return;
            }
        }
        w.colourScale = t.wdColourScale;
        for (const std::string& n : split(t.wdWater, ','))
            if (!trim(n).empty()) w.waterNames.push_back(trim(n));
        w.shapeWeights = shapeWeightsText(t.wdShapes);
        if (!t.wdMaterials.empty()) {
            // The table's settings go into <mod folder>\materials.json, which the world maker reads.
            try {
                const fs::path file = optPath(t.wdMod) / "materials.json";
                std::map<std::string, renutworld::MaterialSettings> materials;
                for (const MaterialEdit& m : t.wdMaterials) materials[m.name] = materialSettings(m);
                fs::create_directories(file.parent_path());
                writeText(file, renutworld::materialsJson(materials, file));
                w.materialsFile = file;
            } catch (std::exception& e) {
                error("Making world " + id + ": " + e.what());
                ImGui::End();
                return;
            }
        } else if (std::error_code ec; fs::is_regular_file(optPath(t.wdMod) / "materials.json", ec)) {
            w.materialsFile = optPath(t.wdMod) / "materials.json";  // saved by an earlier session
        }
        w.maxTexture = uint32_t(t.wdMaxTexture);
        if (!trim(t.wdMusic).empty()) w.music = optPath(t.wdMusic);
        w.musicVolume = t.wdMusicVolume;
        if (t.wdSkyMode == 1) w.sky = toUtf8(optPath(t.wdSkyImage));
        if (t.wdSkyMode == 2) {
            auto hex = [](const float* c) {
                char b[8];
                std::snprintf(b, sizeof b, "%02x%02x%02x", unsigned(std::lround(std::clamp(c[0], 0.0f, 1.0f) * 255)),
                              unsigned(std::lround(std::clamp(c[1], 0.0f, 1.0f) * 255)), unsigned(std::lround(std::clamp(c[2], 0.0f, 1.0f) * 255)));
                return std::string(b);
            };
            w.sky = hex(t.wdSkyTop) + "," + hex(t.wdSkyHorizon);
        }
        fs::path mod = optPath(t.wdMod), assets = root_;
        bool force = t.wdForce;
        runTask("Making world " + id, true, [this, assets, mod, w, force](Task&) -> std::function<void()> {
            renutworld::make(assets, mod, w, force, [this](const std::string& s) { log(s); });
            return [this, mod, id = w.id] { status("Made world " + id + " in " + toUtf8(mod) + " (see the log)"); };
        });
    }
    ImGui::End();
}

namespace {

constexpr const char* kVehiclePartPrefix = "aid_objparams_banjox_vehicleblock_";
const char* const kVehicleButtons[] = {"", "A", "B", "X"};

// The vehicle file's (and so the blueprint's) name: the vehicle's name in a-z, 0-9 and _.
std::string vehicleFileStem(const std::string& name) {
    std::string s;
    for (char c : lower(trim(name))) s += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
    return s.empty() ? std::string("vehicle") : s;
}

// The vehicle file renutvehicle reads: the name, then each part.
Json vehicleJson(const std::string& name, const std::vector<VehiclePartEdit>& parts) {
    Json j = Json::object();
    if (!trim(name).empty()) j["name"] = trim(name);
    Json list = Json::array();
    for (const VehiclePartEdit& p : parts) {
        Json o = Json::object();
        o["part"] = lower(trim(p.part));
        if (!trim(p.base).empty()) o["base"] = lower(trim(p.base));
        Json cell = Json::array(), rotation = Json::array();
        for (size_t k = 0; k < 3; ++k) {
            cell.push(p.cell[k]);
            rotation.push(double(p.rotation[k]));
        }
        o["cell"] = cell;
        o["rotation"] = rotation;
        char colour[8];
        std::snprintf(colour, sizeof colour, "%02x%02x%02x", unsigned(std::lround(std::clamp(p.colour[0], 0.0f, 1.0f) * 255)),
                      unsigned(std::lround(std::clamp(p.colour[1], 0.0f, 1.0f) * 255)), unsigned(std::lround(std::clamp(p.colour[2], 0.0f, 1.0f) * 255)));
        o["colour"] = std::string(colour);
        if (p.button > 0 && p.button <= 3) o["button"] = kVehicleButtons[p.button];
        list.push(o);
    }
    j["parts"] = list;
    return j;
}

// A vehicle file's parts (and its name, into `name`); throws on what renutvehicle would refuse to read.
std::vector<VehiclePartEdit> loadVehicleParts(const Json& j, std::string& name) {
    if (!j.has("parts") || j["parts"].type() != Json::Array) fail("a vehicle file holds {\"name\": ..., \"parts\": [ ... ]}");
    name = j.has("name") ? j["name"].asString() : std::string();
    std::vector<VehiclePartEdit> out;
    for (const Json& o : j["parts"].items()) {
        VehiclePartEdit p;
        p.part = o.has("part") ? o["part"].asString() : std::string();
        p.base = o.has("base") ? o["base"].asString() : std::string();
        if (o.has("cell"))
            for (size_t k = 0; k < 3 && k < o["cell"].size(); ++k) p.cell[k] = std::clamp(int(o["cell"][k].asNumber()), 0, 255);
        if (o.has("rotation"))
            for (size_t k = 0; k < 3 && k < o["rotation"].size(); ++k) p.rotation[k] = float(o["rotation"][k].asNumber());
        if (o.has("colour")) {
            uint32_t c = parseHex(o["colour"].asString());
            for (size_t k = 0; k < 3; ++k) p.colour[k] = float((c >> (16 - 8 * k)) & 0xFF) / 255.0f;
        }
        if (o.has("button")) {
            std::string b = lower(o["button"].asString());
            p.button = b == "a" ? 1 : b == "b" ? 2 : b == "x" ? 3 : 0;
        }
        out.push_back(p);
    }
    return out;
}

// What stops part `i` of `parts`; `shipped` (the game's part names, empty without an assets folder) checks the names.
std::vector<std::string> vehiclePartProblems(const std::vector<VehiclePartEdit>& parts, size_t i, const std::vector<std::string>& shipped) {
    const VehiclePartEdit& p = parts[i];
    std::vector<std::string> out;
    const std::string part = lower(trim(p.part)), base = lower(trim(p.base));
    auto isShipped = [&](const std::string& n) { return std::binary_search(shipped.begin(), shipped.end(), n); };
    if (part.empty()) out.push_back("pick a part");
    else if (!shipped.empty() && base.empty() && !isShipped(part)) out.push_back("not one of the game's parts: for a reNut part, give its base");
    if (!base.empty() && !shipped.empty() && !isShipped(base)) out.push_back("the base is not one of the game's parts");
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k == i) continue;
        const VehiclePartEdit& o = parts[k];
        if (o.cell[0] == p.cell[0] && o.cell[1] == p.cell[1] && o.cell[2] == p.cell[2]) out.push_back("another part is in this cell");
        if (p.button > 0 && o.button == p.button) out.push_back(std::string("another part is on button ") + kVehicleButtons[p.button]);
    }
    return out;
}

} // namespace

bool App::drawNpcEditor() {
    ToolWindows& t = tools_;
    std::vector<NpcEdit>& list = t.wdNpcList;
    const std::string world = lower(trim(t.wdId));
    if (t.wdNpcSel >= int(list.size())) t.wdNpcSel = int(list.size()) - 1;
    std::vector<std::vector<std::string>> wrong(list.size());
    bool allOk = true;
    size_t challenges = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        wrong[i] = npcProblems(list, i, world);
        allOk &= wrong[i].empty();
        challenges += list[i].challenge ? 1 : 0;
    }

    if (ImGui::Button("Add##npc")) {
        NpcEdit e;
        e.speaker = freeSpeaker(list);
        list.push_back(e);
        t.wdNpcSel = int(list.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(t.wdNpcSel < 0);
    if (ImGui::Button("Duplicate##npc")) {
        auto suffixed = [](const std::string& s) { return trim(s).empty() ? std::string() : trim(s) + "_2"; };
        NpcEdit e = list[size_t(t.wdNpcSel)];
        e.id = suffixed(e.id);
        e.tag = suffixed(e.tag);
        e.chId = suffixed(e.chId);
        e.speaker = freeSpeaker(list);
        list.insert(list.begin() + t.wdNpcSel + 1, e);
        ++t.wdNpcSel;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove##npc")) {
        list.erase(list.begin() + t.wdNpcSel);
        t.wdNpcSel = std::min(t.wdNpcSel, int(list.size()) - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load from file...##npc"))
        openFileDialog("Characters", {{"Characters (JSON)", "json"}}, [this](const fs::path& p) {
            try {
                std::vector<NpcEdit> loaded;
                for (const renutnpc::Npc& n : renutnpc::load(p)) loaded.push_back(fromNpc(n));
                tools_.wdNpcList = std::move(loaded);
                tools_.wdNpcSel = 0;
                tools_.wdNpcFile = toUtf8(p);
                status("Loaded " + std::to_string(tools_.wdNpcList.size()) + " character(s) from " + toUtf8(p.filename()));
            } catch (std::exception& e) {
                error("Loading characters: " + std::string(e.what()));
            }
        }, optPath(t.wdNpcFile));
    ImGui::SameLine();
    ImGui::BeginDisabled(list.empty() || !allOk);
    if (ImGui::Button("Save to file...##npc"))
        saveFileDialog("Save characters", {{"Characters (JSON)", "json"}}, [this, world](const fs::path& picked) {
            fs::path p = picked;
            if (lower(toUtf8(p.extension())) != ".json") p += ".json";
            try {
                std::vector<renutnpc::Npc> npcs;
                for (const NpcEdit& e : tools_.wdNpcList) npcs.push_back(toNpc(e, world));
                writeText(p, renutnpc::toJson(npcs, p));
                tools_.wdNpcFile = toUtf8(p);
                status("Saved " + std::to_string(npcs.size()) + " character(s) to " + toUtf8(p));
            } catch (std::exception& e) {
                error("Saving characters: " + std::string(e.what()));
            }
        }, optPath(t.wdNpcFile));
    ImGui::EndDisabled();
    helpMarker("Load reads a characters file (npcs.json, as the README describes it); Save writes one, with the model and "
               "idle paths relative to it when they are in its folder. Save needs every character free of problems.");

    if (list.empty()) {
        note("No characters yet: Add one, or Load from file.");
        return true;
    }
    if (ImGui::BeginTable("##npcs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, em(110)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Character");
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("Challenge");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < list.size(); ++i) {
            const NpcEdit& e = list[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            std::string who = !trim(e.name).empty() ? trim(e.name) : !trim(e.id).empty() ? trim(e.id) : std::string("(new character)");
            if (ImGui::Selectable(who.c_str(), t.wdNpcSel == int(i), ImGuiSelectableFlags_SpanAllColumns)) t.wdNpcSel = int(i);
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string modelName = trim(e.model).empty() ? std::string("-") : toUtf8(optPath(e.model).filename());
            ImGui::TextUnformatted(modelName.c_str());
            ImGui::TableNextColumn();
            std::string challenge = !e.challenge ? std::string("none") : trim(e.chName).empty() ? std::string("yes") : trim(e.chName);
            ImGui::TextUnformatted(challenge.c_str());
            ImGui::TableNextColumn();
            if (wrong[i].empty()) ImGui::TextUnformatted("ready");
            else ImGui::TextColored(kProblem, "%zu problem%s", wrong[i].size(), wrong[i].size() == 1 ? "" : "s");
        }
        ImGui::EndTable();
    }
    if (list.size() > 4)
        ImGui::TextColored(kProblem, "reNut lends characters four spare tags across every mod that is on: more than four won't all appear.");
    if (challenges > 3)
        ImGui::TextColored(kProblem, "reNut has twelve spare game flags across every mod, four per challenge: more than three won't all save.");
    if (t.wdNpcSel < 0) return allOk;

    NpcEdit& e = list[size_t(t.wdNpcSel)];
    ImGui::PushID(t.wdNpcSel);
    for (auto& w : wrong[size_t(t.wdNpcSel)]) ImGui::TextColored(kProblem, "- %s", w.c_str());

    ImGui::SeparatorText("Who they are");
    ImGui::SetNextItemWidth(em(200));
    inputText("Character id##npc", e.id);
    helpMarker("1-40 characters of a-z, 0-9 and _, unique in this world. It names the character's files in the mod and its "
               "script, scripts\\renutnpc_<world>_<id>.lua.");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Name##npc", trim(e.id).empty() ? "the id" : trim(e.id).c_str(), e.name);
    helpMarker("Who they are, for you and other mods: it names them in their script and in reNut's list of characters. "
               "Players only see what they say.");
    pathField(*this, "Model##npc", e.model, PathPick::OpenFile, {{"Models", "dae;glb;obj"}}, "a .dae, .glb or .obj file");
    helpMarker("What they look like: every mesh of the file with its textures and vertex colours, y up. They are stood "
               "with their feet on the ground at Position and get a skeleton made from their height.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Scale##npc", &e.scale, 0, 0, "%g");
    helpMarker("Their size, times this. Ripped models are often in other units: a 3DS rip is often about 0.03-0.04 to be "
               "person-sized. After Make world, the log says how tall they came out, in metres.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(100));
    ImGui::InputFloat("Smoothing angle##npc", &e.smooth, 0, 0, "%g");
    helpMarker("Faces meeting at less than this many degrees share a normal (look smooth). 40 suits most characters.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Vertex colour scale##npc", &e.colourScale, 0, 0, "%g");
    helpMarker("Their vertex colours times this (at most white): 2 for 3DS rips, whose vertex colours are stored at half, "
               "else 1.");
    comboField("Rig##npc", e.rig, {"auto"}, "auto", em(160));
    helpMarker("Their skeleton. auto builds one from their shape (hips, chest, neck, head, and each side's hip, knee, "
               "ankle, shoulder and elbow) and binds every vertex to it. It is the only rig for now.");
    int idleMode = trim(e.idle) == "breathe" ? 0 : 1;
    ImGui::TextUnformatted("Idle:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Breathe##npcidle", &idleMode, 0)) e.idle = "breathe";
    ImGui::SameLine();
    if (ImGui::RadioButton("Timeline file##npcidle", &idleMode, 1) && trim(e.idle) == "breathe") e.idle.clear();
    helpMarker("The motion they loop while standing (and play when bumped). Breathe sways the hips, lifts the chest and "
               "turns the head. A timeline is a JSON file of poses for the bones BASE, BACKA (hips), BACKB (chest), "
               "NECK1, HEAD, and on each side LF_/RT_ H, K, F (hip, knee, ankle) and S, E (shoulder, elbow) "
               "(reNut's docs/nuttool/animations.md).");
    if (idleMode == 1) pathField(*this, "Timeline##npc", e.idle, PathPick::OpenFile, {{"Animation timeline", "json"}}, "a .json timeline");
    ImGui::SetNextItemWidth(em(120));
    ImGui::SliderFloat("Arms down (degrees)##npc", &e.armsDown, 0.0f, 90.0f, "%.0f");
    helpMarker("Lowers both arms by this much in every animation. A model in a T-pose (arms straight out, as many rips "
               "are) needs about 70 to stand with its arms at its sides; 0 for one already standing naturally.");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Leave out##npc", "optional: parts of names, e.g. meshes[4]", e.exclude);
    helpMarker("Parts of object, material or texture names (separated by commas) to leave out of the model, such as a "
               "rip's shadow sheet or a prop that sits below the feet and makes the character float or measure tall.");
    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Shape key weights##npc", "optional: name=weight, ...", e.shapes);
    helpMarker("A .glb's shape keys (morph targets) come with the model: a face's blink or smile. These set where they "
               "start, 0 to 1 (else the .glb's own); Lua moves them with npc:set_shape(name, weight).");

    ImGui::SeparatorText("Where they stand");
    ImGui::SetNextItemWidth(em(260));
    ImGui::InputFloat3("Position (x, y, z)##npc", e.position, "%g");
    helpMarker("Where their feet go in the world, in metres (the world's coordinates, after its Scale). The game drops "
               "them onto the ground below, so a y a little high is fine.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputFloat("Facing (degrees)##npc", &e.yaw, 0, 0, "%g");
    helpMarker("Which way they face, turning about the vertical axis: 0 faces +z, 90 faces +x, 180 faces -z.");

    ImGui::SeparatorText("Enemy");
    ImGui::Checkbox("Is an enemy##npc", &e.enemy);
    helpMarker("An enemy instead of someone to talk to: no lines or challenge, hits wear its health down, and at 0 it's "
               "beaten. What it does is a Lua script, scripts\\renutnpc_<world>_<id>_ai.lua.");
    if (e.enemy) {
        ImGui::SetNextItemWidth(em(120));
        ImGui::InputInt("Health (hits)##npc", &e.enemyHealth);
        e.enemyHealth = std::clamp(e.enemyHealth, 1, 1000);
        ImGui::TextUnformatted("Behaviour:");
        ImGui::SameLine();
        ImGui::RadioButton("Mirror Banjo##npcai", &e.enemyBehaviour, 0);
        ImGui::SameLine();
        ImGui::RadioButton("My own Lua##npcai", &e.enemyBehaviour, 1);
        helpMarker("Mirror Banjo: NutTool writes a script that makes it copy what Banjo does, as his mirror image (like "
                   "Dark Link). It's written once, so you can change it. My own Lua: it stands until your script moves it "
                   "(renut.npcs.on in reNut's docs/tutorials/characters-and-challenges.md).");
    }

    ImGui::SeparatorText("Talking");
    std::string tagHint = defaultTag(world, lower(trim(e.id)));
    ImGui::SetNextItemWidth(em(320));
    inputTextWithHint("Tag##npc", tagHint.c_str(), e.tag);
    helpMarker("The name the world's scene and their dialogs know them by: 1-63 characters of a-z, 0-9 and _, unique "
               "across every mod. reNut lends it one of the game's spare tags. Empty: renut_<world>_<id>.");
    ImGui::SetNextItemWidth(em(120));
    ImGui::InputInt("Speaker slot##npc", &e.speaker);
    e.speaker = std::clamp(e.speaker, 1, 32);
    helpMarker("The dialog speaker slot their lines use, 1-32 (32 by default). reNut gives the slot to this character "
               "while the world is up; two characters in one world need different slots.");
    note("What they say. {PAGEBREAK} starts a new page (the player presses a button to go on); {MOODHAPPY}, {MOODSAD} "
         "and {MOODANGRY} set the mood they say what follows in.");
    static const char* const lineLabels[] = {"Greeting", "Objectives", "Win", "Lose", "Completed"};
    static const char* const lineHelp[] = {
        "The first thing they say when the player talks to them. Always needed.",
        "With a challenge: what they say after the greeting to explain it (where to go, the times to beat, the rewards).",
        "With a challenge: what they say when the player made it through the gate in time.",
        "With a challenge: what they say when time ran out.",
        "With a challenge: said instead of the greeting once the player has beaten it."};
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i);
        ImGui::TextUnformatted(lineLabels[i]);
        helpMarker(lineHelp[i]);
        inputTextMultiline("##line", e.lines[i], ImVec2(-FLT_MIN, em(54)));
        pathField(*this, "Sound##npcsound", e.sounds[i], PathPick::OpenFile, {{"Sound", "ogg;mp3;flac;wav"}},
                  "optional: an .ogg, .mp3, .flac or .wav");
        helpMarker("Played once as this line starts, instead of their voice on its first page (a greeting, a cheer). "
                   "It's copied into the mod's sounds folder.");
        ImGui::PopID();
    }

    ImGui::SeparatorText("Voice");
    ImGui::RadioButton("None##npcvoice", &e.voiceMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Game character's voice##npcvoice", &e.voiceMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Your own clips##npcvoice", &e.voiceMode, 2);
    helpMarker("What they sound like as each page of what they say comes up, as the game's characters babble. None: "
               "silent. A game character's: that character's own babble. Your own clips: one of your sound files, at random.");
    if (e.voiceMode == 1) {
        comboField("Character##npcvoice", e.gameVoice, renutnpc::gameVoices(), "humba", em(200));
        helpMarker("Whose voice: its cues are copied from the game's character into theirs, and reNut loads its sounds "
                   "while you're in the world.");
    }
    if (e.voiceMode == 2) {
        for (size_t k = 0; k < e.voiceClips.size(); ++k) {
            ImGui::PushID(int(k));
            pathField(*this, "Clip##npcclip", e.voiceClips[k], PathPick::OpenFile, {{"Sound", "ogg;mp3;flac;wav"}},
                      "an .ogg, .mp3, .flac or .wav");
            ImGui::SameLine();
            const bool remove = ImGui::Button("Remove##npcclip");
            ImGui::PopID();
            if (remove) {
                e.voiceClips.erase(e.voiceClips.begin() + std::ptrdiff_t(k));
                break;
            }
        }
        if (ImGui::Button("Add a clip##npcclip")) e.voiceClips.emplace_back();
        helpMarker("Short sound files (a second or two of babble). One is picked at random as each page starts, never the "
                   "same one twice in a row. They're copied into the mod's sounds folder.");
    }
    ImGui::SetNextItemWidth(em(160));
    ImGui::SliderFloat("Volume##npcvoice", &e.voiceVolume, 0.0f, 1.0f, "%.2f");
    helpMarker("How loud your clips and the lines' sounds play, 0 to 1 (a game character's voice plays at the game's own).");

    ImGui::SeparatorText("Challenge");
    ImGui::Checkbox("Gives a challenge##npc", &e.challenge);
    helpMarker("A \"reach the gate\" race: talking to them starts it, the player's vehicle is put at the start, and they "
               "drive through the goal gate before the time limit for notes, a jiggy and a T.T. trophy.");
    if (e.challenge) {
        ImGui::SetNextItemWidth(em(200));
        inputText("Challenge id##npc", e.chId);
        helpMarker("1-40 characters of a-z, 0-9 and _. The player's progress is saved under it, and each new id takes up "
                   "game flags for good: pick it once and keep it.");
        ImGui::SetNextItemWidth(em(320));
        inputText("Title##npc", e.chName);
        helpMarker("The challenge's name, shown on its screens (Treasure Dash, say).");
        ImGui::TextUnformatted("Summary");
        helpMarker("A sentence or two about it, shown with the title on the challenge's screens.");
        inputTextMultiline("##summary", e.chSummary, ImVec2(-FLT_MIN, em(40)));
        ImGui::SetNextItemWidth(em(260));
        ImGui::InputFloat3("Goal gate (x, y, z)##npc", e.goal, "%g");
        helpMarker("The middle of the goal gate, on the ground: the player wins by driving through it.");
        ImGui::SetNextItemWidth(em(120));
        ImGui::InputFloat("Gate facing (degrees)##npc", &e.goalYaw, 0, 0, "%g");
        helpMarker("Which way the gate faces (0 faces +z, 90 faces +x): turn it across the way the player arrives.");
        ImGui::SetNextItemWidth(em(180));
        ImGui::InputFloat2("Gate width, height (m)##npc", e.gate, "%g");
        helpMarker("The gate's size in metres. 16 by 26, Park Prix's, is easy to hit; narrower is harder.");
        ImGui::TextUnformatted("Clock:");
        ImGui::SameLine();
        int down = e.countDown ? 1 : 0;
        ImGui::RadioButton("Counts up##npcclock", &down, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Counts down##npcclock", &down, 1);
        e.countDown = down == 1;
        helpMarker("Up: the clock runs from 0 and the player sees how fast they are. Down: it runs down from the time limit "
                   "and the challenge is lost at 0. The rewards are the same either way.");
        ImGui::SetNextItemWidth(em(90));
        ImGui::InputFloat("Time limit (s)##npc", &e.timeLimit, 0, 0, "%g");
        helpMarker("Reach the gate within this many seconds for the notes.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(90));
        ImGui::InputFloat("Jiggy (s)##npc", &e.jiggy, 0, 0, "%g");
        helpMarker("Within this many seconds also wins a jiggy, which goes into the Jiggy Bank at once. At most the time limit.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(90));
        ImGui::InputFloat("T.T. trophy (s)##npc", &e.tt, 0, 0, "%g");
        helpMarker("Within this many seconds also wins a T.T. trophy: the hardest time, at most the jiggy's.");
        ImGui::SetNextItemWidth(em(90));
        ImGui::InputInt("Notes (x10)##npc", &e.notes);
        e.notes = std::clamp(e.notes, 1, 1000);
        helpMarker("The notes it pays, in tens: 1 pays 10 notes, 5 pays 50.");
        ImGui::Checkbox("Vehicle start##npc", &e.startSet);
        helpMarker("Where the player's vehicle is put when the challenge starts. Off: 12 m from the character towards the goal.");
        if (e.startSet) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em(260));
            ImGui::InputFloat3("##npcstart", e.start, "%g");
        }
        ImGui::Checkbox("Vehicle facing##npc", &e.startYawSet);
        helpMarker("Which way the vehicle faces at the start, in degrees. Off: towards the goal gate.");
        if (e.startYawSet) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em(120));
            ImGui::InputFloat("##npcstartyaw", &e.startYaw, 0, 0, "%g");
        }
    }
    ImGui::PopID();
    return allOk;
}

void App::drawVehicleTool() {
    ToolWindows& t = tools_;
    if (!t.showVehicle) return;
    if (t.veBundle.empty() && !root_.empty()) {
        std::error_code ec;
        fs::path parts = root_ / "Bundle" / "4f" / "685374";
        if (fs::is_regular_file(parts, ec)) t.veBundle = toUtf8(parts);
    }
    ImGui::SetNextWindowSize(ImVec2(em(700), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("reNut vehicle", &t.showVehicle)) {
        ImGui::End();
        return;
    }
    note("Makes <mod folder>\\vehicles\\<name>.blueprint, a vehicle reNut adds to the player's saved vehicles once: build its "
         "parts list here, or use a vehicle file (JSON) made before.");
    pathField(*this, "Parts bundle##ve", t.veBundle, PathPick::OpenFile, {{"All files", kAllFiles}});
    helpMarker("The game's bundle with the parts' records, which give their block types and mass: Bundle\\4f\\685374.");
    pathField(*this, "Mod folder##ve", t.veMod, PathPick::Folder, {}, "mods\\<your mod>");
    ImGui::RadioButton("Made here##ve", &t.veMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("From a vehicle file##ve", &t.veMode, 1);
    bool ready = !trim(t.veBundle).empty() && !trim(t.veMod).empty();
    if (t.veMode == 1) {
        pathField(*this, "Vehicle file##ve", t.veVehicle, PathPick::OpenFile, {{"Vehicle file", "json"}}, "a .json file");
        helpMarker("{\"name\": ..., \"parts\": [...]}, each part with \"part\", \"cell\": [x, y, z], \"rotation\": [x, y, z] in "
                   "degrees, \"colour\": \"rrggbb\" and \"button\"; a reNut part also names its shipped \"base\". The "
                   "blueprint is named after the file.");
        ready &= !trim(t.veVehicle).empty();
    } else {
        ready &= drawVehicleEditor();
    }
    if (runButton(*this, "Make blueprint", ready)) {
        fs::path bundle = optPath(t.veBundle), vehicle = optPath(t.veVehicle), mod = optPath(t.veMod);
        if (t.veMode == 0) {
            // The parts list goes into <mod folder>\vehicles\<name>.json, the blueprint beside it.
            try {
                vehicle = mod / "vehicles" / (vehicleFileStem(t.veName) + ".json");
                fs::create_directories(vehicle.parent_path());
                writeText(vehicle, vehicleJson(t.veName, t.veParts).dump(2));
            } catch (std::exception& e) {
                error("Making the blueprint: " + std::string(e.what()));
                ImGui::End();
                return;
            }
        }
        runTool("Making the blueprint", [bundle, vehicle, mod] {
            return std::vector<std::string>{"renut-vehicle", cliPath(bundle), cliPath(vehicle), cliPath(mod)};
        });
    }
    ImGui::End();
}

bool App::drawVehicleEditor() {
    ToolWindows& t = tools_;
    std::vector<VehiclePartEdit>& parts = t.veParts;
    if (t.veSel >= int(parts.size())) t.veSel = int(parts.size()) - 1;
    // The shipped parts a vehicle file names, without their prefix.
    std::vector<std::string> shipped;
    if (catalog_)
        for (auto& p : catalog_->parts) {
            std::string n = renutpart::baseName(p.name);
            if (startsWith(n, kVehiclePartPrefix)) shipped.push_back(n.substr(std::strlen(kVehiclePartPrefix)));
        }
    std::sort(shipped.begin(), shipped.end());
    shipped.erase(std::unique(shipped.begin(), shipped.end()), shipped.end());
    std::vector<std::vector<std::string>> wrong(parts.size());
    bool allOk = !parts.empty();
    for (size_t i = 0; i < parts.size(); ++i) {
        wrong[i] = vehiclePartProblems(parts, i, shipped);
        allOk &= wrong[i].empty();
    }

    ImGui::SetNextItemWidth(em(260));
    inputTextWithHint("Vehicle name##ve", "MOD VEHICLE", t.veName);
    helpMarker("Its name in the garage's saved vehicles (at most 31 characters). The blueprint's file is named after it.");
    if (ImGui::Button("Add part##ve")) {
        VehiclePartEdit p;
        if (t.veSel >= 0) {
            // Next to the selected part, so a row of parts is quick to lay out.
            p = parts[size_t(t.veSel)];
            p.cell[0] = std::min(p.cell[0] + 1, 255);
            p.button = 0;
        }
        parts.push_back(p);
        t.veSel = int(parts.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(t.veSel < 0);
    if (ImGui::Button("Remove part##ve")) {
        parts.erase(parts.begin() + t.veSel);
        t.veSel = std::min(t.veSel, int(parts.size()) - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load from file...##ve"))
        openFileDialog("Vehicle file", {{"Vehicle file", "json"}}, [this](const fs::path& p) {
            try {
                tools_.veParts = loadVehicleParts(Json::parse(readText(p)), tools_.veName);
                tools_.veSel = tools_.veParts.empty() ? -1 : 0;
                tools_.veVehicle = toUtf8(p);
                status("Loaded " + std::to_string(tools_.veParts.size()) + " part(s) from " + toUtf8(p.filename()));
            } catch (std::exception& e) {
                error("Loading the vehicle file: " + std::string(e.what()));
            }
        }, optPath(t.veVehicle));
    ImGui::SameLine();
    ImGui::BeginDisabled(!allOk);
    if (ImGui::Button("Save to file...##ve"))
        saveFileDialog("Save vehicle file", {{"Vehicle file", "json"}}, [this](const fs::path& picked) {
            fs::path p = picked;
            if (lower(toUtf8(p.extension())) != ".json") p += ".json";
            try {
                writeText(p, vehicleJson(tools_.veName, tools_.veParts).dump(2));
                tools_.veVehicle = toUtf8(p);
                status("Saved the vehicle file " + toUtf8(p));
            } catch (std::exception& e) {
                error("Saving the vehicle file: " + std::string(e.what()));
            }
        }, optPath(t.veVehicle));
    ImGui::EndDisabled();
    helpMarker("Load reads a vehicle file; Save writes one (the format renut-vehicle and the README describe), to keep "
               "or share the parts list.");
    if (!catalog_) ImGui::TextDisabled("Open the game's assets folder to pick parts from a list and have them checked.");

    if (parts.empty()) {
        note("No parts yet: Add part, or Load from file.");
        return false;
    }
    static const char* const buttons[] = {"none", "A", "B", "X"};
    if (ImGui::BeginTable("##veparts", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, em(150)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Part");
        ImGui::TableSetupColumn("Cell");
        ImGui::TableSetupColumn("Rotation");
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < parts.size(); ++i) {
            const VehiclePartEdit& p = parts[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            ImGui::ColorButton("##colour", ImVec4(p.colour[0], p.colour[1], p.colour[2], 1), ImGuiColorEditFlags_NoTooltip,
                               ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            std::string label = trim(p.part).empty() ? std::string("(pick a part)") : trim(p.part);
            if (!trim(p.base).empty()) label += " (on " + trim(p.base) + ")";
            if (ImGui::Selectable(label.c_str(), t.veSel == int(i), ImGuiSelectableFlags_SpanAllColumns)) t.veSel = int(i);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%d, %d, %d", p.cell[0], p.cell[1], p.cell[2]);
            ImGui::TableNextColumn();
            ImGui::Text("%g, %g, %g", double(p.rotation[0]), double(p.rotation[1]), double(p.rotation[2]));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(buttons[std::clamp(p.button, 0, 3)]);
            ImGui::TableNextColumn();
            if (wrong[i].empty()) ImGui::TextUnformatted("ready");
            else ImGui::TextColored(kProblem, "%zu problem%s", wrong[i].size(), wrong[i].size() == 1 ? "" : "s");
        }
        ImGui::EndTable();
    }
    if (t.veSel < 0) return allOk;

    VehiclePartEdit& p = parts[size_t(t.veSel)];
    ImGui::PushID(t.veSel);
    for (auto& w : wrong[size_t(t.veSel)]) ImGui::TextColored(kProblem, "- %s", w.c_str());
    comboField("Part##vep", p.part, shipped, "body_light_cube, or a reNut part's id", em(320));
    helpMarker("A shipped part's name (as the garage's records name it, body_light_cube say), or the id of a part made with "
               "reNut > New part or New block.");
    comboField("Base##vep", p.base, shipped, "only for a reNut part", em(320));
    helpMarker("For a reNut part: the shipped part it was made from, whose record gives its block type and mass. Empty for "
               "a shipped part.");
    ImGui::SetNextItemWidth(em(200));
    if (ImGui::InputInt3("Cell (x, y, z)##vep", p.cell))
        for (int& c : p.cell) c = std::clamp(c, 0, 255);
    helpMarker("Where it sits on the garage's building grid, one cell per block, y up (0-255 each).");
    ImGui::SetNextItemWidth(em(200));
    ImGui::InputFloat3("Rotation (degrees)##vep", p.rotation, "%g");
    helpMarker("Its turn about x, y and z in degrees, in steps of 90 as the garage turns parts.");
    ImGui::ColorEdit3("Colour##vep", p.colour, ImGuiColorEditFlags_NoInputs);
    helpMarker("Its paint colour.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(90));
    ImGui::Combo("Button##vep", &p.button, buttons, IM_ARRAYSIZE(buttons));
    helpMarker("The pad button that works it, for a part that does something (an engine, a gun...): A, B or X.");
    ImGui::PopID();
    return allOk;
}

} // namespace nut::gui
