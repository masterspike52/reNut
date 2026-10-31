// App menus (AddMenu): their rows, from the app's build functions, on a blade
// of the Guide's or in a window of their own over the game.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio.h"
#include "guide_dialog.h"
#include "imgui.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

std::mutex g_menus_mutex;
std::vector<std::shared_ptr<const MenuDef>> g_menus;  // in the order added
// ShowMenu's requests for the drawing thread: an id, and 1 show, 0 hide, -1 toggle.
std::vector<std::pair<std::string, int>> g_menu_requests;
// Menus shown as of the last frame, for MenuShown.
std::set<std::string, std::less<>> g_menus_shown;

// Choices with more options than this get a search row at the top of their list.
constexpr size_t kSearchFrom = 12;

// A label's words: what's before any "##".
std::string Shown(std::string_view label) {
  return std::string(label.substr(0, label.find("##")));
}

bool Matches(const std::string& text, const std::string& search) {
  if (search.empty()) return true;
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  return lower(text).find(lower(search)) != std::string::npos;
}

}  // namespace

std::vector<std::shared_ptr<const MenuDef>> AppMenus() {
  std::lock_guard lock(g_menus_mutex);
  return g_menus;
}

std::shared_ptr<const MenuDef> FindAppMenu(std::string_view id) {
  std::lock_guard lock(g_menus_mutex);
  for (const auto& menu : g_menus) {
    if (menu->id == id) return menu;
  }
  return nullptr;
}

void AddMenu(MenuDef menu) {
  std::lock_guard lock(g_menus_mutex);
  for (auto& m : g_menus) {
    if (m->id == menu.id) {
      m = std::make_shared<const MenuDef>(std::move(menu));
      return;
    }
  }
  g_menus.push_back(std::make_shared<const MenuDef>(std::move(menu)));
}

void ShowMenu(std::string_view id, bool show) {
  std::lock_guard lock(g_menus_mutex);
  g_menu_requests.push_back({std::string(id), show ? 1 : 0});
}

void ToggleMenu(std::string_view id) {
  std::lock_guard lock(g_menus_mutex);
  g_menu_requests.push_back({std::string(id), -1});
}

bool MenuShown(std::string_view id) {
  std::lock_guard lock(g_menus_mutex);
  return g_menus_shown.find(id) != g_menus_shown.end();
}

// The build function walks the menu's whole tree each frame; only the page
// shown (`path`, from the first) gets rows. Rows are told apart by their
// labels, numbered when one is used twice on a page.
struct Menu::State {
  std::vector<Row>* rows = nullptr;
  std::map<std::string, MenuEvent>* events = nullptr;
  std::map<std::string, std::string>* search = nullptr;
  std::function<void(rexglue_guide::Page)> push;
  std::function<void()> pop;
  std::function<void(KeyboardRequest)> keyboard;
  std::vector<std::string> path;  // page ids to the one shown; the last can be a choice's list
  size_t depth = 0;               // pages in, where the build function is
  std::string key;                // that page's arg
  std::vector<std::map<std::string, int>> seen;  // per depth: labels used so far
  bool found = false;  // the page shown is still there
  bool back = false;
  bool detached = false;

  bool Here() const { return depth == path.size(); }
  std::string Id(std::string_view label) {
    const std::string text(label);
    const int n = seen.back()[text]++;
    return n ? text + "#" + std::to_string(n) : text;
  }
  std::string Key(const std::string& id) const { return key + kMenuRow + id; }
  MenuEvent Take(const std::string& id) {
    const auto it = events->find(Key(id));
    if (it == events->end()) return {};
    MenuEvent e = std::move(it->second);
    events->erase(it);
    return e;
  }
  // Adds to what a row has done since the build function last heard.
  static void Post(std::map<std::string, MenuEvent>* events, const std::string& key, MenuEvent e) {
    MenuEvent& to = (*events)[key];
    to.chosen |= e.chosen;
    to.step += e.step;
    if (e.index) to.index = e.index;
    if (e.number) to.number = e.number;
    if (e.text) to.text = std::move(e.text);
  }
  std::function<void()> Poster(const std::string& id, MenuEvent e) const {
    return [events = events, key = Key(id), e] { Post(events, key, e); };
  }
  std::function<void(int)> Stepper(const std::string& id) const {
    return [events = events, key = Key(id)](int step) { Post(events, key, {.step = step}); };
  }
};

