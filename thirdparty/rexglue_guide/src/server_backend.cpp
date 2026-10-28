// Backend that talks to a RexLiveless server (U:\NewReTip\Server).
//
// The account token is remembered in <data dir>/account.json. While signed in,
// /api/sync runs every few seconds: it is the presence heartbeat and returns
// friends, friend requests and notifications. If the server can't be reached
// the remembered profile still signs in, offline, and keeps retrying.

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>
#include <rex/logging.h>

#include "assets.h"
#include "backend.h"
#include "http.h"

namespace rexglue_guide {
namespace {

using json = nlohmann::json;

constexpr double kSyncInterval = 3.0;
// Syncs that may fail in a row (a slow or dropped request over the internet)
// before the Guide says the connection is lost.
constexpr int kSyncFailuresBeforeLost = 3;
// Override with REXLIVELESS_SERVER=http(s)://host:port.
constexpr const char* kDefaultServer = "https://rexliveless.xyz";

uint64_t ParseXuid(const json& j) {
  if (!j.is_string()) return 0;
  return std::strtoull(j.get<std::string>().c_str(), nullptr, 16);
}

Profile ParseProfile(const json& j) {
  Profile p;
  p.gamertag = j.value("gamertag", "");
  p.xuid = ParseXuid(j.value("xuid", json()));
  p.gamerpic = j.value("gamerpic", 0);
  p.gamerscore = j.value("gamerscore", 0u);
  p.motto = j.value("motto", "");
  return p;
}

std::string ErrorOf(const http::Response& r) {
  if (r.status == 0) return r.error.empty() ? "Can't reach the RexLiveless server." : r.error;
  try {
    const json j = json::parse(r.body);
    if (j.contains("error")) return j["error"].get<std::string>();
  } catch (...) {
  }
  return "The RexLiveless server had a problem (" + std::to_string(r.status) + ").";
}

NotificationKind KindOf(const std::string& kind) {
  if (kind == "invite") return NotificationKind::kInvite;
  if (kind == "crate") return NotificationKind::kCrate;
  if (kind == "blueprint") return NotificationKind::kBlueprint;
  if (kind == "friend_request") return NotificationKind::kFriendRequest;
  if (kind == "friend_accepted") return NotificationKind::kFriendOnline;
  return NotificationKind::kSystem;
}

class ServerBackend final : public Backend {
 public:
  explicit ServerBackend(std::filesystem::path data_dir)
      : account_path_(data_dir / "account.json"),
        gamerpic_dir_(data_dir / "gamerpics"),
        theme_dir_(data_dir / "themes") {
    const char* env = std::getenv("REXLIVELESS_SERVER");
    server_url_ = env && *env ? env : kDefaultServer;
    http::ParseUrl(server_url_, &endpoint_);
    LoadAccount();
    // A remembered account is who the player is from the start: titles read
    // the profile at boot, before the Guide's sign-in (SignInRemembered) has
    // been to the server, and one that sees it change afterwards takes it for
    // another player (Nuts & Bolts makes player 1 anew, without its save).
    if (!token_.empty() && remembered_.xuid) {
      me_ = remembered_;
      signed_in_ = true;
      PublishSnapshot();
    }
  }

  void Update(double now) override {
    now_ = now;
    worker_.Pump();
    if (!gamerpics_listed_) ListGamerpics();
    if (!themes_listed_) ListThemes();
    if (signed_in_ && !sync_in_flight_ && now - last_sync_ >= kSyncInterval) Sync();
  }

  bool signed_in() const override { return signed_in_; }
  const Profile& me() const override { return me_; }
  std::string remembered_gamertag() const override { return token_.empty() ? "" : remembered_.gamertag; }
  int remembered_gamerpic() const override { return remembered_.gamerpic; }

  void CreateAccount(const std::string& gamertag, const std::string& password, int gamerpic,
                     Callback done) override {
    Post("POST", "/api/account/create", {{"gamertag", gamertag}, {"password", password}, {"gamerpic", gamerpic}},
         [this, done](const http::Response& r) { FinishLogin(r, done); });
  }

  void SignIn(const std::string& gamertag, const std::string& password, Callback done) override {
    Post("POST", "/api/account/login", {{"gamertag", gamertag}, {"password", password}},
         [this, done](const http::Response& r) { FinishLogin(r, done); });
  }

