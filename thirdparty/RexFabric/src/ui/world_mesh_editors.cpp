#include "world_mesh_editors.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "../content/map_edits.h"
#include "../content/packets.h"
#include "../content/types.h"
#include "editor_parts.h"
#include "file_picker.h"
#include "glass.h"
#include "value_widgets.h"
#include "widgets.h"

namespace rexfabric::ui {

namespace {

bool Same(std::string_view a, std::string_view b) { return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); }); }

// What a binary glTF holds, from its JSON chunk alone (the header, then the chunk's length and type).
struct ModelSummary {
  bool read = false;
  size_t nodes = 0;
  size_t meshes = 0;
  size_t parts = 0;  // primitives
  size_t materials = 0;
  size_t images = 0;
  uintmax_t bytes = 0;
};

ModelSummary Summarise(const std::filesystem::path& file) {
  constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
  constexpr uint32_t kJsonChunk = 0x4E4F534A;  // "JSON"
  ModelSummary summary;
  std::error_code error;
  summary.bytes = std::filesystem::file_size(file, error);
  std::ifstream in(file, std::ios::binary);
  uint32_t header[5] = {};
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kGlbMagic || header[4] != kJsonChunk) return summary;
  std::string text(header[3], '\0');
  if (!in.read(text.data(), std::streamsize(text.size()))) return summary;
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
  if (!json.is_object()) return summary;
  const auto count = [&json](const char* key) { return json.contains(key) && json[key].is_array() ? json[key].size() : size_t(0); };
  summary.read = true;
  summary.nodes = count("nodes");
  summary.meshes = count("meshes");
  summary.materials = count("materials");
  summary.images = count("images");
  for (const nlohmann::json& mesh : json.value("meshes", nlohmann::json::array())) summary.parts += mesh.value("primitives", nlohmann::json::array()).size();
  return summary;
}