bool Menu::Button(std::string_view label, std::string_view detail) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) return false;
  const bool chosen = s.Take(id).chosen;
  s.rows->push_back({.label = Shown(label), .detail = std::string(detail), .activate = s.Poster(id, {.chosen = true})});
  return chosen;
}

bool Menu::Toggle(std::string_view label, bool* value) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) return false;
  const bool changed = s.Take(id).chosen;
  if (changed) *value = !*value;
  Row row{.label = Shown(label), .activate = s.Poster(id, {.chosen = true})};
  row.toggle = *value;
  s.rows->push_back(std::move(row));
  return changed;
}

bool Menu::Choice(std::string_view label, int* index, const std::vector<std::string>& options) {
  State& s = *state_;
  const std::string id = s.Id(label);
  const std::string list = std::string(1, kMenuList) + id;
  const std::string list_arg = s.key + kMenuPath + list;
  if (s.depth + 1 == s.path.size() && s.path.back() == list) {
    // Its list is the page shown: the options, each going back with it chosen.
    s.found = true;
    const std::string& search = (*s.search)[list_arg];
    if (options.size() > kSearchFrom) {
      s.rows->push_back({.label = "Search", .sub = search, .activate = [s_keyboard = s.keyboard, search_map = s.search, list_arg, search] {
                           s_keyboard({"Search", "", search, false, 32, [search_map, list_arg](std::optional<std::string> text) {
                                         if (text) (*search_map)[list_arg] = *text;
                                       }});
                         }});
    }
    for (size_t i = 0; i < options.size(); ++i) {
      if (!Matches(options[i], search)) continue;
      Row row{.label = options[i]};
      if (int(i) == *index) row.glyph = Glyph::kCheck;
      row.activate = [post = s.Poster(id, {.index = int(i)}), pop = s.pop] {
        post();
        pop();
      };
      s.rows->push_back(std::move(row));
    }
    return false;
  }
  if (!s.Here()) return false;
  const MenuEvent e = s.Take(id);
  const int was = *index;
  const int n = int(options.size());
  if (e.index) *index = *e.index;
  if (e.step && n > 0) *index = ((std::clamp(*index, 0, n - 1) + e.step) % n + n) % n;
  const bool valid = *index >= 0 && *index < n;
  Row row{.label = Shown(label), .detail = valid ? options[size_t(*index)] : std::string()};
  row.step = s.Stepper(id);
  // The list opens on the one chosen (below its search row).
  const int selected = std::max(0, *index) + (options.size() > kSearchFrom ? 1 : 0);
  row.activate = [push = s.push, search_map = s.search, list_arg, title = Shown(label), selected] {
    search_map->erase(list_arg);
    rexglue_guide::Page page{PageKind::kMenu, title, list_arg};
    page.selected = selected;
    push(std::move(page));
  };
  s.rows->push_back(std::move(row));
  return *index != was;
}

bool Menu::Number(std::string_view label, int* value, int min, int max, int step) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) return false;
  const MenuEvent e = s.Take(id);
  const int was = *value;
  if (e.number) *value = int(std::clamp<double>(std::round(*e.number), min, max));
  if (e.step) *value = int(std::clamp<int64_t>(int64_t(*value) + int64_t(e.step) * step, min, max));
  Row row{.label = Shown(label), .detail = std::to_string(*value)};
  row.step = s.Stepper(id);
  row.activate = [keyboard = s.keyboard, events = s.events, key = s.Key(id), title = Shown(label), current = *value] {
    keyboard({title, "", std::to_string(current), false, 12, [events, key](std::optional<std::string> text) {
                if (!text) return;
                char* end = nullptr;
                const double number = std::strtod(text->c_str(), &end);
                if (end != text->c_str()) State::Post(events, key, {.number = number});
              }});
  };
  s.rows->push_back(std::move(row));
  return *value != was;
}

bool Menu::Slider(std::string_view label, float* value, float min, float max, float step, const char* format) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) return false;
  const MenuEvent e = s.Take(id);
  const float was = *value;
  if (e.number) {
    float v = std::clamp(float(*e.number), min, max);
    if (step > 0.0f) v = std::clamp(min + std::round((v - min) / step) * step, min, max);
    *value = v;
  }
  char shown[64] = {};
  std::snprintf(shown, sizeof(shown), format ? format : "%.2f", double(*value));
  Row row{.label = Shown(label), .detail = shown};
  const float range = max - min;
  row.slider = range > 0.0f ? std::clamp((*value - min) / range, 0.0f, 1.0f) : 0.0f;
  row.slider_step = range > 0.0f && step > 0.0f ? step / range : 0.05f;
  row.set_slider = [events = s.events, key = s.Key(id), min, range](float t) {
    State::Post(events, key, {.number = double(min + t * range)});
  };
  s.rows->push_back(std::move(row));
  return *value != was;
}

