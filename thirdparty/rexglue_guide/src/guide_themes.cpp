// Themes: picking one, and the Theme Designer (trusted players and admins).

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio.h"
#include "file_picker.h"
#include "guide_dialog.h"
#include "photo.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// Background pictures picked in the system file dialog, prepared on its thread
// and taken into the designer on the Guide's.
struct ImagePick {
  std::string jpg;
  std::string error;
};
std::mutex g_image_mutex;
std::vector<ImagePick> g_image_picks;

void OnImageChosen(std::optional<std::filesystem::path> file, std::string error) {
  if (!file && error.empty()) return;  // cancelled
  ImagePick pick;
  if (file) pick.jpg = photo::BackgroundFromFile(*file, &pick.error);
  else pick.error = std::move(error);
  std::lock_guard lock(g_image_mutex);
  g_image_picks.push_back(std::move(pick));
}

std::string Percent(float value) {
  return std::to_string(int(std::round(value * 100.0f))) + "%";
}

uint8_t ChannelOf(ImU32 color, int shift) {
  return uint8_t((color >> shift) & 0xFF);
}

ImU32 WithChannel(ImU32 color, int shift, float value) {
  const ImU32 v = ImU32(std::clamp(std::round(value * 255.0f), 0.0f, 255.0f));
  return (color & ~(ImU32(0xFF) << shift)) | (v << shift);
}

}  // namespace

// ------------------------------------------------------------------ picking --

themes::Theme GuideDialog::FromServer(const ServerTheme& server) {
  themes::Theme theme;
  theme.key = "server:" + std::to_string(server.id);
  theme.name = server.name;
  theme.author = server.author;
  theme.author_xuid = server.author_xuid;
  theme.server_id = server.id;
  theme.updated = server.updated;
  theme.has_image = server.has_image;
  theme.nsfw = server.nsfw;
  theme.likes = server.likes;
  theme.users = server.users;
  theme.liked = server.liked;
  theme.created = server.created;
  themes::ParseData(server.data, &theme);
  return theme;
}

// ------------------------------------------------------------------ gallery --

namespace {

constexpr int kGalleryColumns = 3;

const char* SortName(int sort) {
  static constexpr const char* kNames[] = {"Most Liked", "Most Used", "Newest", "Name"};
  return kNames[std::clamp(sort, 0, int(std::size(kNames)) - 1)];
}

// A heart: two circles over a point.
void Heart(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
  const float r = size * 0.27f;
  dl->AddCircleFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.55f), r, col, 16);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.55f), r, col, 16);
  dl->AddTriangleFilled(ImVec2(c.x - r * 1.9f, c.y - r * 0.3f), ImVec2(c.x + r * 1.9f, c.y - r * 0.3f),
                        ImVec2(c.x, c.y + size * 0.48f), col);
}

}  // namespace

// Built in first, then the rest in the chosen order; NSFW ones only when the
// player has asked to see them.
std::vector<themes::Theme> GuideDialog::GalleryThemes() const {
  std::vector<themes::Theme> server;
  for (const ServerTheme& t : backend_->themes()) {
    if (t.nsfw && !show_nsfw_) continue;
    server.push_back(FromServer(t));
  }
  std::stable_sort(server.begin(), server.end(), [this](const themes::Theme& l, const themes::Theme& r) {
    switch (theme_sort_) {
      case ThemeSort::kMostLiked: return l.likes != r.likes ? l.likes > r.likes : l.users > r.users;
      case ThemeSort::kMostUsed: return l.users != r.users ? l.users > r.users : l.likes > r.likes;
      case ThemeSort::kNewest: return l.created > r.created;
      default: return l.name < r.name;
    }
  });
  std::vector<themes::Theme> list = themes::BuiltIn();
  list.insert(list.end(), server.begin(), server.end());
  return list;
}