class MeshEditor final : public DocumentEditor<content::MeshAsset> {
 public:
  MeshEditor(EditorHost& host, std::string path, content::MeshAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {}

  bool Save() override {
    const std::optional<std::filesystem::path> file = File();
    std::string error;
    if (!file || !content::WriteMeshAsset(*file, document_, &error)) {
      if (file) SetStatus(error, true);
      return false;
    }
    MarkSaved();
    host_.Saved(path_);
    SavedStatus(false);
    return true;
  }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (mesh)"; }

  void DrawContents() override {
    TakePicked();
    DrawToolbar(CanUndo(), CanRedo());
    const std::optional<std::filesystem::path> file = File();
    const std::filesystem::path model = file ? content::MeshModelFile(*file) : std::filesystem::path();
    if (model != summarised_) {
      summarised_ = model;
      summary_ = model.empty() ? ModelSummary{} : Summarise(model);
    }
    if (BeginLabelTable("##mesh")) {
      LabelRow("Help");
      widgets::InputString("##help", document_.help, 0, "What it's for");
      LabelRow("Model");
      ImGui::AlignTextToFramePadding();
      if (model.empty()) ImGui::TextColored(widgets::kErrorColor, "There's no model beside it");
      else if (!summary_.read) ImGui::TextColored(widgets::kErrorColor, "%s isn't a binary glTF RexFabric can read", model.filename().string().c_str());
      else ImGui::Text("%s, %.1f MB", model.filename().string().c_str(), double(summary_.bytes) / (1024.0 * 1024.0));
      ImGui::SameLine();
      ImGui::BeginDisabled(picking_);
      if (glass::SmallButton("Replace...")) {
        picking_ = true;
        file_picker::Choose(file_picker::Files::kMeshes, [picked = picked_](std::optional<std::filesystem::path> chosen, std::string error) {
          std::lock_guard lock(picked->mutex);
          picked->done = true;
          picked->file = std::move(chosen);
          picked->error = std::move(error);
        });
      }
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Takes another model in its place, at once: whatever shows it shows the new one once it's loaded again");
      if (summary_.read) {
        LabelRow("Holds");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%zu nodes, %zu meshes of %zu parts, %zu materials, %zu textures", summary_.nodes, summary_.meshes, summary_.parts, summary_.materials, summary_.images);
      }
      ImGui::EndTable();
    }
    Remember();
  }

 private:
  // The Open dialog's answer, written on its thread.
  struct Picked {
    std::mutex mutex;
    bool done = false;
    std::optional<std::filesystem::path> file;
    std::string error;
  };

  // A new model is taken in at once: it's a file beside the asset, not part of what Save writes.
  void TakePicked() {
    std::optional<std::filesystem::path> picked;
    std::string error;
    {
      std::lock_guard lock(picked_->mutex);
      if (!picked_->done) return;
      picked_->done = false;
      picked = std::move(picked_->file);
      error = std::move(picked_->error);
    }
    picking_ = false;
    if (!error.empty()) SetStatus(error, true);
    const std::optional<std::filesystem::path> file = picked ? File() : std::nullopt;
    if (!file) return;
    if (!content::SetMeshModel(*file, *picked, &error)) {
      SetStatus(error, true);
      return;
    }
    summarised_.clear();
    host_.Saved(path_);
    SetStatus("Took in " + picked->filename().string());
  }

  std::shared_ptr<Picked> picked_ = std::make_shared<Picked>();
  bool picking_ = false;
  std::filesystem::path summarised_;
  ModelSummary summary_;
};

class WorldEditor final : public DocumentEditor<content::WorldAsset> {
 public:
  WorldEditor(EditorHost& host, std::string path, content::WorldAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {}

  bool Save() override {
    const std::optional<std::filesystem::path> file = File();
    std::string error;
    if (!file || !content::WriteWorldAsset(*file, document_, &error)) {
      if (file) SetStatus(error, true);
      return false;
    }
    MarkSaved();
    host_.Saved(path_);
    SavedStatus(!Problem().empty());
    return true;
  }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (world)"; }

  void DrawContents() override {
    DrawToolbar(CanUndo(), CanRedo());
    if (BeginLabelTable("##world")) {
      LabelRow("Built on");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##base", document_.base.empty() ? "Pick a world" : document_.base.c_str())) {
        for (const content::WorldDecl& world : content::Worlds()) {
          if (Same(world.path, path_)) continue;
          if (ImGui::Selectable(world.path.c_str(), Same(world.path, document_.base))) document_.base = world.path;
          if (!world.help.empty()) ImGui::SetItemTooltip("%s", world.help.c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The world it's built on: entered as it, with this world's map edits made besides its own");
      LabelRow("Help");
      widgets::InputString("##help", document_.help, 0, "What it's for");
      ImGui::EndTable();
    }
    if (const std::string problem = Problem(); !problem.empty()) ImGui::TextColored(widgets::kErrorColor, "%s", problem.c_str());
    ImGui::TextDisabled("A map edit of this world changes it alone; the game enters it where something asks for it, as a signpost going there does.");
    Remember();
  }

 private:
  std::string Problem() const {
    if (document_.base.empty()) return "Pick the world it's built on";
    if (!content::HasWorld(document_.base)) return "There's no world " + document_.base + " (its mod may be off)";
    return {};
  }
};

class PacketEditor final : public DocumentEditor<content::PacketAsset> {
 public:
  PacketEditor(EditorHost& host, std::string path, content::PacketAsset asset) : DocumentEditor(host, std::move(path), std::move(asset)) {}

  bool Save() override {
    const std::optional<std::filesystem::path> file = File();
    std::string error;
    if (!file || !content::WritePacketAsset(*file, document_, &error)) {
      if (file) SetStatus(error, true);
      return false;
    }
    MarkSaved();
    host_.Saved(path_);
    SavedStatus(!Problem().empty());
    return true;
  }

 protected:
  std::string Title() const override { return std::string(content::NameOf(path_)) + " (packet)"; }

  void DrawContents() override {
    DrawToolbar(CanUndo(), CanRedo());
    const TypeChoices choices = host_.Choices(content::ModOf(path_));
    if (BeginLabelTable("##packet")) {
      DrawBase();
      LabelRow("Name");
      widgets::InputString("##name", document_.name, 0, "Shown where it's picked and sold");
      LabelRow("Help");
      widgets::InputString("##help", document_.help, 0, "What the shop says of it");
      LabelRow("Icon");
      nlohmann::json icon = document_.icon.empty() ? nlohmann::json() : nlohmann::json(document_.icon);
      if (ValueField("##icon", {.type = content::PropertyType::kAsset, .of = std::string(kTextureClass)}, icon, host_.Types(), choices)) document_.icon = icon.is_string() ? icon.get<std::string>() : std::string();
      ImGui::SetItemTooltip("The picture on its star menu leaf and its shop reel");
      LabelRow("Look");
      const std::string look = content::PacketLookStruct(document_.base);
      if (look.empty()) ImGui::TextDisabled("Its base has no look to set");
      else ValueField("##look", {.type = content::PropertyType::kStruct, .of = look}, document_.look, host_.Types(), choices);
      DrawUnlock();
      LabelRow("Order");
      ImGui::InputDouble("##order", &document_.order, 1.0, 10.0, "%g");
      ImGui::SetItemTooltip("Where it sorts among its mod's packets, lower first; equal ones by path");
      ImGui::EndTable();
    }
    if (const std::string problem = Problem(); !problem.empty()) ImGui::TextColored(widgets::kErrorColor, "%s", problem.c_str());
    ImGui::TextDisabled("It pours, grows and counts as its base does; the look and the unlock are its own.");
    Remember();
  }

 private:
  static constexpr std::string_view kTextureClass = "Texture";

  // The base, from the game's packet base enum. A new base starts its look over, as each base's look is its own.
  void DrawBase() {
    LabelRow("Pours as");
    const std::optional<content::Enum> bases = content::FindEnum(content::PacketBaseEnum());
    if (ImGui::BeginCombo("##base", document_.base.empty() ? "Pick a base" : document_.base.c_str())) {
      for (const content::EnumValue& base : bases ? bases->values : std::vector<content::EnumValue>{}) {
        if (ImGui::Selectable(base.name.c_str(), base.name == document_.base) && base.name != document_.base) {
          document_.base = base.name;
          document_.look = nlohmann::json::object();
        }
        if (!base.help.empty()) ImGui::SetItemTooltip("%s", base.help.c_str());
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The game's packet it's a variant of");
  }

  void DrawUnlock() {
    LabelRow("Unlock");
    const bool shop = document_.unlock == content::PacketUnlock::kShop;
    if (ImGui::BeginCombo("##unlock", shop ? "In the shop" : "By default")) {
      if (ImGui::Selectable("By default", !shop)) document_.unlock = content::PacketUnlock::kDefault;
      ImGui::SetItemTooltip("Every player has it from the start");
      if (ImGui::Selectable("In the shop", shop)) document_.unlock = content::PacketUnlock::kShop;
      ImGui::SetItemTooltip("Bought in the shop, from its level, for its price");
      ImGui::EndCombo();
    }
    if (!shop) return;
    LabelRow("Shop level");
    ImGui::InputScalar("##level", ImGuiDataType_S64, &document_.level);
    LabelRow("Price");
    ImGui::InputScalar("##price", ImGuiDataType_S64, &document_.price);
  }

  std::string Problem() const {
    if (document_.base.empty()) return "Pick the packet it pours as";
    const std::optional<content::Enum> bases = content::FindEnum(content::PacketBaseEnum());
    const bool known = bases && std::any_of(bases->values.begin(), bases->values.end(), [this](const content::EnumValue& value) { return value.name == document_.base; });
    if (!known) return "\"" + document_.base + "\" isn't one of the game's packets";
    if (document_.name.empty()) return "Give it a name";
    if (document_.unlock == content::PacketUnlock::kShop && (document_.level < 0 || document_.price < 0)) return "Its shop level and price can't be below 0";
    return {};
  }
};

}  // namespace

std::unique_ptr<AssetEditor> OpenMeshEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::MeshAsset> asset = mod ? content::ReadMeshAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<MeshEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenWorldEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::WorldAsset> asset = mod ? content::ReadWorldAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<WorldEditor>(host, path, std::move(*asset)) : nullptr;
}

std::unique_ptr<AssetEditor> OpenPacketEditor(EditorHost& host, const std::string& path) {
  const ModInfo* mod = host.model().mods().Find(content::ModOf(path));
  std::optional<content::PacketAsset> asset = mod ? content::ReadPacketAsset(content::AssetFile(*mod, path), nullptr) : std::nullopt;
  return asset ? std::make_unique<PacketEditor>(host, path, std::move(*asset)) : nullptr;
}

}  // namespace rexfabric::ui