bool Menu::TextField(std::string_view label, std::string* text, size_t max_length) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) return false;
  MenuEvent e = s.Take(id);
  const bool changed = e.text && *e.text != *text;
  if (e.text) *text = std::move(*e.text);
  Row row{.label = Shown(label), .sub = text->empty() ? "None" : *text};
  row.activate = [keyboard = s.keyboard, events = s.events, key = s.Key(id), title = Shown(label), current = *text,
                  max_length] {
    keyboard({title, "", current, false, max_length, [events, key](std::optional<std::string> typed) {
                if (typed) State::Post(events, key, {.text = std::move(*typed)});
              }});
  };
  s.rows->push_back(std::move(row));
  return changed;
}

void Menu::Page(std::string_view label, const std::function<void(Menu&)>& build, std::string_view detail) {
  State& s = *state_;
  const std::string id = s.Id(label);
  if (!s.Here()) {
    // A page on the way to the one shown: in, for its build function to go on.
    if (s.path[s.depth] != id) return;
    const std::string outer = s.key;
    s.key += kMenuPath + id;
    ++s.depth;
    s.seen.emplace_back();
    if (s.Here()) s.found = true;
    build(*this);
    s.seen.pop_back();
    --s.depth;
    s.key = outer;
    return;
  }
  s.rows->push_back({.label = Shown(label), .detail = std::string(detail),
                     .activate = [push = s.push, arg = s.key + kMenuPath + id, title = Shown(label)] {
                       push({PageKind::kMenu, title, arg});
                     }});
}

void Menu::Header(std::string_view text) {
  State& s = *state_;
  if (!s.Here()) return;
  s.rows->push_back({.label = std::string(text), .header = true});
}

void Menu::Text(std::string_view text, Tone tone) {
  State& s = *state_;
  if (!s.Here() || text.empty()) return;
  Row row{.text = std::string(text)};
  if (tone == Tone::kNormal) row.text_color = color::kText;
  if (tone == Tone::kError) row.text_color = IM_COL32(255, 120, 110, 255);
  s.rows->push_back(std::move(row));
}

void Menu::Image(rex::ui::ImmediateTexture* texture, float aspect, float max_height) {
  State& s = *state_;
  if (!s.Here()) return;
  s.rows->push_back({.picture = reinterpret_cast<ImTextureID>(texture),
                     .picture_aspect = aspect > 0.0f ? aspect : 1.0f,
                     .picture_height = max_height});
}

void Menu::Back() {
  state_->back = true;
}

bool Menu::detached() const {
  return state_->detached;
}

void GuideDialog::MenuRows(std::vector<Row>& rows, Page& page, std::function<void(Page)> push,
                           std::function<void()> pop, bool detached) {
  // The arg: the menu's id, then the pages in.
  std::vector<std::string> parts;
  for (size_t from = 0;;) {
    const size_t to = page.arg.find(kMenuPath, from);
    parts.push_back(page.arg.substr(from, to == std::string::npos ? std::string::npos : to - from));
    if (to == std::string::npos) break;
    from = to + 1;
  }
  const auto menu = FindAppMenu(parts.front());
  if (!menu || !menu->build) return;

  Menu::State s;
  s.rows = &rows;
  s.events = &menu_events_;
  s.search = &menu_search_;
  s.push = std::move(push);
  s.pop = pop;
  s.keyboard = [this](KeyboardRequest request) { OpenKeyboard(std::move(request)); };
  s.path.assign(parts.begin() + 1, parts.end());
  s.key = menu->id;
  s.seen.emplace_back();
  s.found = s.path.empty();
  s.detached = detached;
  Menu builder(&s);
  menu->build(builder);

  // What its rows did has been heard; any left were for rows gone since.
  for (auto it = menu_events_.begin(); it != menu_events_.end();) {
    const std::string& key = it->first;
    const bool mine = key.size() > menu->id.size() && key.compare(0, menu->id.size(), menu->id) == 0 &&
                      (key[menu->id.size()] == kMenuRow || key[menu->id.size()] == kMenuPath);
    it = mine ? menu_events_.erase(it) : std::next(it);
  }
  // Its page has gone (the build function stopped making it), or it asked to go back.
  if (!s.found || s.back) pop();
}