  void SignInRemembered(Callback done) override {
    if (token_.empty()) return done({false, "No saved account."});
    Post("POST", "/api/sync", SyncBody(), [this, done](const http::Response& r) {
      if (r.status == 401) {
        // The remembered account (signed in from the start) is no longer valid.
        if (signed_in_) SignOut();
        else ForgetToken();
        return done({false, "Please sign in again."});
      }
      // Reachable or not, the remembered profile is signed in.
      me_ = remembered_;
      signed_in_ = true;
      last_sync_ = now_;
      if (r.status == 200) ApplySync(r.body);
      PublishSnapshot();
      done({});
    });
  }

  void SignOut() override {
    if (!token_.empty()) Post("POST", "/api/account/logout", json::object(), nullptr);
    ForgetToken();
    signed_in_ = false;
    connected_ = false;
    first_sync_ = true;
    me_ = {};
    friends_.clear();
    requests_.clear();
    notifications_.clear();
    messages_.clear();
    newest_message_ = 0;
    own_photo_ = 0;
    failed_syncs_ = 0;
    reported_.clear();
    PublishSnapshot();
  }

  void SetGamerpic(int gamerpic) override {
    me_.gamerpic = gamerpic;
    SaveAccount();
    Post("POST", "/api/profile", {{"gamerpic", gamerpic}}, nullptr);
    PublishSnapshot();
  }

  void SetMotto(const std::string& motto) override {
    me_.motto = motto;
    Post("POST", "/api/profile", {{"motto", motto}}, nullptr);
  }

  const std::vector<Friend>& friends() const override { return friends_; }
  const std::vector<FriendRequest>& friend_requests() const override { return requests_; }