void GuideDialog::DrawThemeGallery(ImDrawList* dl, ImVec2 pos, ImVec2 end, Page& page, float a) {
  gallery_ = GalleryThemes();
  gallery_cells_.clear();
  // The order, and what's hidden, above the tiles.
  const std::string sort = std::string("Sort: ") + SortName(int(theme_sort_));
  const float sort_w = CapsWidth(Semibold(), Px(14), sort, Px(1.2f));
  const ImVec2 sort_pos(end.x - sort_w - Px(20), pos.y - Px(36));
  Caps(dl, Semibold(), Px(14), sort_pos, WithAlpha(color::kText, a), sort, Px(1.2f));
  sort_hit_ = {ImVec2(sort_pos.x - Px(8), sort_pos.y - Px(6)), ImVec2(sort_pos.x + sort_w + Px(8), sort_pos.y + Px(22))};
  if (!show_nsfw_) {
    const std::string hidden = "NSFW hidden";
    Caps(dl, Semibold(), Px(14), ImVec2(sort_pos.x - CapsWidth(Semibold(), Px(14), hidden, Px(1.2f)) - Px(28), sort_pos.y),
         WithAlpha(color::kTextDim, a), hidden, Px(1.2f));
  }

  const float gap = Px(14);
  const float tile_w = std::floor((end.x - pos.x - Px(14) - gap * (kGalleryColumns - 1)) / kGalleryColumns);
  const float tile_h = std::floor(tile_w * 0.56f) + Px(70);
  const float pitch = tile_h + gap;
  const int count = int(gallery_.size());
  const int rows = (count + kGalleryColumns - 1) / kGalleryColumns;
  const float fit = (end.y - pos.y + gap) / pitch;  // rows the view holds, part ones too
  const int visible = std::max(1, int(fit));
  page.selected = std::clamp(page.selected, 0, std::max(0, count - 1));
  const int sel_row = page.selected / kGalleryColumns;
  float follow = page.scroll;
  if (sel_row < follow) follow = float(sel_row);
  if (sel_row >= follow + visible) follow = float(sel_row - visible + 1);
  const float target = ScrollTowards(page, follow, float(rows) - fit);
  if (page.free_scroll) page.selected = KeepInGrid(page.selected, count, kGalleryColumns, target, fit);

  dl->PushClipRect(ImVec2(pos.x - Px(8), pos.y - Px(8)), ImVec2(end.x, end.y), true);
  for (int i = 0; i < count; ++i) {
    const ImVec2 t0(pos.x + float(i % kGalleryColumns) * (tile_w + gap), pos.y + (float(i / kGalleryColumns) - page.scroll) * pitch);
    const ImVec2 t1(t0.x + tile_w, t0.y + tile_h);
    gallery_cells_.push_back({t0, t1});
    if (t1.y < pos.y - pitch || t0.y > end.y) continue;
    DrawThemeTile(dl, gallery_[i], t0, t1, i == page.selected, a);
  }
  dl->PopClipRect();
}

