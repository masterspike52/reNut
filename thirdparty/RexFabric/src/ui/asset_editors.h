// The editors assets open in (docs/content_and_classes.md, Editors). Each is a window of its own, owned and drawn by
// the content browser, which it asks for what it can't know alone: the classes a mod may use, and that it saved.
//
//   ClassEditor     the properties editor: a class's parent and help, the defaults it inherits grouped by the
//                   class declaring them, each with a reset to its parent's value, and the properties it adds
//   EnumEditor      an enum's values: add, remove, reorder, rename, each with the name the player sees
//   StructEditor    a struct's fields: add, remove, reorder, rename and retype, each with its default
//   TableEditor     a data table's rows as a grid, a column per field of its struct: add, remove, reorder, rename
//   GameTableEditor one of the game's tables, read only, as the game has it now
//   TranslationEditor  a mod's texts beside their translations into one of the game's languages
//                   (content/localisation.h): the missing and the out of date marked
//   TextureEditor   a texture: its picture, what it's for, and a new picture in its place
//   MapEditEditor   a map edit: the world it changes, and its edits, each a spawn of an avatar class (its place and the
//                   values it sets on that avatar alone) or one of the game's kinds of edit
//   LanguageEditor  a language asset: the game's own texts by group, each beside the mod's text in its place
//                   (content/game_texts.h), the changed and the out of date marked
//   MetadataEditor  a mod's metadata, its mod.toml: name, version, author, description, the game version it needs
//                   and the mods it depends on
//
// Renaming an enum's value or a struct's field and saving renames it in every value of every asset holding one.
//
// An avatar class's ClassEditor is the Avatar editor: its viewport (avatar_viewport.h) beside the rest, the component
// selected in either being the one the viewport's gizmo is on. A later editor is another AssetEditor, picked by the
// browser for its class.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "../content/assets.h"
#include "menu_model.h"
#include "value_widgets.h"

namespace rexfabric::ui {

// What an editor asks of the browser hosting it.
class EditorHost {
 public:
  virtual ~EditorHost() = default;
  virtual MenuModel& model() = 0;
  // What `mod` may name: the classes it may use as a parent or an asset, and the enums and structs it may type
  // properties with. RexFabric's and the game's, then its own and its dependencies', as their files are now.
  virtual TypeChoices Choices(std::string_view mod) = 0;
  // The enums and structs as their files are now, read again after anything is saved.
  virtual const content::TypeLookup& Types() = 0;
  // An editor wrote something: the browser reads the content again and reloads the classes.
  virtual void Saved(const std::string& path) = 0;
  // Where a new editor window of this size opens: beside the browser, where there's room.
  virtual ImVec2 EditorPosition(const ImVec2& size) = 0;
};

// A problem an editor found, and when there's one, what fixes it.
struct EditorProblem {
  std::string text;
  std::string fix_label;
  std::function<void()> fix;
};

class AssetEditor {
 public:
  AssetEditor(EditorHost& host, std::string path) : host_(host), path_(std::move(path)) {}
  virtual ~AssetEditor() = default;
  AssetEditor(const AssetEditor&) = delete;
  AssetEditor& operator=(const AssetEditor&) = delete;

  // The path of what it edits; for the metadata, the mod's.
  const std::string& path() const { return path_; }
  virtual bool dirty() const = 0;
  virtual bool Save() = 0;
  // Something else was saved, which may change what this one shows (an ancestor, a mod's dependencies).
  virtual void ContentChanged() {}

  // Draws its window; false once it has closed. A new window opens a little off the last one.
  bool Draw();
  // Brings its window to the front on the next Draw.
  void Focus() { focus_ = true; }
  // Closes it, once unsaved changes are saved or let go.
  void RequestClose();

 protected:
  // The title, before the " *" of unsaved changes.
  virtual std::string Title() const = 0;
  // How much wider than most its window opens.
  virtual float WidthFactor() const { return 1.0f; }
  virtual void DrawContents() = 0;
  virtual void Undo() {}
  virtual void Redo() {}

  void SetStatus(std::string text, bool error = false);
  // The problems list, at the bottom of the window.
  void DrawProblems(const std::vector<EditorProblem>& problems);
  // The toolbar's Save and Undo and Redo, and the status after them.
  void DrawToolbar(bool can_undo, bool can_redo);

  EditorHost& host_;
  std::string path_;

 private:
  bool focus_ = true;
  bool placed_ = false;
  bool closing_ = false;
  bool closed_ = false;
  bool open_save_prompt_ = false;
  std::string status_;
  bool status_error_ = false;
  double status_time_ = 0.0;
};

// The editor an asset opens in, by what it is; null for an asset no editor opens yet.
std::unique_ptr<AssetEditor> OpenClassEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenEnumEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenStructEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenTableEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenGameTableEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenLanguageEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenTextureEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenMapEditEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenMaterialEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenMetadataEditor(EditorHost& host, const std::string& mod);
// Its path is the mod's with this after it, as no asset's is.
inline constexpr std::string_view kTranslationsSuffix = ":translations";
std::unique_ptr<AssetEditor> OpenTranslationEditor(EditorHost& host, const std::string& mod);

}  // namespace rexfabric::ui
