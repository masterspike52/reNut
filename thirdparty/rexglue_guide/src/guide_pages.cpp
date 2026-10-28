// Rows of the blades' own pages and the small settings pages.

#include <mutex>
#include <string>
#include <vector>

#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// The Guide's own credits, always listed after the app's: its icons, gamerpics
// and sounds come from DashX360.
const Credit kGuideCredits[] = {
    {"ZivvoZ", "DashX360 1.1 icons, gamerpics and Guide sounds"},
};

}  // namespace

std::vector<Row> GuideDialog::BuildRows(Page& page) {
  std::vector<Row> rows;
  switch (page.kind) {
    case PageKind::kHome: HomeRows(rows, page); break;
    case PageKind::kSocial: SocialRows(rows, page); break;
    case PageKind::kGames: GamesRows(rows, page); break;
    case PageKind::kSettings: SettingsRows(rows, page); break;
    case PageKind::kFriends: FriendsRows(rows, page); break;
    case PageKind::kRequests: RequestsRows(rows, page); break;
    case PageKind::kNotifications: NotificationsRows(rows, page); break;
    case PageKind::kAchievements: AchievementsRows(rows, page); break;
    case PageKind::kLeaderboards: LeaderboardsRows(rows, page); break;
    case PageKind::kLeaderboard: LeaderboardRows(rows, page); break;
    case PageKind::kGamercard: GamercardRows(rows, page); break;
    case PageKind::kPreferences: PreferencesRows(rows, page); break;
    case PageKind::kThemes: break;  // drawn as a gallery
    case PageKind::kMessages: MessagesRows(rows, page); break;
    case PageKind::kConversation: ConversationRows(rows, page); break;
    case PageKind::kNewMessage: NewMessageRows(rows, page); break;
    case PageKind::kSystem: SystemRows(rows, page); break;
    case PageKind::kCredits: CreditsRows(rows, page); break;
    case PageKind::kAccount: AccountRows(rows, page); break;
    case PageKind::kSignInHome: SignInHomeRows(rows, page); break;
    case PageKind::kThemeDesigner: ThemeDesignerRows(rows, page); break;
    case PageKind::kColorEditor: ColorEditorRows(rows, page); break;
    case PageKind::kMyThemes: MyThemesRows(rows, page); break;
    case PageKind::kGamerpics: break;  // drawn as a grid
  }
  return rows;
}

Row GuideDialog::NeedSignIn(Row row) const {
  if (!backend_->signed_in()) {
    row.disabled = true;
    row.detail = "SIGN IN";
    row.activate = nullptr;
  }
  return row;
}

void GuideDialog::HomeRows(std::vector<Row>& rows, Page&) {
  const bool in = backend_->signed_in();
  if (!in) {
    rows.push_back({.label = "Sign In", .glyph = Glyph::kAddPerson, .activate = [this] { OpenSignIn(); }});
  }
  int online = 0;
  Row friends{.label = "Friends", .activate = [this] { Push({PageKind::kFriends, "Friends"}); }};
  for (const Friend& f : backend_->friends()) {
    if (!f.online) continue;
    ++online;
    if (friends.avatars.size() < 3) friends.avatars.push_back(f.profile.gamerpic);
  }
  friends.detail = std::to_string(online) + " online";
  rows.push_back(NeedSignIn(std::move(friends)));
  int unread = 0;
  for (const auto& n : backend_->notifications()) unread += n.unread;
  rows.push_back(NeedSignIn({.label = "Notifications",
                             .detail = unread ? std::to_string(unread) + " new" : "",
                             .glyph = Glyph::kBell,
                             .unread = unread > 0,
                             .activate = [this] { Push({PageKind::kNotifications, "Notifications"}); }}));
  const int unread_messages = in ? UnreadMessages() : 0;
  rows.push_back(NeedSignIn({.label = "Messages",
                             .detail = unread_messages ? std::to_string(unread_messages) + " new" : "",
                             .glyph = Glyph::kInvite,
                             .unread = unread_messages > 0,
                             .activate = [this] { Push({PageKind::kMessages, "Messages"}); }}));
  rows.push_back({.label = "Achievements",
                  .detail = AchievementSummary(),
                  .glyph = Glyph::kTrophy,
                  .activate = [this] { Push({PageKind::kAchievements, config_.title_name}); }});
  if (in) {
    rows.push_back({.label = "My Profile",
                    .detail = std::to_string(backend_->me().gamerscore) + " G",
                    .avatars = {backend_->me().gamerpic},
                    .activate = [this] { Push({PageKind::kGamercard, "Profile", backend_->me().gamertag}); }});
  }
}

