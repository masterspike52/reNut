// The content browser (docs/content_and_classes.md, The content browser): every mod's content, browsed as folders of
// assets, and the editors its assets open in.
//
// The tree on the left has the game's tables, read only, then every mod, a mod that's off dimmed, with the folders of
// its content folder below it; the grid on the right shows what the folder picked holds, as tiles. A mod's root
// starts with its metadata, in red. A
// double click opens a folder, or an asset in its editor (asset_editors.h); a right click offers what can be done
// with it; dragging a tile onto a folder, in the grid, the tree or the path above, moves it there. Search and the class
// filter list the assets below the folder shown (every mod's, from the top) whose name matches and that are of the
// class.
//
// Renames and moves take references along: every asset of every mod naming what moved names its new path. A delete
// says what names what's going first, and leaves those to be fixed.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/ui/ui_system.h>

#include "../content/assets.h"
#include "asset_editors.h"
#include "menu_model.h"

namespace rexfabric::ui {

class ContentBrowserWindow : public rex::ui::DialogBase, public EditorHost {
 public:
  // `closed` runs as the window goes.
  ContentBrowserWindow(MenuModel& model, std::function<void()> closed);
  ~ContentBrowserWindow() override;

  // Closes the window and its editors, once their unsaved changes are saved or let go.
  void Hide();

  bool Draw(ImGuiIO& io) override;

  MenuModel& model() override { return model_; }
  TypeChoices Choices(std::string_view mod) override;
  const content::TypeLookup& Types() override { return types_; }
  void Saved(const std::string& path) override;
  ImVec2 EditorPosition(const ImVec2& size) override;

 private:
  enum class TileKind { kMod, kMetadata, kTranslations, kFolder, kClass, kEnum, kStruct, kTable, kLanguage, kTexture, kMapEdit, kMaterial, kMesh, kWorld, kPacket, kGameTable, kOther };
  struct Tile {
    TileKind kind = TileKind::kOther;
    std::string path;
    std::string label;
    std::string sub;
    bool dim = false;
  };
  // What the window asks for in a popup.
  enum class Ask { kNone, kNewMod, kNewFolder, kNewClass, kNewEnum, kNewStruct, kNewTable, kNewLanguage, kNewMapEdit, kNewMaterial, kNewWorld, kNewPacket, kImportImage, kImportMesh, kRename, kDelete };
  static const char* AskTitle(Ask ask);
  // How a class asset looks: its chain of classes, and whether its objects are of a game type.
  struct ClassLook {
    std::vector<std::string> chain;
    bool typed = false;
  };

  void OnDraw(ImGuiIO& io);
  // Reads every mod's content folder again.
  void Refresh();

  void DrawToolbar();
  void DrawAddMenu();
  void DrawBreadcrumb();
  void DrawTree();
  void DrawTreeFolder(const std::string& folder);
  void DrawGrid();
  void DrawTile(const Tile& tile, const ImVec2& size);
  void DrawGlyph(const Tile& tile, const ImVec2& min, float size);
  void DrawTileMenu(const Tile& tile);
  void DrawFooter();
  void DrawEditors();
  void DrawPopups();
  void DrawClosePrompt();
  // Makes the last item a place to drop a dragged asset or folder into `folder`.
  void AcceptMove(const std::string& folder);

  std::vector<Tile> Tiles() const;
  bool PassesFilter(const content::AssetInfo& asset) const;
  const content::ModContent* ContentOf(std::string_view mod) const;
  const ModInfo* ModOf(std::string_view path) const;

  void Navigate(std::string folder);
  // Shows the game's tables, which are no mod's.
  void NavigateGameTables();
  void Open(const Tile& tile);
  void OpenEditor(const std::string& path, TileKind kind);
  // What the asset at `path` is, as the content was last read.
  TileKind KindOf(std::string_view path) const;
  // Calls `visit` with the content of `mod` and of each mod it depends on.
  void ForModAndDependencies(std::string_view mod, const std::function<void(const content::ModContent&)>& visit) const;
  void AskFor(Ask ask, std::string target, std::string text = {});