void GuideDialog::PumpMenus() {
  std::vector<std::pair<std::string, int>> requests;
  {
    std::lock_guard lock(g_menus_mutex);
    requests.swap(g_menu_requests);
  }
  for (const auto& [id, want] : requests) {
    const auto menu = FindAppMenu(id);
    if (!menu) continue;
    if (menu->place == MenuPlace::kDetached) {
      auto it = std::find_if(windows_.begin(), windows_.end(), [&](const MenuWindowState& w) { return w.id == id; });
      const bool open = it != windows_.end() && !it->closing;
      const bool show = want < 0 ? !open : want > 0;
      if (show && !open) {
        if (it != windows_.end()) {
          // Closing still: back it comes, on top.
          MenuWindowState w = std::move(*it);
          windows_.erase(it);
          w.closing = false;
          windows_.push_back(std::move(w));
        } else {
          MenuWindowState w;
          w.id = id;
          w.stack = {Page{PageKind::kMenu, menu->title, id}};
          windows_.push_back(std::move(w));
        }
        audio::Play(Sound::kOpen);
      } else if (!show && open) {
        it->closing = true;
        audio::Play(Sound::kClose);
      }
      continue;
    }
    const bool there = screen_ == Screen::kGuide && !closing_ && size_t(blade_) < blades_.size() &&
                       blades_[size_t(blade_)].menu == id;
    const bool show = want < 0 ? !there : want > 0;
    if (show && !there) {
      if (screen_ != Screen::kGuide || closing_) OpenGuide();
      auto find = [&] {
        for (size_t b = 0; b < blades_.size(); ++b) {
          if (blades_[b].menu == id) return int(b);
        }
        return -1;
      };
      int b = find();
      if (b < 0) {
        LayoutBlades();  // added since the Guide opened
        b = find();
      }
      if (b >= 0) {
        blade_ = b;
        stacks_[size_t(b)] = {RootPage(b)};
        slide_ = 0.0f;
      }
    } else if (!show && there) {
      Close();
    }
  }

  std::lock_guard lock(g_menus_mutex);
  g_menus_shown.clear();
  for (const MenuWindowState& w : windows_) {
    if (!w.closing) g_menus_shown.insert(w.id);
  }
  if (screen_ == Screen::kGuide && !closing_ && size_t(blade_) < blades_.size() && !blades_[size_t(blade_)].menu.empty()) {
    g_menus_shown.insert(blades_[size_t(blade_)].menu);
  }
}

ImTextureID GuideDialog::MenuIcon(const std::string& id) {
  const auto menu = FindAppMenu(id);
  if (!menu || menu->icon.empty()) return 0;
  const std::span<const uint8_t> file(reinterpret_cast<const uint8_t*>(menu->icon.data()), menu->icon.size());
  return textures_.Picture("menu:" + id, file, false, 128);
}

void GuideDialog::DrawMenuMark(ImDrawList* dl, const std::string& id, ImVec2 c, float a) {
  if (const ImTextureID icon = MenuIcon(id)) {
    Image(dl, icon, ImVec2(c.x - Px(20), c.y - Px(20)), ImVec2(c.x + Px(20), c.y + Px(20)), WithAlpha(color::kText, a));
    return;
  }
  // No icon: the title's first letter (a whole UTF-8 character).
  const auto menu = FindAppMenu(id);
  if (!menu || menu->title.empty()) return;
  size_t n = 1;
  while (n < menu->title.size() && (static_cast<unsigned char>(menu->title[n]) & 0xC0) == 0x80) ++n;
  const std::string letter = menu->title.substr(0, n);
  const ImVec2 s = Measure(Semibold(), Px(38), letter);
  Text(dl, Semibold(), Px(38), ImVec2(std::floor(c.x - s.x * 0.5f), std::floor(c.y - s.y * 0.5f)), WithAlpha(color::kText, a),
       letter);
}