// A small Guide in the theme's colours (and picture) over its name, maker,
// likes and players.
void GuideDialog::DrawThemeTile(ImDrawList* dl, const themes::Theme& t, ImVec2 p0, ImVec2 p1, bool selected, float a) {
  const themes::Palette& p = t.palette(dark_mode_);
  const float round = Px(8);
  if (selected) {
    dl->AddRectFilled(ImVec2(p0.x - Px(5), p0.y - Px(5)), ImVec2(p1.x + Px(5), p1.y + Px(5)), WithAlpha(color::kGreen, a),
                      round + Px(4));
  }
  dl->AddRectFilled(p0, p1, WithAlpha(IM_COL32(0, 0, 0, 150), a), round);

  // The preview: blade, picture, strip, a selected row and two plain ones.
  const float w = p1.x - p0.x;
  const ImVec2 v0(p0.x, p0.y), v1(p1.x, p0.y + std::floor(w * 0.56f));
  dl->AddRectFilled(v0, v1, WithAlpha(p.colors[themes::kBlade], a), round, ImDrawFlags_RoundCornersTop);
  if (t.server_id && t.has_image) {
    ImVec2 size;
    const std::string key = t.key + "_" + std::to_string(t.updated);
    const ImTextureID tex = thumbnails_.Get(textures_, key, backend_->ThemeImage(t.server_id, t.updated), t.image_blur, &size);
    if (tex && size.x > 0 && size.y > 0) {
      const ImVec2 area(v1.x - v0.x, v1.y - v0.y);
      const float scale = std::max(area.x / size.x, area.y / size.y);
      const float u = area.x / scale / size.x, v = area.y / scale / size.y;
      dl->AddImageRounded(tex, v0, v1, ImVec2((1 - u) * 0.5f, (1 - v) * 0.5f), ImVec2((1 + u) * 0.5f, (1 + v) * 0.5f),
                          WithAlpha(color::kWhite, t.image_alpha * a), round, ImDrawFlags_RoundCornersTop);
    }
  }
  const float s = (v1.y - v0.y) / 10.0f;  // preview units
  dl->AddRectFilled(ImVec2(v0.x + s * 0.5f, v0.y + s * 0.5f), ImVec2(v0.x + s * 2.0f, v1.y - s * 0.5f),
                    WithAlpha(p.colors[themes::kBladeStrip], a), s * 0.4f);
  dl->AddCircle(ImVec2(v0.x + s * 1.25f, v0.y + s * 1.6f), s * 0.45f, WithAlpha(p.colors[themes::kAccentLight], a), 16, s * 0.18f);
  const float rx0 = v0.x + s * 2.6f, rx1 = v1.x - s * 0.6f;
  dl->AddRectFilled(ImVec2(rx0, v0.y + s * 1.0f), ImVec2(rx1, v0.y + s * 2.6f), WithAlpha(p.colors[themes::kAccent], a), s * 0.3f);
  dl->AddRectFilled(ImVec2(rx0 + s * 0.5f, v0.y + s * 1.55f), ImVec2(rx0 + (rx1 - rx0) * 0.5f, v0.y + s * 2.05f),
                    WithAlpha(p.colors[themes::kText], a), s * 0.2f);
  for (int row = 0; row < 2; ++row) {
    const float y = v0.y + s * (3.6f + row * 1.7f);
    dl->AddRectFilled(ImVec2(rx0 + s * 0.5f, y), ImVec2(rx0 + (rx1 - rx0) * (row ? 0.45f : 0.6f), y + s * 0.5f),
                      WithAlpha(p.colors[themes::kText], a), s * 0.2f);
    dl->AddRectFilled(ImVec2(rx0 + s * 0.5f, y + s * 0.75f), ImVec2(rx0 + (rx1 - rx0) * 0.35f, y + s * 1.05f),
                      WithAlpha(p.colors[themes::kTextDim], a), s * 0.15f);
  }

  // Name, maker, likes and players.
  const float tx = p0.x + Px(10), max_w = w - Px(20);
  TextFit(dl, Regular(), Px(22), ImVec2(tx, v1.y + Px(6)), WithAlpha(color::kText, a), t.name, max_w - Px(26));
  if (t.key == theme_.key) DrawGlyph(dl, Glyph::kCheck, ImVec2(p1.x - Px(18), v1.y + Px(19)), Px(22), WithAlpha(color::kText, a));
  const ImU32 dim = WithAlpha(color::kTextDim, a);
  TextFit(dl, Regular(), Px(16), ImVec2(tx, v1.y + Px(32)), dim, t.server_id ? "by " + t.author : "Built in", max_w * 0.5f);
  if (t.server_id) {
    float x = p1.x - Px(10);
    const std::string users = std::to_string(t.users);
    x -= Measure(Regular(), Px(16), users).x;
    Text(dl, Regular(), Px(16), ImVec2(x, v1.y + Px(32)), dim, users);
    DrawGlyph(dl, Glyph::kPeople, ImVec2(x - Px(12), v1.y + Px(42)), Px(18), dim);
    x -= Px(30);
    const std::string likes = std::to_string(t.likes);
    x -= Measure(Regular(), Px(16), likes).x;
    Text(dl, Regular(), Px(16), ImVec2(x, v1.y + Px(32)), dim, likes);
    Heart(dl, ImVec2(x - Px(12), v1.y + Px(42)), Px(15), t.liked ? WithAlpha(color::kGreenLight, a) : dim);
    if (t.nsfw) {
      const ImVec2 n0(p1.x - Px(54), p0.y + Px(6)), n1(p1.x - Px(6), p0.y + Px(24));
      dl->AddRectFilled(n0, n1, WithAlpha(IM_COL32(190, 40, 40, 230), a), Px(4));
      Caps(dl, Semibold(), Px(11), ImVec2(n0.x + Px(5), n0.y + Px(3)), WithAlpha(color::kWhite, a), "NSFW", Px(0.8f));
    }
  }
}