  void CreateMod(const std::string& name);
  void CreateFolder(const std::string& name);
  void CreateClass(const std::string& name, const std::string& parent);
  // A new enum or struct, opened in its editor.
  void CreateType(const std::string& name, TileKind kind);
  // A new data table of rows of `row`, opened in its editor.
  void CreateTable(const std::string& name, const std::string& row);
  // A new language asset of the game's texts in `language`, opened in its editor.
  void CreateLanguage(const std::string& name, const std::string& language);
  void CreateMapEdit(const std::string& name, const std::string& world);
  void CreateMaterial(const std::string& name, const std::string& type);
  // A new world built on `base`, opened in its editor.
  void CreateWorld(const std::string& name, const std::string& base);
  // A new packet of the game's packet base `base`, opened in its editor.
  void CreatePacket(const std::string& name, const std::string& base);
  // Opens the system's Open dialog for a picture (or a model, for a mesh) to import into `folder`; once one is chosen,
  // asks for its name.
  void StartImport(const std::string& folder, bool mesh = false);
  // What the Open dialog chose, taken each frame.
  void TakePicked();
  // A new texture named `name` of the picture at `source`, opened in its editor.
  void ImportImage(const std::string& name, const std::string& source);
  // A new mesh named `name` of the model at `source`, opened in its editor.
  void ImportMesh(const std::string& name, const std::string& source);
  void RenameTarget(const std::string& name);
  void DeleteTarget();
  void DuplicateAsset(const std::string& path);
  void MoveTo(const std::string& path, const std::string& folder);
  // An editor with unsaved changes that changing `path` would touch: one of it or inside it, or of an asset naming it.
  // Says so in the status and gives true.
  bool Busy(const std::string& path, const std::vector<content::Reference>& references);
  // After `from` became `to` (or went, for `to` empty): its editors close, and for a move open again where it went.
  void Relocated(const std::string& from, const std::string& to);

  void SetStatus(std::string text, bool error = false);

  MenuModel& model_;
  std::function<void()> closed_;
  bool closing_ = false;

  std::map<std::string, content::ModContent> content_;  // by lower case mod folder
  std::map<std::string, ClassLook> looks_;              // by lower case class path
  // The mods' enums and structs as their files were last read, by lower case path, and the lookup the editors use,
  // which finds the game's in the registry.
  std::map<std::string, content::Enum> enums_;
  std::map<std::string, content::Struct> structs_;
  content::TypeLookup types_;
  bool refresh_ = false;
  bool reload_ = false;
  std::string navigate_after_refresh_;

  std::string folder_;  // empty: every mod
  bool game_tables_ = false;  // the game's tables shown instead
  std::string selected_;
  bool reveal_ = true;  // the tree opens down to the folder shown
  std::string search_;
  std::string class_filter_;
  std::string combo_filter_;

  std::vector<std::unique_ptr<AssetEditor>> editors_;
  // The browser window as last drawn, for placing editors beside it.
  float window_left_ = 0.0f;
  float window_right_ = 0.0f;
  float window_top_ = 0.0f;

  Ask ask_ = Ask::kNone;
  bool open_ask_ = false;
  std::string ask_target_;
  std::string ask_text_;
  std::string ask_parent_;
  std::vector<content::Reference> ask_references_;
  bool open_close_prompt_ = false;

  // The Open dialog's answer, written on its thread and taken on the UI's.
  struct Picked {
    std::mutex mutex;
    bool done = false;
    std::optional<std::filesystem::path> file;
    std::string error;
    std::string folder;
    bool mesh = false;  // a model for a mesh, not a picture
  };
  std::shared_ptr<Picked> picked_ = std::make_shared<Picked>();
  bool picking_ = false;

  std::string status_;
  bool status_error_ = false;
  double status_time_ = 0.0;
};

}  // namespace rexfabric::ui
