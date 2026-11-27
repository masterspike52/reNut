// What the asset editors share (asset_editors.h): the document editor every one of them is, and the rows and checks
// they lay out and run alike. Not for anything beyond the editors.

#pragma once

#include <cfloat>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "../content/assets.h"
#include "asset_editors.h"

namespace rexfabric::ui {

// In multiples of the font size. An editor with a viewport (an avatar class's, a material's) is wider, for its
// viewport beside the rest, which starts at this share of the window.
inline constexpr float kAvatarEditorWidth = 2.0f;
inline constexpr float kViewportShare = 0.5f;
inline constexpr float kViewportMinWidth = 16.0f;
inline constexpr float kNameColumn = 9.0f;
// A value the asset sets, rather than one it inherits.
inline constexpr ImVec4 kOverriddenColor{0.55f, 0.75f, 1.0f, 1.0f};

// A row of a two column table: its label, then the cursor in its field, as wide as the field.
void LabelRow(const char* label);
bool BeginLabelTable(const char* id);

// Each of `properties` with the value `values` gives it, by name: in blue with a reset to the default where it gives
// one. True when a value changed.
bool DrawSetValues(const char* id, const std::vector<content::Property>& properties, nlohmann::json& values, EditorHost& host, const TypeChoices& choices);

// Whether a mod may use what `path` names: RexFabric's and the game's, its own, its dependencies'. Read from its
// metadata as it is now, not as it loaded.
bool MayUse(const ModInfo& mod, std::string_view path);
// A problem for a path a mod may not use, with adding the dependency as its fix; nothing when it may.
std::optional<EditorProblem> DependencyProblem(EditorHost& host, const ModInfo& mod, std::string_view path, const std::string& what);

// An editor of one document: what's being edited, what's saved, and the history between them for undo.
template <typename Document>
class DocumentEditor : public AssetEditor {
 public:
  DocumentEditor(EditorHost& host, std::string path, Document document) : AssetEditor(host, std::move(path)), document_(document), saved_(document), history_{std::move(document)} {}

  bool dirty() const override { return !Equal(document_, saved_); }

 protected:
  // Whether two versions of the document are the same. A document without == says how in its editor.
  virtual bool Equal(const Document& a, const Document& b) const {
    if constexpr (std::equality_comparable<Document>) return a == b;
    else return false;
  }

  void Undo() override {
    if (at_ > 0) document_ = history_[--at_];
  }
  void Redo() override {
    if (at_ + 1 < history_.size()) document_ = history_[++at_];
  }
  bool CanUndo() const { return at_ > 0; }
  bool CanRedo() const { return at_ + 1 < history_.size(); }
  // Keeps the document as it is in the history once an edit is done. At the end of DrawContents.
  void Remember() {
    if (ImGui::IsAnyItemActive() || Equal(document_, history_[at_])) return;
    history_.resize(at_ + 1);
    history_.push_back(document_);
    ++at_;
  }
  // The file was read again: the document is as it says, and the history starts over.
  void Reload(Document document) {
    document_ = saved_ = document;
    history_ = {std::move(document)};
    at_ = 0;
  }
  void MarkSaved() { saved_ = document_; }
  const ModInfo* Mod() const { return host_.model().mods().Find(content::ModOf(path_)); }
  // The asset's file, or null with the status saying the mod is gone.
  std::optional<std::filesystem::path> File() {
    const ModInfo* mod = Mod();
    if (mod) return content::AssetFile(*mod, path_);
    SetStatus("The mod " + std::string(content::ModOf(path_)) + " is gone", true);
    return std::nullopt;
  }
  // The status after a save: a problem, a mod that's off, or done.
  void SavedStatus(bool problems) {
    const ModInfo* mod = Mod();
    if (problems) SetStatus("Saved, but it won't load until its problems are fixed", true);
    else if (mod && !mod->enabled) SetStatus("Saved. " + mod->name + " is off, so its content isn't loaded");
    else SetStatus("Saved");
  }

  Document document_;

 private:
  Document saved_;
  std::vector<Document> history_;
  size_t at_ = 0;
};

}  // namespace rexfabric::ui