  void SendFriendRequest(const std::string& gamertag, Callback done) override {
    Post("POST", "/api/friends/request", {{"gamertag", gamertag}}, [this, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)});
      SyncSoon();
      done({});
    });
  }

  void AnswerFriendRequest(const std::string& gamertag, bool accept) override {
    std::erase_if(requests_, [&](const FriendRequest& f) { return f.from.gamertag == gamertag; });
    std::erase_if(notifications_, [&](const Notification& n) {
      return n.kind == NotificationKind::kFriendRequest && n.from == gamertag;
    });
    Post("POST", "/api/friends/answer", {{"gamertag", gamertag}, {"accept", accept}},
         [this](const http::Response&) { SyncSoon(); });
  }

  void RemoveFriend(const std::string& gamertag) override {
    std::erase_if(friends_, [&](const Friend& f) { return f.profile.gamertag == gamertag; });
    PublishSnapshot();
    Post("POST", "/api/friends/remove", {{"gamertag", gamertag}}, [this](const http::Response&) { SyncSoon(); });
  }

  void SendInvite(const std::string& gamertag, const std::string& session_id, Callback done) override {
    Post("POST", "/api/invites", {{"to", gamertag}, {"session_id", session_id}, {"title_id", title_id_}},
         [done](const http::Response& r) {
           if (done) done(r.status == 200 ? Result{} : Result{false, ErrorOf(r)});
         });
  }

  const std::vector<Notification>& notifications() const override { return notifications_; }

  void MarkRead(uint64_t id) override {
    for (auto& n : notifications_) {
      if (n.id == id) n.unread = false;
    }
    Post("POST", "/api/notifications/read", {{"id", id}}, nullptr);
  }

  void Dismiss(uint64_t id) override {
    std::erase_if(notifications_, [&](const Notification& n) { return n.id == id; });
    Post("POST", "/api/notifications/dismiss", {{"id", id}}, nullptr);
  }

  void SetTitle(const std::string& title_id, const std::string& presence) override {
    title_id_ = title_id;
    presence_ = presence;
  }

  void ReportAchievements(const std::string& title_id, const std::vector<AchievementRecord>& unlocked) override {
    if (!signed_in_ || !connected_ || achievements_in_flight_) return;
    // Unlocks are stored per install, not per account. The first account
    // signed in here owns everything already unlocked; any other account only
    // gets what was unlocked after it first signed in on this install.
    const int64_t since = AchievementsSince(me_.xuid);
    json list = json::array();
    std::vector<uint32_t> ids;
    for (const auto& a : unlocked) {
      if (reported_.count(a.id)) continue;
      if (a.unlocked < since) {
        reported_.insert(a.id);
        continue;
      }
      list.push_back({{"id", a.id}, {"gamerscore", a.gamerscore}, {"label", a.label}, {"unlocked", a.unlocked}});
      ids.push_back(a.id);
    }
    if (list.empty()) return;
    achievements_in_flight_ = true;
    Post("POST", "/api/achievements", {{"title_id", title_id}, {"achievements", list}},
         [this, ids](const http::Response& r) {
           achievements_in_flight_ = false;
           if (r.status != 200) return;
           reported_.insert(ids.begin(), ids.end());
           try {
             me_.gamerscore = json::parse(r.body).value("gamerscore", me_.gamerscore);
           } catch (...) {
           }
         });
  }

  const std::vector<Message>& messages() const override { return messages_; }

  void SendTextMessage(const std::string& gamertag, const std::string& text, Callback done) override {
    Post("POST", "/api/messages", {{"to", gamertag}, {"text", text}}, [this, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)});
      SyncSoon();
      done({});
    });
  }

  void MarkConversationRead(const std::string& gamertag) override {
    bool any = false;
    for (auto& m : messages_) {
      any |= m.unread && m.with == gamertag;
      if (m.with == gamertag) m.unread = false;
    }
    if (any) Post("POST", "/api/messages/read", {{"with", gamertag}}, nullptr);
  }

  std::vector<int> gamerpic_choices() const override {
    std::vector<int> choices;
    for (int i = 0; i < assets::kGamerpicCount; ++i) choices.push_back(i);
    if (own_photo_) choices.push_back(own_photo_);
    choices.insert(choices.end(), server_gamerpics_.begin(), server_gamerpics_.end());
    return choices;
  }

  void UploadPhoto(std::string png, Callback done) override {
    worker_.Post(endpoint_, "PUT", "/api/profile/photo", png, token_,
                 [this, png, done](const http::Response& r) {
                   if (r.status != 200) return done({false, ErrorOf(r)});
                   std::string note;  // e.g. others see it once an admin trusts the account
                   try {
                     const json j = json::parse(r.body);
                     note = j.value("note", "");
                     const int gamerpic = j.value("gamerpic", 0);
                     gamerpic_png_[gamerpic] = png;
                     std::error_code ec;
                     std::filesystem::create_directories(gamerpic_dir_, ec);
                     std::ofstream(gamerpic_dir_ / (std::to_string(gamerpic) + ".png"), std::ios::binary | std::ios::trunc) << png;
                     own_photo_ = gamerpic;
                     me_.gamerpic = remembered_.gamerpic = gamerpic;
                     SaveAccount();
                     PublishSnapshot();
                   } catch (...) {
                   }
                   done({.note = note});
                 },
                 "image/png");
  }

  // Server gamerpics are kept in <data dir>/gamerpics/<number>.png; a number
  // never changes picture, so the copy there never goes stale.
  std::string ServerGamerpic(int gamerpic) override {
    if (gamerpic < kFirstServerGamerpic) return {};
    if (auto it = gamerpic_png_.find(gamerpic); it != gamerpic_png_.end()) return it->second;
    const auto path = gamerpic_dir_ / (std::to_string(gamerpic) + ".png");
    if (std::ifstream file(path, std::ios::binary); file) {
      std::string png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      if (!png.empty()) return gamerpic_png_[gamerpic] = std::move(png);
    }
    if (gamerpic_requested_.insert(gamerpic).second) {
      // Signed in: a player's own photo is only theirs to see until an admin
      // trusts them.
      worker_.Post(endpoint_, "GET", "/api/gamerpics/" + std::to_string(gamerpic), "", token_,
                   [this, gamerpic, path](const http::Response& r) {
                     if (r.status != 200 || r.body.empty()) {
                       gamerpic_requested_.erase(gamerpic);  // asked again on a later draw
                       return;
                     }
                     std::error_code ec;
                     std::filesystem::create_directories(gamerpic_dir_, ec);
                     std::ofstream(path, std::ios::binary | std::ios::trunc) << r.body;
                     gamerpic_png_[gamerpic] = r.body;
                   });
    }
    return {};
  }

  const std::vector<ServerTheme>& themes() const override { return themes_; }
  bool themes_ready() const override { return !themes_version_.empty(); }
  int theme_quota() const override { return signed_in_ ? theme_quota_ : 0; }

  void SaveTheme(int id, const std::string& name, const std::string& data, bool nsfw,
                 std::function<void(const Result&, int)> done) override {
    json body = {{"name", name}, {"nsfw", nsfw}};
    try {
      body["data"] = json::parse(data);
    } catch (...) {
      return done({false, "That theme couldn't be read."}, 0);
    }
    Post("POST", id ? "/api/themes/" + std::to_string(id) : "/api/themes", body, [this, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)}, 0);
      done({}, StoreTheme(r.body));
    });
  }

  void SetThemeImage(int id, std::string image, Callback done) override {
    const std::string path = "/api/themes/" + std::to_string(id) + "/image";
    auto finish = [this, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)});
      StoreTheme(r.body);
      done({});
    };
    if (image.empty()) {
      worker_.Post(endpoint_, "DELETE", path, "", token_, finish);
    } else {
      const bool png = image.size() > 4 && image.compare(1, 3, "PNG") == 0;
      worker_.Post(endpoint_, "PUT", path, std::move(image), token_, finish, png ? "image/png" : "image/jpeg");
    }
  }

  void LikeTheme(int id, bool like, Callback done) override {
    Post("POST", "/api/themes/" + std::to_string(id) + "/like", {{"like", like}}, [this, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)});
      StoreTheme(r.body);
      done({});
    });
  }

  void SetThemeInUse(int id) override { theme_in_use_ = id; }

  void DeleteTheme(int id, Callback done) override {
    worker_.Post(endpoint_, "DELETE", "/api/themes/" + std::to_string(id), "", token_, [this, id, done](const http::Response& r) {
      if (r.status != 200) return done({false, ErrorOf(r)});
      std::erase_if(themes_, [id](const ServerTheme& t) { return t.id == id; });
      done({});
    });
  }

  // Kept in <data dir>/themes/<id>_<updated>.img: a change to the theme is a
  // new file.
  std::string ThemeImage(int id, int64_t updated) override {
    const std::string name = std::to_string(id) + "_" + std::to_string(updated);
    if (auto it = theme_images_.find(name); it != theme_images_.end()) return it->second;
    const auto path = theme_dir_ / (name + ".img");
    if (std::ifstream file(path, std::ios::binary); file) {
      std::string image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      if (!image.empty()) return theme_images_[name] = std::move(image);
    }
    if (theme_images_requested_.insert(name).second) {
      const std::string url = "/api/themes/" + std::to_string(id) + "/image?v=" + std::to_string(updated);
      worker_.Post(endpoint_, "GET", url, "", "", [this, name, path](const http::Response& r) {
        if (r.status != 200 || r.body.empty()) return;  // not asked again: the picture is gone
        std::error_code ec;
        std::filesystem::create_directories(theme_dir_, ec);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << r.body;
        theme_images_[name] = r.body;
      });
    }
    return {};
  }

  bool connected() const override { return connected_; }
  std::string server_name() const override { return "RexLiveless (" + endpoint_.host + ":" + std::to_string(endpoint_.port) + ")"; }

  LiveSnapshot Snapshot() const override {
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
  }

 private:
  void Post(const std::string& method, const std::string& path, const json& body,
            std::function<void(const http::Response&)> done) {
    worker_.Post(endpoint_, method, path, body.dump(), token_, std::move(done));
  }

  json SyncBody() const { return {{"presence", presence_}, {"title_id", title_id_}, {"theme_id", theme_in_use_}}; }

  void Sync() {
    sync_in_flight_ = true;
    Post("POST", "/api/sync", SyncBody(), [this](const http::Response& r) {
      sync_in_flight_ = false;
      last_sync_ = now_;
      if (r.status == 401 && signed_in_) {
        SignOut();
        if (on_notification) on_notification({.text = "You were signed out. Please sign in again."});
        return;
      }
      if (r.status != 200) {
        ++failed_syncs_;
        REXKRNL_WARN("RexLiveless: sync failed ({} {}), {} in a row", r.status, r.error, failed_syncs_);
        if (connected_ && failed_syncs_ >= kSyncFailuresBeforeLost) {
          if (on_notification) on_notification({.text = "Lost connection to RexLiveless."});
          connected_ = false;
          PublishSnapshot();
        }
        // Try again soon rather than a full interval later.
        last_sync_ = now_ - kSyncInterval + 1.0;
        return;
      }
      failed_syncs_ = 0;
      ApplySync(r.body);
    });
  }

  void SyncSoon() { last_sync_ = now_ - kSyncInterval + 0.2; }

  void FinishLogin(const http::Response& r, const Callback& done) {
    if (r.status != 200) return done({false, ErrorOf(r)});
    try {
      const json j = json::parse(r.body);
      token_ = j.at("token").get<std::string>();
      me_ = ParseProfile(j.at("profile"));
    } catch (...) {
      return done({false, "The RexLiveless server sent a bad answer."});
    }
    remembered_ = me_;
    SaveAccount();
    signed_in_ = true;
    first_sync_ = true;
    reported_.clear();
    PublishSnapshot();
    Sync();
    done({});
  }

  void ApplySync(const std::string& body) {
    json j;
    try {
      j = json::parse(body);
    } catch (...) {
      return;
    }
    const bool was_connected = connected_;
    connected_ = true;
    if (!was_connected && !first_sync_ && on_notification) {
      on_notification({.text = "Connected to RexLiveless."});
    }
    if (j.contains("me")) {
      me_ = ParseProfile(j["me"]);
      if (me_.gamertag != remembered_.gamertag || me_.gamerpic != remembered_.gamerpic) {
        remembered_ = me_;
        SaveAccount();
      }
    }
    public_ip_ = j.value("public_ip", "");
    xts_port_ = j.value("xts_port", uint16_t(0));
    relay_port_ = j.value("relay_port", uint16_t(0));
    relay_key_ = j.value("relay_key", "");

    std::vector<Friend> friends;
    for (const json& f : j.value("friends", json::array())) {
      Friend fr;
      fr.profile = ParseProfile(f);
      fr.online = f.value("online", false);
      fr.status = f.value("presence", "");
      fr.title_id = f.value("title_id", "");
      fr.last_seen = f.value("last_seen", int64_t(0));
      if (fr.status.empty() && fr.online) fr.status = "Online";
      if (!first_sync_ && fr.online && on_notification) {
        auto old = std::find_if(friends_.begin(), friends_.end(),
                                [&](const Friend& o) { return o.profile.xuid == fr.profile.xuid; });
        if (old != friends_.end() && !old->online) {
          on_notification({.kind = NotificationKind::kFriendOnline,
                           .from = fr.profile.gamertag,
                           .from_xuid = fr.profile.xuid,
                           .from_gamerpic = fr.profile.gamerpic,
                           .text = "is now online"});
        }
      }
      friends.push_back(std::move(fr));
    }
    friends_ = std::move(friends);

    requests_.clear();
    for (const json& r : j.value("requests", json::array())) {
      requests_.push_back({ParseProfile(r), r.value("time", int64_t(0))});
    }

    std::vector<Notification> notifications;
    uint64_t newest = newest_notification_;
    for (const json& n : j.value("notifications", json::array())) {
      Notification note;
      note.id = n.value("id", uint64_t(0));
      note.kind = KindOf(n.value("kind", ""));
      note.from = n.value("from", "");
      note.from_xuid = ParseXuid(n.value("from_xuid", json()));
      note.from_gamerpic = n.value("from_gamerpic", 0);
      note.text = n.value("text", "");
      note.time = n.value("time", int64_t(0));
      note.unread = n.value("unread", true);
      if (n.contains("data") && n["data"].is_object()) note.session_id = n["data"].value("session_id", "");
      if (note.id > newest_notification_ && !first_sync_ && on_notification) on_notification(note);
      newest = std::max(newest, note.id);
      notifications.push_back(std::move(note));
    }
    notifications_ = std::move(notifications);
    newest_notification_ = newest;

    std::vector<Message> messages;
    uint64_t newest_message = newest_message_;
    for (const json& m : j.value("messages", json::array())) {
      Message msg;
      msg.id = m.value("id", uint64_t(0));
      msg.with = m.value("with", "");
      msg.with_gamerpic = m.value("with_gamerpic", 0);
      msg.sent = m.value("sent", false);
      msg.text = m.value("text", "");
      msg.time = m.value("time", int64_t(0));
      msg.unread = m.value("unread", false);
      if (msg.id > newest_message_ && !msg.sent && !first_sync_ && on_notification) {
        on_notification({.kind = NotificationKind::kMessage,
                         .from = msg.with,
                         .from_gamerpic = msg.with_gamerpic,
                         .text = msg.text});
      }
      newest_message = std::max(newest_message, msg.id);
      messages.push_back(std::move(msg));
    }
    messages_ = std::move(messages);
    newest_message_ = newest_message;
    own_photo_ = j.value("photo", 0);
    theme_quota_ = j.value("theme_quota", 0);
    if (j.value("themes_version", themes_version_) != themes_version_ && !themes_in_flight_) ListThemes();
    ApplyGamerpicList(j);
    first_sync_ = false;
    PublishSnapshot();
  }

  void ListThemes() {
    themes_listed_ = true;
    themes_in_flight_ = true;
    // Signed in, it says which this player likes.
    worker_.Post(endpoint_, "GET", "/api/themes", "", token_, [this](const http::Response& r) {
      themes_in_flight_ = false;
      if (r.status != 200) {
        themes_listed_ = false;  // again on a later frame
        return;
      }
      try {
        const json j = json::parse(r.body);
        std::vector<ServerTheme> themes;
        for (const json& t : j.value("themes", json::array())) themes.push_back(ParseTheme(t));
        themes_ = std::move(themes);
        themes_version_ = j.value("version", "");
      } catch (...) {
      }
    });
  }

  static ServerTheme ParseTheme(const json& t) {
    ServerTheme theme;
    theme.id = t.value("id", 0);
    theme.name = t.value("name", "");
    theme.author = t.value("author", "");
    theme.author_xuid = ParseXuid(t.value("author_xuid", json()));
    theme.data = t.value("data", json::object()).dump();
    theme.has_image = t.value("has_image", false);
    theme.nsfw = t.value("nsfw", false);
    theme.likes = t.value("likes", 0);
    theme.users = t.value("users", 0);
    theme.liked = t.value("liked", false);
    theme.created = t.value("created", int64_t(0));
    theme.updated = t.value("updated", int64_t(0));
    return theme;
  }

  // A theme the server sent back after a change, into the list; its id.
  int StoreTheme(const std::string& body) {
    try {
      const ServerTheme theme = ParseTheme(json::parse(body).at("theme"));
      auto it = std::find_if(themes_.begin(), themes_.end(), [&](const ServerTheme& t) { return t.id == theme.id; });
      if (it != themes_.end()) *it = theme;
      else themes_.insert(themes_.begin(), theme);
      return theme.id;
    } catch (...) {
      return 0;
    }
  }

  void ListGamerpics() {
    gamerpics_listed_ = true;
    worker_.Post(endpoint_, "GET", "/api/gamerpics", "", "", [this](const http::Response& r) {
      if (r.status != 200) return;
      try {
        ApplyGamerpicList(json::parse(r.body));
      } catch (...) {
      }
    });
  }

  void ApplyGamerpicList(const json& j) {
    if (!j.contains("gamerpics") || !j["gamerpics"].is_array()) return;
    server_gamerpics_.clear();
    for (const json& g : j["gamerpics"]) {
      const int n = g.value("gamerpic", 0);
      if (n >= kFirstServerGamerpic) server_gamerpics_.push_back(n);
    }
  }

  void PublishSnapshot() {
    std::lock_guard lock(snapshot_mutex_);
    snapshot_.signed_in = signed_in_;
    snapshot_.connected = connected_;
    snapshot_.me = me_;
    snapshot_.friends = friends_;
    snapshot_.public_ip = public_ip_;
    snapshot_.token = token_;
    snapshot_.server_host = endpoint_.host;
    snapshot_.server_port = endpoint_.port;
    snapshot_.server_tls = endpoint_.tls;
    snapshot_.xts_port = xts_port_;
    snapshot_.relay_port = relay_port_;
    snapshot_.relay_key = relay_key_;
  }

  void LoadAccount() {
    std::ifstream file(account_path_);
    if (!file) return;
    try {
      const json j = json::parse(file);
      // A token only means something to the server that issued it.
      if (j.value("server", "") == server_url_) token_ = j.value("token", "");
      remembered_.gamertag = j.value("gamertag", "");
      remembered_.xuid = ParseXuid(j.value("xuid", json()));
      remembered_.gamerpic = j.value("gamerpic", 0);
    } catch (...) {
    }
  }

  void SaveAccount() {
    std::error_code ec;
    std::filesystem::create_directories(account_path_.parent_path(), ec);
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", static_cast<unsigned long long>(remembered_.xuid));
    const json j = {{"server", server_url_},
                    {"token", token_},
                    {"gamertag", remembered_.gamertag},
                    {"xuid", xuid},
                    {"gamerpic", remembered_.gamerpic}};
    std::ofstream(account_path_, std::ios::trunc) << j.dump(2);
  }

  void ForgetToken() {
    token_.clear();
    SaveAccount();
  }

  // <data dir>/achievements.json: {"owner": xuid, "accounts": {xuid: first sign-in}}.
  // Returns the earliest unlock time that counts for this account (0 = all).
  int64_t AchievementsSince(uint64_t xuid) {
    if (!xuid) return INT64_MAX;
    if (xuid == since_xuid_) return since_;
    since_xuid_ = xuid;
    since_ = ReadAchievementsSince(xuid);
    return since_;
  }

  int64_t ReadAchievementsSince(uint64_t xuid) {
    char key[17];
    std::snprintf(key, sizeof(key), "%016llX", static_cast<unsigned long long>(xuid));
    const auto path = account_path_.parent_path() / "achievements.json";
    json j = json::object();
    if (std::ifstream file(path); file) {
      try {
        j = json::parse(file);
      } catch (...) {
        j = json::object();
      }
    }
    bool changed = false;
    if (!j.contains("owner") || !j["owner"].is_string()) {
      j["owner"] = key;
      changed = true;
    }
    if (!j.contains("accounts") || !j["accounts"].is_object()) j["accounts"] = json::object();
    if (!j["accounts"].contains(key)) {
      j["accounts"][key] = int64_t(std::time(nullptr));
      changed = true;
    }
    if (changed) std::ofstream(path, std::ios::trunc) << j.dump(2);
    if (j["owner"].get<std::string>() == key) return 0;
    return j["accounts"][key].get<int64_t>();
  }

  std::filesystem::path account_path_;
  std::filesystem::path gamerpic_dir_;
  std::filesystem::path theme_dir_;
  std::string server_url_;
  http::Endpoint endpoint_;
  http::Worker worker_;

  std::string token_;
  Profile remembered_;
  Profile me_;
  bool signed_in_ = false;
  bool connected_ = false;
  bool first_sync_ = true;
  bool sync_in_flight_ = false;
  bool achievements_in_flight_ = false;
  double now_ = 0.0;
  double last_sync_ = -100.0;
  uint64_t newest_notification_ = 0;
  uint64_t newest_message_ = 0;
  int failed_syncs_ = 0;
  std::vector<Message> messages_;
  std::vector<int> server_gamerpics_;
  int own_photo_ = 0;  // the account's uploaded photo, 0 if none
  bool gamerpics_listed_ = false;
  std::map<int, std::string> gamerpic_png_;
  std::set<int> gamerpic_requested_;
  std::vector<ServerTheme> themes_;
  std::string themes_version_;
  bool themes_listed_ = false;
  bool themes_in_flight_ = false;
  int theme_quota_ = 0;
  int theme_in_use_ = 0;
  std::map<std::string, std::string> theme_images_;
  std::set<std::string> theme_images_requested_;
  std::string title_id_;
  std::string presence_;
  std::string public_ip_;
  uint16_t xts_port_ = 0;
  uint16_t relay_port_ = 0;
  std::string relay_key_;
  std::set<uint32_t> reported_;
  uint64_t since_xuid_ = 0;
  int64_t since_ = 0;

  std::vector<Friend> friends_;
  std::vector<FriendRequest> requests_;
  std::vector<Notification> notifications_;

  mutable std::mutex snapshot_mutex_;
  LiveSnapshot snapshot_;
};

}  // namespace

std::unique_ptr<Backend> CreateServerBackend(std::filesystem::path data_dir) {
  return std::make_unique<ServerBackend>(std::move(data_dir));
}

}  // namespace rexglue_guide