void GuideDialog::HandleGalleryInput(const NavInput& in, Page& page) {
  const int count = int(gallery_.size());
  int sel = std::clamp(page.selected, 0, std::max(0, count - 1));
  if (in.left && sel > 0) --sel;
  if (in.right && sel + 1 < count) ++sel;
  if (in.up && sel >= kGalleryColumns) sel -= kGalleryColumns;
  if (in.down && sel + kGalleryColumns < count) sel += kGalleryColumns;
  if (in.left || in.right || in.up || in.down) page.free_scroll = false;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.MouseWheel != 0.0f) {
    HoldScroll(page);
    page.scroll_goal -= io.MouseWheel;  // a row a notch
  }
  if (sel != page.selected) audio::Play(Sound::kMove);
  page.selected = sel;
  bool use = in.accept;
  for (size_t i = 0; i < gallery_cells_.size(); ++i) {
    if (!ImGui::IsMouseHoveringRect(gallery_cells_[i].first, gallery_cells_[i].second, false)) continue;
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
      HoldScroll(page);
      page.selected = int(i);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      HoldScroll(page);
      page.selected = int(i);
      use = true;
    }
  }
  const bool sort_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                            ImGui::IsMouseHoveringRect(sort_hit_.first, sort_hit_.second, false);
  if (in.x || sort_clicked) {
    theme_sort_ = ThemeSort((int(theme_sort_) + 1) % int(ThemeSort::kCount));
    page.selected = 0;
    page.scroll = page.scroll_goal = 0.0f;
    page.free_scroll = false;
    audio::Play(Sound::kSelect);
    SavePreferences();
  } else if (in.y && page.selected < count) {
    LikeTheme(gallery_[page.selected]);
  } else if (use && page.selected < count) {
    audio::Play(Sound::kSelect);
    SelectTheme(gallery_[page.selected]);
  } else if (in.back || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    Pop();
  }
}

void GuideDialog::LikeTheme(const themes::Theme& theme) {
  if (!theme.server_id) return;  // built in
  if (!backend_->signed_in()) return ShowMessage({"Like Theme", "Sign in to like themes.", {"OK"}});
  audio::Play(Sound::kSelect);
  backend_->LikeTheme(theme.server_id, !theme.liked, [this](const Result& r) {
    if (!r.ok) ShowMessage({"Like Theme", r.error, {"OK"}});
  });
}

void GuideDialog::SelectTheme(const themes::Theme& theme) {
  theme_ = theme;
  SavePreferences();
}

void GuideDialog::FollowSelectedTheme() {
  for (auto& b : backends_) {
    if (b) b->SetThemeInUse(theme_.server_id);
  }
  if (!theme_.server_id || !backend_->themes_ready()) return;
  for (const ServerTheme& t : backend_->themes()) {
    if (t.id != theme_.server_id) continue;
    // Marked NSFW since, for a player who hides those: back to the default.
    if (t.nsfw && !show_nsfw_) SelectTheme(themes::Default());
    else if (t.updated != theme_.updated) SelectTheme(FromServer(t));
    return;
  }
  // Removed on the server (by its maker or an admin).
  SelectTheme(themes::Default());
}

const themes::Theme& GuideDialog::ShownTheme() const {
  return draft_ ? draft_->theme : theme_;
}

