// Friends, friend requests, notifications and gamer cards.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "guide_dialog.h"
#include "live_sessions.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::FriendsRows(std::vector<Row>& rows, Page&) {
  std::vector<const Friend*> sorted;
  for (const Friend& f : backend_->friends()) sorted.push_back(&f);
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const Friend* a, const Friend* b) { return a->online > b->online; });
  for (const Friend* f : sorted) {
    rows.push_back({.label = f->profile.gamertag,
                    .sub = f->online ? (f->status.empty() ? "Online" : f->status)
                                     : "Last seen " + TimeAgo(f->last_seen),
                    .gamerpic = f->profile.gamerpic,
                    .icon_dim = !f->online,
                    .activate = [this, gt = f->profile.gamertag] { Push({PageKind::kGamercard, gt, gt}); }});
  }
  if (rows.empty()) rows.push_back({.label = "No friends yet", .sub = "Add some from the Social blade.", .disabled = true});
}

void GuideDialog::RequestsRows(std::vector<Row>& rows, Page&) {
  for (const FriendRequest& r : backend_->friend_requests()) {
    rows.push_back({.label = r.from.gamertag,
                    .sub = "Sent " + TimeAgo(r.time),
                    .gamerpic = r.from.gamerpic,
                    .activate = [this, gt = r.from.gamertag] { AnswerRequest(gt); }});
  }
  if (rows.empty()) rows.push_back({.label = "No friend requests", .disabled = true});
}

