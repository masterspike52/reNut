// Messages between friends.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "guide_dialog.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

void GuideDialog::MessagesRows(std::vector<Row>& rows, Page&) {
  rows.push_back({.label = "New Message", .glyph = Glyph::kInvite,
                  .activate = [this] { Push({PageKind::kNewMessage, "New Message"}); }});
  // One row per person, newest conversation first (messages arrive newest first).
  std::vector<std::string> order;
  std::map<std::string, int> unread;
  std::map<std::string, const Message*> latest;
  for (const Message& m : backend_->messages()) {
    if (!latest.count(m.with)) {
      latest[m.with] = &m;
      order.push_back(m.with);
    }
    unread[m.with] += m.unread;
  }
  for (const std::string& gt : order) {
    const Message& m = *latest[gt];
    rows.push_back({.label = gt,
                    .sub = (m.sent ? "You: " : "") + m.text,
                    .detail = TimeAgo(m.time),
                    .gamerpic = m.with_gamerpic,
                    .unread = unread[gt] > 0,
                    .activate = [this, gt] { OpenConversation(gt); }});
  }
  if (order.empty()) rows.push_back({.label = "No messages yet", .sub = "Send one to a friend.", .disabled = true});
}

void GuideDialog::ConversationRows(std::vector<Row>& rows, Page& page) {
  backend_->MarkConversationRead(page.arg);
  rows.push_back({.label = "Reply", .glyph = Glyph::kInvite, .activate = [this, gt = page.arg] { Compose(gt); }});
  for (const Message& m : backend_->messages()) {
    if (m.with != page.arg) continue;
    const std::string from = m.sent ? backend_->me().gamertag : m.with;
    rows.push_back({.label = m.sent ? "You" : m.with,
                    .sub = m.text,
                    .detail = TimeAgo(m.time),
                    .gamerpic = m.sent ? backend_->me().gamerpic : m.with_gamerpic,
                    .activate = [this, from, m] {
                      ShowMessage({from + " - " + TimeAgo(m.time), m.text, {"Reply", "Close"}, 0,
                                   [this, gt = m.with](int i) {
                                     if (i == 0) Compose(gt);
                                   }});
                    }});
  }
}

void GuideDialog::NewMessageRows(std::vector<Row>& rows, Page&) {
  for (const Friend& f : backend_->friends()) {
    rows.push_back({.label = f.profile.gamertag,
                    .sub = f.online ? (f.status.empty() ? "Online" : f.status) : "Last seen " + TimeAgo(f.last_seen),
                    .gamerpic = f.profile.gamerpic,
                    .icon_dim = !f.online,
                    .activate = [this, gt = f.profile.gamertag] { Compose(gt); }});
  }
  if (rows.empty()) rows.push_back({.label = "No friends yet", .sub = "Add some from the Social blade.", .disabled = true});
}

int GuideDialog::UnreadMessages() const {
  int unread = 0;
  for (const Message& m : backend_->messages()) unread += m.unread;
  return unread;
}

void GuideDialog::Compose(const std::string& gamertag) {
  OpenKeyboard({"Message to " + gamertag, "Up to 250 characters.", "", false, 250,
                [this, gamertag](std::optional<std::string> text) {
                  if (!text || text->empty()) return;
                  Busy("Sending message...");
                  backend_->SendTextMessage(gamertag, *text, [this, gamertag](const Result& r) {
                    busy_.reset();
                    if (r.ok) ShowToast({.glyph = Glyph::kInvite, .line1 = "Message sent", .line2 = "to " + gamertag});
                    else ShowMessage({"Send Message", r.error, {"OK"}});
                  });
                }});
}

void GuideDialog::OpenConversation(const std::string& gamertag) {
  backend_->MarkConversationRead(gamertag);
  Push({PageKind::kConversation, gamertag, gamertag});
}

}  // namespace rexglue_guide