bool GuideDialog::ShownDark() const {
  return draft_ ? draft_->dark : dark_mode_;
}

void GuideDialog::ApplyShownTheme() {
  themes::Apply(ShownTheme().palette(ShownDark()));
}

// ----------------------------------------------------------------- designer --

// Admins: a new theme, theirs, then everyone else's (to change or remove).
void GuideDialog::MyThemesRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "New Theme", .glyph = Glyph::kAddPerson, .activate = [this] { OpenDesigner(0); }});
  for (const bool mine : {true, false}) {
    for (const ServerTheme& t : backend_->themes()) {
      if ((t.author_xuid == backend_->me().xuid) != mine) continue;
      const themes::Theme theme = FromServer(t);
      rows.push_back({.swatch = theme.palette(dark_mode_).colors[themes::kAccent],
                      .label = t.name,
                      .sub = "by " + t.author + (t.nsfw ? " - NSFW" : ""),
                      .activate = [this, id = t.id] { OpenDesigner(id); }});
    }
  }
}

void GuideDialog::OpenDesigner(int server_id) {
  ThemeDraft draft;
  draft.dark = dark_mode_;
  const ServerTheme* existing = nullptr;
  for (const ServerTheme& t : backend_->themes()) {
    if (t.id == server_id) existing = &t;
  }
  if (existing) {
    draft.theme = FromServer(*existing);
  } else {
    // A new one starts as the theme in use, to change from there.
    draft.theme = theme_;
    draft.theme.key = "draft";
    draft.theme.name = backend_->me().gamertag + "'s Theme";
    draft.theme.author = backend_->me().gamertag;
    draft.theme.server_id = 0;
    draft.theme.updated = 0;
    if (theme_.has_image) {
      // Its picture comes along if it's here already.
      draft.image_file = backend_->ThemeImage(theme_.server_id, theme_.updated);
      draft.image = draft.image_file.empty() ? ThemeDraft::Image::kKept : ThemeDraft::Image::kNew;
    }
    draft.theme.has_image = false;
    draft.dirty = true;
  }
  draft_ = std::move(draft);
  Push({PageKind::kThemeDesigner, "Theme Designer"});
}

bool GuideDialog::DesignerOpen() const {
  for (const Page& p : stacks_[size_t(BladeOf(kSettingsBlade))]) {
    if (p.kind == PageKind::kThemeDesigner) return true;
  }
  return false;
}

bool GuideDialog::LeaveDesigner(std::function<void()> leave) {
  if (!draft_ || !draft_->dirty) {
    draft_.reset();
    return true;
  }
  ShowMessage({"Theme Designer", "Save the changes to " + draft_->theme.name + "?", {"Save", "Discard", "Keep Editing"}, 0,
               [this, leave](int i) {
                 if (i == 0) {
                   SaveDraft([this, leave] {
                     draft_.reset();
                     leave();
                   });
                 } else if (i == 1) {
                   draft_.reset();
                   leave();
                 }
               }});
  return false;
}

