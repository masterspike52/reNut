#include "imgui_menu.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <utility>

#include <imgui.h>

#include <rex/hid/input_system.h>
#include <rexfabric/rexfabric.h>

#include "../platform.h"
#include "glass.h"

namespace rexfabric::ui {
namespace {

constexpr float kIconSize = 40.0f;
constexpr float kCardIconSize = 64.0f;
constexpr float kRowPad = 6.0f;
constexpr float kButtonZone = 108.0f;
constexpr int kGridColumns = 3;
constexpr int kGridRows = 2;
constexpr int kGridSize = kGridColumns * kGridRows;
// A search per keystroke would be a request per character.
constexpr double kSearchDelay = 0.45;

rex::hid::IInputSystem* GuestInput() { return REX_INTERFACE(rex::hid::IInputSystem); }

std::string Clip(const std::string& text, float width) {
  if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width) return text;
  const float ellipsis = ImGui::CalcTextSize("...").x;
  size_t length = text.size();
  while (length > 0) {
    --length;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
    if (ImGui::CalcTextSize(text.c_str(), text.c_str() + length).x + ellipsis <= width) break;
  }
  return text.substr(0, length) + "...";
}

std::string Count(uint64_t count) {
  char text[32];
  if (count >= 1000000) std::snprintf(text, sizeof(text), "%.1fM", count / 1000000.0);
  else if (count >= 1000) std::snprintf(text, sizeof(text), "%.1fk", count / 1000.0);
  else std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(count));
  return text;
}

void Centered(const char* text) {
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x) * 0.5f));
  ImGui::TextUnformatted(text);
}

void Icon(ImTextureID texture, float size) {
  if (texture != ImTextureID_Invalid) {
    ImGui::Image(texture, ImVec2(size, size));
    return;
  }
  const ImVec2 corner = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(corner, ImVec2(corner.x + size, corner.y + size), IM_COL32(38, 42, 50, 255), 3.0f);
  ImGui::Dummy(ImVec2(size, size));
}

float ButtonWidth(const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f; }

}  // namespace

ModsWindow::ModsWindow(MenuModel& model, std::function<void()> closed) : model_(model), closed_(std::move(closed)) {
  if (auto* input = GuestInput()) input->AcquireUIBlock();
}

ModsWindow::~ModsWindow() {
  if (auto* input = GuestInput()) input->ReleaseUIBlock();
  if (closed_) closed_();
}

bool ModsWindow::Draw(ImGuiIO& io) {
  if (closing_) return false;
  OnDraw(io);
  return !closing_;
}