void GuideDialog::NotificationsRows(std::vector<Row>& rows, Page&) {
  for (const Notification& n : backend_->notifications()) {
    Row row{.label = n.from.empty() ? "RexLiveless" : n.from,
            .sub = n.text,
            .detail = TimeAgo(n.time),
            .gamerpic = n.from.empty() ? -1 : n.from_gamerpic,
            .unread = n.unread,
            .activate = [this, n] { OpenNotification(n); }};
    switch (n.kind) {
      case NotificationKind::kInvite: row.glyph = Glyph::kInvite; break;
      case NotificationKind::kCrate:
      case NotificationKind::kBlueprint: row.glyph = Glyph::kCrate; break;
      case NotificationKind::kFriendRequest: row.glyph = Glyph::kAddPerson; break;
      default: break;
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty()) rows.push_back({.label = "You're all caught up", .disabled = true});
}

void GuideDialog::GamercardRows(std::vector<Row>& rows, Page& page) {
  const bool mine = page.arg == backend_->me().gamertag;
  if (mine) {
    rows.push_back({.label = "Change Gamerpic", .activate = [this] { Push({PageKind::kGamerpics, "Choose Gamerpic", "settings"}); }});
    rows.push_back({.label = "Upload Photo", .detail = "256 x 256", .activate = [this] { ChoosePhoto(); }});
    rows.push_back({.label = "Edit Motto", .activate = [this] { EditMotto(); }});
  } else if (!FindFriend(page.arg)) {
    // Someone the title showed (XShowGamerCardUI) who isn't a friend.
    rows.push_back({.label = "Send Friend Request", .activate = [this, gt = page.arg] {
                      Busy("Sending friend request...");
                      backend_->SendFriendRequest(gt, [this, gt](const Result& r) {
                        busy_.reset();
                        if (r.ok) ShowToast({.glyph = Glyph::kAddPerson, .line1 = "Friend request sent", .line2 = "to " + gt});
                        else ShowMessage({"Add Friend", r.error, {"OK"}});
                      });
                    }});
  } else {
    const Friend* f = FindFriend(page.arg);
    rows.push_back({.label = "Send Message", .activate = [this, gt = page.arg] { Compose(gt); }});
    rows.push_back({.label = "Invite to Game",
                    .disabled = !f || !f->online,
                    .activate = [this, gt = page.arg] {
                      const std::string session = sessions::HostedSessionId();
                      if (session.empty()) {
                        ShowMessage({"Invite to Game", "Host a garden on Xbox LIVE first, then invite " + gt + ".", {"OK"}});
                        return;
                      }
                      backend_->SendInvite(gt, session, [this, gt](const Result& r) {
                        if (r.ok) ShowToast({.glyph = Glyph::kInvite, .line1 = "Invite sent", .line2 = "to " + gt});
                        else ShowMessage({"Invite to Game", r.error, {"OK"}});
                      });
                    }});
    rows.push_back({.label = "Remove Friend", .activate = [this, gt = page.arg] {
                      ShowMessage({"Remove Friend", "Remove " + gt + " from your friends list?",
                                   {"Remove", "Cancel"}, 0, [this, gt](int i) {
                                     if (i != 0) return;
                                     backend_->RemoveFriend(gt);
                                     Pop();
                                   }});
                    }});
  }
}

const Friend* GuideDialog::FindFriend(const std::string& gamertag) const {
  for (const Friend& f : backend_->friends()) {
    if (f.profile.gamertag == gamertag) return &f;
  }
  return nullptr;
}

std::string GuideDialog::GamertagOf(uint64_t xuid) const {
  // Titles pass the offline profile's XUID (not 0009...) for the local
  // player: that's the signed-in account too.
  if (xuid == backend_->me().xuid || (xuid >> 48) != 0x0009) return backend_->me().gamertag;
  for (const Friend& f : backend_->friends()) {
    if (f.profile.xuid == xuid) return f.profile.gamertag;
  }
  return sessions::HostGamertag(xuid);
}

void GuideDialog::ShowTitleGamercard(uint64_t xuid) {
  if (screen_ == Screen::kClosed) {
    // The title's request is player 1's.
    user_ = 0;
    controller_ = -1;
    backend_ = &BackendFor(0);
  }
  if (screen_ == Screen::kSignIn || !backend_->signed_in()) return;
  const std::string gamertag = GamertagOf(xuid);
  if (gamertag.empty()) {
    REXKRNL_INFO("Rexglue Guide: no gamer card for XUID {:016X}: not a friend or a host", xuid);
    ShowToast({.glyph = Glyph::kAddPerson, .line1 = "Gamer card unavailable",
               .line2 = "That player isn't known to RexLiveless"});
    return;
  }
  const Page card{PageKind::kGamercard, gamertag == backend_->me().gamertag ? "Profile" : gamertag, gamertag};
  if (screen_ == Screen::kGuide && !closing_) {
    Push(card);
    return;
  }
  OpenGuide();
  blade_ = kSocialBlade;
  stacks_[kSocialBlade] = {card};
  if (!std::exchange(title_ui_, true)) {
    if (auto* kernel = REX_KERNEL_STATE()) kernel->BroadcastNotification(kNotifySystemUi, 1);
  }
}

void GuideDialog::AddFriend() {
  OpenKeyboard({"Add Friend", "Enter the gamertag of the person you want to add.", "", false, 15,
                  [this](std::optional<std::string> text) {
                    if (!text || text->empty()) return;
                    Busy("Sending friend request...");
                    backend_->SendFriendRequest(*text, [this, gt = *text](const Result& r) {
                      busy_.reset();
                      if (r.ok) ShowToast({.glyph = Glyph::kAddPerson, .line1 = "Friend request sent", .line2 = "to " + gt});
                      else ShowMessage({"Add Friend", r.error, {"OK"}});
                    });
                  }});
}

void GuideDialog::AnswerRequest(const std::string& gamertag) {
  ShowMessage({"Friend Request", gamertag + " wants to be your friend.", {"Accept", "Decline", "Cancel"}, 0,
               [this, gamertag](int i) {
                 if (i == 0 || i == 1) backend_->AnswerFriendRequest(gamertag, i == 0);
               }});
}

void GuideDialog::OpenNotification(const Notification& n) {
  backend_->MarkRead(n.id);
  switch (n.kind) {
    case NotificationKind::kInvite:
      ShowMessage({"Game Invite", n.from + " " + n.text + ".", {"Join Game", "Decline", "Cancel"}, 0,
                   [this, n](int i) {
                     if (i == 1) backend_->Dismiss(n.id);
                     if (i != 0) return;
                     std::string error;
                     if (n.session_id.empty() || !sessions::AcceptInvite(n.session_id, n.from_xuid, &error)) {
                       ShowMessage({"Game Invite", error.empty() ? "That game has ended." : error, {"OK"}});
                       return;
                     }
                     backend_->Dismiss(n.id);
                     Close();
                   }});
      break;
    case NotificationKind::kCrate:
      ShowMessage({"Crate Delivered", n.from + " " + n.text + ". Open it from your garden.", {"OK"}});
      break;
    case NotificationKind::kBlueprint:
      ShowMessage({"Blueprint Received", n.from + " " + n.text + ". Find it in your garage's blueprint inbox.", {"OK"}});
      break;
    case NotificationKind::kFriendRequest: AnswerRequest(n.from); break;
    default: ShowMessage({n.from.empty() ? "RexLiveless" : n.from, n.text, {"OK"}}); break;
  }
}

}  // namespace rexglue_guide