void GuideDialog::ThemeDesignerRows(std::vector<Row>& rows, Page&) {
  if (!draft_) return;
  ThemeDraft& d = *draft_;
  themes::Theme& t = d.theme;
  themes::Palette& palette = d.dark ? t.dark : t.light;
  auto changed = [this] { draft_->dirty = true; };

  rows.push_back({.label = "Name", .detail = t.name, .activate = [this] {
                    OpenKeyboard({"Theme Name", "Up to 24 characters.", draft_->theme.name, false, 24,
                                  [this](std::optional<std::string> text) {
                                    if (!draft_ || !text || text->empty()) return;
                                    draft_->theme.name = *text;
                                    draft_->dirty = true;
                                  }});
                  }});
  rows.push_back({.label = "Editing", .detail = d.dark ? "Dark Mode" : "Light Mode",
                  .activate = [this] { draft_->dark = !draft_->dark; }});
  for (int e = 0; e < themes::kElementCount; ++e) {
    rows.push_back({.swatch = palette.colors[e],
                    .label = themes::Info(e).name,
                    .detail = themes::Hex(palette.colors[e]),
                    .activate = [this, e] { Push({PageKind::kColorEditor, themes::Info(e).name, std::to_string(e)}); }});
  }
  rows.push_back({.label = d.dark ? "Copy Light Mode Colours" : "Copy Dark Mode Colours", .activate = [this] {
                    const bool dark = draft_->dark;
                    ShowMessage({"Theme Designer",
                                 std::string("Replace every ") + (dark ? "Dark" : "Light") + " Mode colour with its " +
                                     (dark ? "Light" : "Dark") + " Mode one?",
                                 {"Copy", "Cancel"}, 0, [this, dark](int i) {
                                   if (i != 0 || !draft_) return;
                                   themes::Theme& theme = draft_->theme;
                                   (dark ? theme.dark : theme.light) = dark ? theme.light : theme.dark;
                                   draft_->dirty = true;
                                 }});
                  }});
  rows.push_back({.label = "NSFW", .detail = t.nsfw ? "Yes" : "No", .activate = [this] {
                    draft_->theme.nsfw = !draft_->theme.nsfw;
                    draft_->dirty = true;
                  }});
  rows.push_back({.label = "Background Blur", .detail = Percent(t.backdrop_blur), .slider = t.backdrop_blur,
                  .set_slider = [this, changed](float v) {
                    draft_->theme.backdrop_blur = v;
                    changed();
                  }});
  const bool has_image = d.image == ThemeDraft::Image::kNew || (d.image == ThemeDraft::Image::kKept && t.has_image);
  rows.push_back({.label = "Background Picture", .detail = has_image ? "Change" : "None",
                  .activate = [this] { ChooseThemeImage(); }});
  if (has_image) {
    rows.push_back({.label = "Remove Background Picture", .activate = [this] {
                      draft_->image = ThemeDraft::Image::kRemoved;
                      draft_->image_file.clear();
                      draft_->dirty = true;
                    }});
    rows.push_back({.label = "Picture Opacity", .detail = Percent(t.image_alpha), .slider = t.image_alpha,
                    .set_slider = [this, changed](float v) {
                      draft_->theme.image_alpha = v;
                      changed();
                    }});
    rows.push_back({.label = "Picture Blur", .detail = Percent(t.image_blur), .slider = t.image_blur,
                    .set_slider = [this, changed](float v) {
                      draft_->theme.image_blur = v;
                      changed();
                    }});
  }
  rows.push_back({.label = "Save Theme", .detail = d.dirty ? "Unsaved" : "", .glyph = Glyph::kCheck,
                  .activate = [this] { SaveDraft(nullptr); }});
  if (t.server_id) {
    rows.push_back({.label = "Delete Theme", .activate = [this] {
                      ShowMessage({"Delete Theme", "Delete " + draft_->theme.name + " for everyone using it?", {"Delete", "Cancel"},
                                   0, [this](int i) {
                                     if (i != 0 || !draft_) return;
                                     const int id = draft_->theme.server_id;
                                     Busy("Deleting theme...");
                                     backend_->DeleteTheme(id, [this, id](const Result& r) {
                                       busy_.reset();
                                       if (!r.ok) return ShowMessage({"Delete Theme", r.error, {"OK"}});
                                       if (theme_.server_id == id) SelectTheme(themes::Default());
                                       draft_.reset();
                                       while (Stack().size() > 1 && Current().kind != PageKind::kPreferences &&
                                              Current().kind != PageKind::kMyThemes) {
                                         Stack().pop_back();
                                       }
                                     });
                                   }});
                    }});
  }
}