void ModsWindow::OnDraw(ImGuiIO& io) {
  model_.Poll();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(940.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
  bool open = true;
  if (glass::BeginWindow("Mods##rexfabric", &open, ImGuiWindowFlags_NoCollapse)) DrawMods();
  ImGui::End();
  if (browser_open_) DrawBrowser();
  if (pending_) std::exchange(pending_, {})();
  if (!open) Hide();
}

// The mod list ------------------------------------------------------------------------------------------

void ModsWindow::DrawMods() {
  const ImGuiStyle& style = ImGui::GetStyle();
  if (model_.restart_needed()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Restart to apply changes");
    ImGui::SameLine();
    if (ImGui::Button("Restart")) pending_ = [this] { model_.Restart(); };
  } else {
    ImGui::NewLine();
  }
  ImGui::Separator();

  const float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
  const float list_height = ImGui::GetContentRegionAvail().y - footer - ImGui::GetTextLineHeightWithSpacing();
  const float column_width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
  ImGui::BeginGroup();
  ImGui::TextUnformatted("Available");
  DrawColumn("##available", false, column_width, list_height);
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted("Enabled");
  DrawColumn("##enabled", true, column_width, list_height);
  ImGui::EndGroup();

  if (ImGui::Button("Open Mods Folder")) model_.OpenModsFolder();
  if (model_.thunderstore()) {
    ImGui::SameLine();
    if (ImGui::Button("Browse Mods")) browser_open_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload Scripts")) pending_ = [this] { model_.ReloadScripts(); };
#if REXFABRIC_WITH_NODE_EDITOR
  ImGui::SameLine();
  if (ImGui::Button("Node Editor")) pending_ = [] { ShowNodeEditor(); };
#endif
}

void ModsWindow::DrawColumn(const char* id, bool enabled, float width, float height) {
  ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders);
  size_t shown = 0;
  for (size_t i = 0; i < model_.mods().mods().size(); ++i) {
    if (model_.mods().mods()[i].enabled != enabled) continue;
    ++shown;
    ImGui::PushID(static_cast<int>(i));
    DrawRow(i, enabled);
    ImGui::PopID();
  }
  if (!shown) ImGui::TextDisabled(enabled ? "No mods enabled" : "Nothing available");
  ImGui::EndChild();
}

void ModsWindow::DrawRow(size_t index, bool enabled_column) {
  const ModInfo& mod = model_.mods().mods()[index];
  const ImGuiStyle& style = ImGui::GetStyle();
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float row_height = std::max(kIconSize, line * 3.0f) + kRowPad * 2.0f;
  const ImVec2 origin = ImGui::GetCursorPos();
  const ImVec2 screen = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(screen, ImVec2(screen.x + width, screen.y + row_height));
  if (hovered) ImGui::GetWindowDrawList()->AddRectFilled(screen, ImVec2(screen.x + width, screen.y + row_height), IM_COL32(255, 255, 255, 20), 3.0f);

  ImGui::SetCursorPos(ImVec2(origin.x + kRowPad, origin.y + (row_height - kIconSize) * 0.5f));
  Icon(model_.ModIcon(mod), kIconSize);

  const float text_x = origin.x + kRowPad * 2.0f + kIconSize;
  const float text_width = std::max(40.0f, width - (text_x - origin.x) - kButtonZone);
  ImGui::SetCursorPos(ImVec2(text_x, origin.y + kRowPad));
  ImGui::TextUnformatted(Clip(mod.name, text_width).c_str());
  std::string meta = mod.version.empty() ? std::string() : "v" + mod.version;
  if (!mod.author.empty()) meta += (meta.empty() ? "by " : " by ") + mod.author;
  if (meta.empty()) meta = mod.folder;
  ImGui::SetCursorPos(ImVec2(text_x, origin.y + kRowPad + line));
  ImGui::TextDisabled("%s", Clip(meta, text_width).c_str());
  if (!mod.description.empty()) {
    ImGui::SetCursorPos(ImVec2(text_x, origin.y + kRowPad + line * 2.0f));
    ImGui::TextDisabled("%s", Clip(mod.description, text_width).c_str());
  }

  if (hovered) {
    const float button = ImGui::GetFrameHeight();
    const int buttons = enabled_column ? 3 : 1;
    ImGui::SetCursorPos(ImVec2(origin.x + width - buttons * button - (buttons - 1) * style.ItemSpacing.x - kRowPad, origin.y + (row_height - button) * 0.5f));
    if (enabled_column) {
      if (ImGui::ArrowButton("##disable", ImGuiDir_Left)) pending_ = [this, index] { model_.SetEnabled(index, false); };
      ImGui::SameLine();
      ImGui::BeginDisabled(index == 0);
      if (ImGui::ArrowButton("##up", ImGuiDir_Up)) pending_ = [this, index] { model_.Move(index, -1); };
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(index + 1 >= model_.mods().EnabledCount());
      if (ImGui::ArrowButton("##down", ImGuiDir_Down)) pending_ = [this, index] { model_.Move(index, 1); };
      ImGui::EndDisabled();
    } else if (ImGui::ArrowButton("##enable", ImGuiDir_Right)) {
      pending_ = [this, index] { model_.SetEnabled(index, true); };
    }
  }
  ImGui::SetCursorPos(origin);
  ImGui::Dummy(ImVec2(width, row_height));
}

// The Thunderstore browser ------------------------------------------------------------------------------

void ModsWindow::DrawBrowser() {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(1040.0f, 660.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 460.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (glass::BeginWindow("Browse Mods##rexfabric", &browser_open_, ImGuiWindowFlags_NoCollapse)) {
    DrawToolbar();
    ImGui::Separator();
    const float pager = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    int pages = 1;
    DrawGrid(std::max(kCardIconSize, ImGui::GetContentRegionAvail().y - pager), pages);
    DrawPager(pages);
  }
  ImGui::End();
}

void ModsWindow::DrawToolbar() {
  const ImGuiStyle& style = ImGui::GetStyle();
  net::Thunderstore& store = *model_.thunderstore();
  net::Query& query = model_.query();
  const auto reset_paging = [this] { page_ = 0; page_input_ = 1; };
  constexpr float kSortWidth = 170.0f;
  constexpr float kCategoryWidth = 190.0f;
  const float deprecated_width = ImGui::CalcTextSize("Deprecated").x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;

  ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - kSortWidth - kCategoryWidth - deprecated_width - style.ItemSpacing.x * 3.0f));
  if (ImGui::InputTextWithHint("##search", "Search", search_, sizeof(search_))) search_typed_at_ = ImGui::GetTime();
  const bool apply = ImGui::IsItemDeactivatedAfterEdit() || (search_typed_at_ != 0.0 && ImGui::GetTime() - search_typed_at_ >= kSearchDelay);
  if (apply) {
    search_typed_at_ = 0.0;
    if (query.search != search_) {
      query.search = search_;
      reset_paging();
    }
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(kSortWidth);
  const char* sort_label = net::kOrderings[0].label;
  for (const net::Ordering& ordering : net::kOrderings) {
    if (query.ordering == ordering.value) sort_label = ordering.label;
  }
  if (ImGui::BeginCombo("##sort", sort_label)) {
    for (const net::Ordering& ordering : net::kOrderings) {
      if (ImGui::Selectable(ordering.label, query.ordering == ordering.value) && query.ordering != ordering.value) {
        query.ordering = ordering.value;
        reset_paging();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(kCategoryWidth);
  const std::vector<net::Category>& categories = store.Categories();
  const char* category_label = "All categories";
  for (const net::Category& category : categories) {
    if (category.id == query.category_id) category_label = category.name.c_str();
  }
  if (ImGui::BeginCombo("##category", category_label)) {
    if (ImGui::Selectable("All categories", query.category_id.empty()) && !query.category_id.empty()) {
      query.category_id.clear();
      reset_paging();
    }
    for (const net::Category& category : categories) {
      if (ImGui::Selectable(category.name.c_str(), category.id == query.category_id) && category.id != query.category_id) {
        query.category_id = category.id;
        reset_paging();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  if (ImGui::Checkbox("Deprecated", &query.deprecated)) reset_paging();
}

void ModsWindow::DrawGrid(float height, int& pages) {
  const ImGuiStyle& style = ImGui::GetStyle();
  net::Thunderstore& store = *model_.thunderstore();
  const size_t first = static_cast<size_t>(page_) * kGridSize;

  // A grid page can straddle two API pages. The second waits for the first, so a page past the end is
  // never asked for.
  net::Query primary = model_.query();
  primary.page = static_cast<int>(first / net::Thunderstore::kPageSize) + 1;
  const net::Thunderstore::Page& page_a = store.GetPage(primary);
  const int second = static_cast<int>((first + kGridSize - 1) / net::Thunderstore::kPageSize) + 1;
  const net::Thunderstore::Page* page_b = nullptr;
  if (second != primary.page && page_a.status == net::Status::kReady && page_a.total > static_cast<uint32_t>(second - 1) * net::Thunderstore::kPageSize) {
    net::Query secondary = primary;
    secondary.page = second;
    page_b = &store.GetPage(secondary);
  }
  pages = std::max(1, static_cast<int>((page_a.total + kGridSize - 1) / kGridSize));
  if (page_ >= pages && page_a.status == net::Status::kReady) {
    page_ = pages - 1;
    page_input_ = page_ + 1;
  }

  ImGui::BeginChild("##grid", ImVec2(0.0f, height));
  if (page_a.status == net::Status::kError) {
    ImGui::Spacing();
    Centered(page_a.error.empty() ? "Could not load mods" : page_a.error.c_str());
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - ButtonWidth("Try Again")) * 0.5f));
    if (ImGui::Button("Try Again")) store.Invalidate();
  } else if (page_a.status != net::Status::kReady) {
    ImGui::Spacing();
    Centered("Loading");
  } else if (page_a.total == 0) {
    ImGui::Spacing();
    Centered("No mods found");
  } else {
    const auto package_at = [&](size_t index) -> const net::Package* {
      const int number = static_cast<int>(index / net::Thunderstore::kPageSize) + 1;
      const net::Thunderstore::Page* source = number == primary.page ? &page_a : number == second ? page_b : nullptr;
      if (!source || source->status != net::Status::kReady) return nullptr;
      const size_t offset = index % net::Thunderstore::kPageSize;
      return offset < source->packages.size() ? &source->packages[offset] : nullptr;
    };
    const ImVec2 cell((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (kGridColumns - 1)) / kGridColumns, (ImGui::GetContentRegionAvail().y - style.ItemSpacing.y * (kGridRows - 1)) / kGridRows);
    for (int row = 0; row < kGridRows; ++row) {
      for (int column = 0; column < kGridColumns; ++column) {
        if (column > 0) ImGui::SameLine();
        const size_t index = first + static_cast<size_t>(row * kGridColumns + column);
        ImGui::PushID(static_cast<int>(index));
        if (const net::Package* package = package_at(index)) DrawCard(*package, cell);
        else ImGui::Dummy(cell);
        ImGui::PopID();
      }
    }
  }
  ImGui::EndChild();
}

void ModsWindow::DrawCard(const net::Package& package, const ImVec2& size) {
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::BeginChild("##card", size, ImGuiChildFlags_Borders);
  const float text_width = std::max(40.0f, ImGui::GetContentRegionAvail().x - kCardIconSize - style.ItemSpacing.x);
  Icon(model_.PackageIcon(package), kCardIconSize);
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(Clip(package.name, text_width).c_str());
  ImGui::TextDisabled("%s", Clip("by " + package.owner, text_width).c_str());
  ImGui::TextDisabled("%s downloads, %s likes", Count(package.downloads).c_str(), Count(package.likes).c_str());
  if (package.deprecated) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Deprecated");
  ImGui::EndGroup();
  ImGui::Separator();

  ImGui::BeginChild("##about", ImVec2(0.0f, std::max(ImGui::GetTextLineHeight(), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())));
  ImGui::TextWrapped("%s", package.description.c_str());
  ImGui::EndChild();

  const net::Thunderstore::Install* install = model_.thunderstore()->GetInstall(package);
  const bool installing = install && install->status == net::Status::kLoading;
  const bool failed = install && install->status == net::Status::kError;
  const char* install_label = installing ? "Installing" : failed ? "Retry" : model_.IsInstalled(package.name) ? "Reinstall" : "Install";
  const float buttons = ButtonWidth(install_label) + ButtonWidth("View") + style.ItemSpacing.x;
  std::string categories;
  for (const std::string& category : package.categories) categories += (categories.empty() ? "" : ", ") + category;
  if (failed) categories = install->error;
  if (installing) categories = install->step;
  if (!categories.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Clip(categories, std::max(20.0f, ImGui::GetContentRegionAvail().x - buttons - style.ItemSpacing.x)).c_str());
    ImGui::SameLine();
  }
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
  ImGui::BeginDisabled(installing);
  if (ImGui::Button(install_label)) model_.Install(package);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("View")) OpenUrl(package.page_url);
  ImGui::EndChild();
}

void ModsWindow::DrawPager(int pages) {
  const ImGuiStyle& style = ImGui::GetStyle();
  char counter[32];
  std::snprintf(counter, sizeof(counter), "/ %d", pages);
  constexpr float kPageInputWidth = 64.0f;
  const float cluster = ButtonWidth("<<") * 2.0f + ImGui::GetFrameHeight() * 2.0f + kPageInputWidth + ImGui::CalcTextSize(counter).x + style.ItemSpacing.x * 5.0f;
  const auto go = [this, pages](int page) { page_ = std::clamp(page, 0, pages - 1); page_input_ = page_ + 1; };
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - cluster) * 0.5f));

  ImGui::BeginDisabled(page_ <= 0);
  if (ImGui::Button("<<")) go(0);
  ImGui::SameLine();
  if (ImGui::ArrowButton("##previous", ImGuiDir_Left)) go(page_ - 1);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(kPageInputWidth);
  if (ImGui::InputInt("##page", &page_input_, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) go(page_input_ - 1);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(counter);
  ImGui::SameLine();
  ImGui::BeginDisabled(page_ + 1 >= pages);
  if (ImGui::ArrowButton("##next", ImGuiDir_Right)) go(page_ + 1);
  ImGui::SameLine();
  if (ImGui::Button(">>")) go(pages - 1);
  ImGui::EndDisabled();
}

}  // namespace rexfabric::ui