void GuideDialog::SocialRows(std::vector<Row>& rows, Page&) {
  const bool in = backend_->signed_in();
  int online = 0;
  for (const Friend& f : backend_->friends()) online += f.online;
  rows.push_back(NeedSignIn({.label = "Friends",
                             .detail = std::to_string(online) + " online",
                             .glyph = Glyph::kPeople,
                             .activate = [this] { Push({PageKind::kFriends, "Friends"}); }}));
  const size_t requests = backend_->friend_requests().size();
  rows.push_back(NeedSignIn({.label = "Friend Requests",
                             .detail = requests ? std::to_string(requests) + " pending" : "",
                             .glyph = Glyph::kInvite,
                             .unread = requests > 0,
                             .activate = [this] { Push({PageKind::kRequests, "Friend Requests"}); }}));
  const int unread_messages = in ? UnreadMessages() : 0;
  rows.push_back(NeedSignIn({.label = "Messages",
                             .detail = unread_messages ? std::to_string(unread_messages) + " new" : "",
                             .glyph = Glyph::kInvite,
                             .unread = unread_messages > 0,
                             .activate = [this] { Push({PageKind::kMessages, "Messages"}); }}));
  rows.push_back(NeedSignIn({.label = "Add Friend", .glyph = Glyph::kAddPerson, .activate = [this] { AddFriend(); }}));
}

void GuideDialog::GamesRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "Achievements",
                  .detail = AchievementSummary(),
                  .glyph = Glyph::kTrophy,
                  .activate = [this] { Push({PageKind::kAchievements, config_.title_name}); }});
  rows.push_back(NeedSignIn({.label = "Leaderboards",
                             .glyph = Glyph::kPeople,
                             .activate = [this] { Push({PageKind::kLeaderboards, "Leaderboards"}); }}));
}

void GuideDialog::SettingsRows(std::vector<Row>& rows, Page&) {
  rows.push_back(NeedSignIn({.label = "Profile",
                             .icon = textures_.Icon(assets::Icon::kAccount),
                             .activate = [this] { Push({PageKind::kGamercard, "Profile", backend_->me().gamertag}); }}));
  rows.push_back({.label = "Preferences",
                  .icon = textures_.Icon(assets::Icon::kPreferences),
                  .activate = [this] { Push({PageKind::kPreferences, "Preferences"}); }});
  rows.push_back({.label = "System Settings",
                  .icon = textures_.Icon(assets::Icon::kSystem),
                  .activate = [this] { Push({PageKind::kSystem, "System Settings"}); }});
  rows.push_back(NeedSignIn({.label = "Account Management",
                             .icon = textures_.Icon(assets::Icon::kPrivacy),
                             .activate = [this] { Push({PageKind::kAccount, "Account Management"}); }}));
  rows.push_back({.label = "Credits", .icon = textures_.Icon(assets::Icon::kFriends),
                  .activate = [this] { Push({PageKind::kCredits, "Credits"}); }});
  if (config_.on_quit) {
    rows.push_back({.label = "Quit Game", .icon = textures_.Icon(assets::Icon::kTurnOff), .activate = [this] {
                      ShowMessage({"Quit Game", "Any progress since your last save will be lost.",
                                   {"Quit", "Cancel"}, 0, [this](int i) {
                                     if (i == 0 && config_.on_quit) config_.on_quit();
                                   }});
                    }});
  }
}

void GuideDialog::SystemRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "Network Status",
                  .sub = backend_->server_name(),
                  .detail = backend_->connected() ? "Connected" : "Offline",
                  .icon = textures_.Icon(assets::Icon::kSystem)});
  rows.push_back({.label = "Guide Version", .detail = "1.0"});
}

void GuideDialog::CreditsRows(std::vector<Row>& rows, Page&) {
  {
    std::lock_guard lock(g_credits_mutex);
    for (const Credit& c : AppCredits()) rows.push_back({.label = c.who, .sub = c.what});
  }
  for (const Credit& c : kGuideCredits) rows.push_back({.label = c.who, .sub = c.what});
}

void GuideDialog::AccountRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "Sign Out", .icon = textures_.Icon(assets::Icon::kTurnOff), .activate = [this] { ConfirmSignOut(); }});
}

}  // namespace rexglue_guide