void GuideDialog::ColorEditorRows(std::vector<Row>& rows, Page& page) {
  if (!draft_) return;
  const int e = std::clamp(std::atoi(page.arg.c_str()), 0, int(themes::kElementCount) - 1);
  auto palette = [this] { return &(draft_->dark ? draft_->theme.dark : draft_->theme.light); };
  const ImU32 color = palette()->colors[e];
  struct Channel {
    const char* name;
    int shift;
  };
  static constexpr Channel kChannels[] = {{"Red", IM_COL32_R_SHIFT},
                                          {"Green", IM_COL32_G_SHIFT},
                                          {"Blue", IM_COL32_B_SHIFT},
                                          {"Opacity", IM_COL32_A_SHIFT}};
  for (const Channel& c : kChannels) {
    const float value = ChannelOf(color, c.shift) / 255.0f;
    rows.push_back({.label = c.name,
                    .detail = c.shift == IM_COL32_A_SHIFT ? Percent(value) : std::to_string(ChannelOf(color, c.shift)),
                    .slider = value,
                    .set_slider = [this, palette, e, shift = c.shift](float v) {
                      ImU32& target = palette()->colors[e];
                      target = WithChannel(target, shift, v);
                      draft_->dirty = true;
                    },
                    .slider_step = 5.0f / 255.0f});
  }
  rows.push_back({.label = "Hex Code", .detail = themes::Hex(color), .activate = [this, palette, e] {
                    OpenKeyboard({"Hex Code", "#RRGGBB, or #RRGGBBAA with opacity.", themes::Hex(palette()->colors[e]), false, 9,
                                  [this, palette, e](std::optional<std::string> text) {
                                    if (!draft_ || !text) return;
                                    if (!themes::ParseHex(*text, &palette()->colors[e])) {
                                      return ShowMessage({"Hex Code", "That isn't a colour like #6CB040.", {"OK"}});
                                    }
                                    draft_->dirty = true;
                                  }});
                  }});
  rows.push_back({.label = "Reset to Default", .activate = [this, palette, e] {
                    palette()->colors[e] = themes::Default().palette(draft_->dark).colors[e];
                    draft_->dirty = true;
                  }});
}

void GuideDialog::SaveDraft(std::function<void()> then) {
  if (!draft_) return;
  Busy("Saving theme...");
  const themes::Theme theme = draft_->theme;
  backend_->SaveTheme(theme.server_id, theme.name, themes::DataJson(theme), theme.nsfw, [this, then](const Result& r, int id) {
    if (!r.ok || !draft_) {
      busy_.reset();
      if (!r.ok) ShowMessage({"Save Theme", r.error, {"OK"}});
      return;
    }
    draft_->theme.server_id = id;
    auto finish = [this, id, then](const Result& r) {
      busy_.reset();
      if (!r.ok) return ShowMessage({"Save Theme", r.error, {"OK"}});
      if (!draft_) return;
      // Saved: it's the theme in use now, and what the designer goes on with.
      for (const ServerTheme& t : backend_->themes()) {
        if (t.id != id) continue;
        SelectTheme(FromServer(t));
        draft_->theme = theme_;
      }
      draft_->image = ThemeDraft::Image::kKept;
      draft_->image_file.clear();
      draft_->dirty = false;
      ShowToast({.glyph = Glyph::kCheck, .line1 = "Theme saved", .line2 = theme_.name});
      if (then) then();
    };
    switch (draft_->image) {
      case ThemeDraft::Image::kNew: backend_->SetThemeImage(id, draft_->image_file, finish); break;
      case ThemeDraft::Image::kRemoved: backend_->SetThemeImage(id, "", finish); break;
      default: finish({}); break;
    }
  });
}

void GuideDialog::ChooseThemeImage() {
  file_picker::ChoosePicture(OnImageChosen);
}

void GuideDialog::PumpThemeImagePicks() {
  std::vector<ImagePick> picks;
  {
    std::lock_guard lock(g_image_mutex);
    picks.swap(g_image_picks);
  }
  for (ImagePick& pick : picks) {
    if (pick.jpg.empty()) {
      ShowMessage({"Background Picture", pick.error, {"OK"}});
    } else if (draft_) {
      draft_->image_file = std::move(pick.jpg);
      draft_->image = ThemeDraft::Image::kNew;
      ++draft_->image_serial;
      draft_->dirty = true;
    }
  }
}

}  // namespace rexglue_guide