void GuideDialog::DrawMenuWindows(ImDrawList* dl, const NavInput& in, float dt) {
  // Opening and closing.
  for (MenuWindowState& w : windows_) {
    w.open_t = w.closing ? std::max(0.0f, w.open_t - dt / 0.14f) : std::min(1.0f, w.open_t + dt / 0.18f);
  }
  std::erase_if(windows_, [](const MenuWindowState& w) { return w.closing && w.open_t <= 0.0f; });
  std::erase_if(windows_, [](const MenuWindowState& w) { return !FindAppMenu(w.id); });

  // Where each goes, the one the mouse is over (the topmost), and the one the
  // pad works (the topmost holding input).
  struct Placed {
    std::shared_ptr<const MenuDef> menu;
    ImVec2 p0, p1;
  };
  std::vector<Placed> placed;
  int hovered = -1, pad = -1;
  const ImGuiIO& io = ImGui::GetIO();
  for (size_t i = 0; i < windows_.size(); ++i) {
    MenuWindowState& w = windows_[i];
    const auto menu = FindAppMenu(w.id);
    const MenuWindow& def = menu->window;
    const ImVec2 size(std::min(Px(def.width), display_.x), std::min(Px(def.height), display_.y));
    const int col = int(def.anchor) % 3, row = int(def.anchor) / 3;
    const float margin = Px(40);
    ImVec2 p0(col == 0 ? margin : col == 1 ? (display_.x - size.x) * 0.5f : display_.x - size.x - margin,
              row == 0 ? margin : row == 1 ? (display_.y - size.y) * 0.5f : display_.y - size.y - margin);
    p0.x += Px(def.x) + w.moved.x;
    p0.y += Px(def.y) + w.moved.y;
    // Always on the screen, however it was dragged.
    p0.x = std::floor(std::clamp(p0.x, 0.0f, std::max(0.0f, display_.x - size.x)));
    p0.y = std::floor(std::clamp(p0.y, 0.0f, std::max(0.0f, display_.y - size.y)));
    placed.push_back({menu, p0, ImVec2(p0.x + size.x, p0.y + size.y)});
    if (w.closing) continue;
    if (ImGui::IsMouseHoveringRect(placed.back().p0, placed.back().p1, false)) hovered = int(i);
    if (def.hold_input) pad = int(i);
  }
  window_holds_input_ = pad >= 0;

  for (size_t i = 0; i < windows_.size(); ++i) {
    MenuWindowState& w = windows_[i];
    const MenuDef& def = *placed[i].menu;
    const MenuWindow& layout = def.window;
    const ImVec2 p0 = placed[i].p0, p1 = placed[i].p1;
    const float a = Ease(w.open_t);
    const float round = Px(12);
    const std::string id = w.id;
    w.p0 = p0;
    w.p1 = p1;
    // Under the mouse, it keeps it from the game and the app's windows.
    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(ImVec2(p1.x - p0.x, p1.y - p0.y));
    ImGui::Begin(("##rexglue_menu_" + id).c_str(), nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();

    // Its parts: a strip down the left with the mark and title, or a title
    // across the top; the close button, and back on a page past the first.
    const bool strip = layout.strip;
    const float bar = strip ? 0.0f : Px(62);
    const ImVec2 s0(p0.x + Px(9), p0.y + Px(9)), s1(p0.x + Px(81), p1.y - Px(9));
    const ImVec2 close_c(p1.x - Px(32), p0.y + Px(31));
    const ImVec2 back_c(strip ? s1.x + Px(30) : p0.x + Px(32), p0.y + Px(31));
    const bool can_back = w.stack.size() > 1;
    const ImVec2 l0(strip ? s1.x + Px(12) : p0.x + Px(10), p0.y + (strip ? Px(60) : bar + Px(2)));
    const ImVec2 l1(p1.x - Px(12), p1.y - Px(12));
    const ImVec2 title0(strip ? s0 : p0), title1(strip ? s1 : ImVec2(p1.x, p0.y + bar));

    auto push = [this, id](Page page) {
      for (MenuWindowState& o : windows_) {
        if (o.id != id) continue;
        o.stack.push_back(std::move(page));
        o.slide = 1.0f;
      }
    };
    auto pop = [this, id] {
      for (MenuWindowState& o : windows_) {
        if (o.id != id || o.closing) continue;
        if (o.stack.size() > 1) {
          o.stack.pop_back();
          o.slide = -1.0f;
          audio::Play(Sound::kBack);
        } else {
          o.closing = true;
          audio::Play(Sound::kClose);
        }
      }
    };

    if (!w.closing) {
      // The mouse: the title bar's buttons and dragging, then the rows.
      const bool mouse = int(i) == hovered;
      const auto over = [&](ImVec2 c) {
        return ImGui::IsMouseHoveringRect(ImVec2(c.x - Px(20), c.y - Px(20)), ImVec2(c.x + Px(20), c.y + Px(20)), false);
      };
      bool used = false;
      if (mouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (over(close_c)) {
          w.closing = true;
          audio::Play(Sound::kClose);
          used = true;
        } else if (can_back && over(back_c)) {
          pop();
          used = true;
        } else if (layout.movable && ImGui::IsMouseHoveringRect(title0, title1, false)) {
          w.dragging = true;
          used = true;
        }
      }
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) w.dragging = false;
      if (w.dragging) {
        w.moved.x += io.MouseDelta.x;
        w.moved.y += io.MouseDelta.y;
        used = true;
      }
      if (!used && !w.stack.empty()) {
        const bool drags = w.list.dragging_slider >= 0 || w.list.dragging_scroll;
        const ListInput result = HandleList(int(i) == pad ? in : NavInput{}, w.list, w.stack.back(), mouse || drags);
        if (result.back) pop();
      }
    }
    if (w.stack.empty()) continue;

    // The rows, from the menu's build function.
    w.slide += (0.0f - w.slide) * std::min(1.0f, dt * 14.0f);
    if (std::abs(w.slide) < 0.002f) w.slide = 0.0f;
    w.list.rows.clear();
    if (!w.closing) MenuRows(w.list.rows, w.stack.back(), push, pop, true);
    MenuWindowState& drawn = windows_[i];  // (still: the build function's requests wait for PumpMenus)
    if (drawn.stack.empty()) continue;

    Shadow(dl, p0, p1, round, Px(24), a);
    dl->AddRectFilled(p0, p1, WithAlpha(color::kBlade, a), round);
    DrawThemeBackground(dl, p0, p1, round, a);
    if (strip) {
      dl->AddRectFilled(s0, s1, WithAlpha(color::kBladeStrip, a), Px(8));
      const ImVec2 mark((s0.x + s1.x) * 0.5f, s0.y + Px(38));
      DrawMenuMark(dl, id, mark, a);
      TextVertical(dl, Regular(), Px(30), ImVec2(mark.x, (mark.y + Px(40) + s1.y) * 0.5f), WithAlpha(color::kText, a),
                   def.title);
    } else {
      const float tx = p0.x + (can_back ? Px(60) : Px(24));
      TextFit(dl, Light(), Px(30), ImVec2(tx, p0.y + Px(12)), WithAlpha(color::kText, a), def.title,
              close_c.x - Px(30) - tx);
      dl->AddLine(ImVec2(p0.x + Px(18), p0.y + bar), ImVec2(p1.x - Px(18), p0.y + bar),
                  WithAlpha(IM_COL32(255, 255, 255, 30), a), Px(1));
    }
    // Close (x) and back (<), lit under the mouse.
    const bool mouse_here = int(i) == hovered;
    auto lit = [&](ImVec2 c) {
      return mouse_here && ImGui::IsMouseHoveringRect(ImVec2(c.x - Px(20), c.y - Px(20)), ImVec2(c.x + Px(20), c.y + Px(20)), false);
    };
    const float k = Px(8);
    if (lit(close_c)) dl->AddCircleFilled(close_c, Px(18), WithAlpha(color::kGreen, a), 24);
    dl->AddLine(ImVec2(close_c.x - k, close_c.y - k), ImVec2(close_c.x + k, close_c.y + k), WithAlpha(color::kText, a), Px(2.5f));
    dl->AddLine(ImVec2(close_c.x - k, close_c.y + k), ImVec2(close_c.x + k, close_c.y - k), WithAlpha(color::kText, a), Px(2.5f));
    if (can_back) {
      if (lit(back_c)) dl->AddCircleFilled(back_c, Px(18), WithAlpha(color::kGreen, a), 24);
      dl->PathLineTo(ImVec2(back_c.x + k * 0.5f, back_c.y - k));
      dl->PathLineTo(ImVec2(back_c.x - k * 0.5f, back_c.y));
      dl->PathLineTo(ImVec2(back_c.x + k * 0.5f, back_c.y + k));
      dl->PathStroke(WithAlpha(color::kText, a), 0, Px(2.5f));
    }
    DrawList(dl, drawn.list, drawn.stack, l0, l1, a, drawn.slide);
  }
}

}  // namespace rexglue_guide
