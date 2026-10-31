// Stand-in for the Guide server: accounts are remembered locally and the
// social data is canned, so the UI can be built and reviewed without a server.

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

#include "assets.h"
#include "backend.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace rexglue_guide {

const char* ThisPlatform() {
#if defined(__SWITCH__)
  return "switch";
#elif defined(__ORBIS__) || defined(__PROSPERO__)  // PS4, PS5
  return "playstation";
#elif defined(__ANDROID__)
  return "android";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  return "ios";
#elif defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#else
  return "";
#endif
}

std::string ValidateGamertag(const std::string& gamertag) {
  if (gamertag.empty()) return "Enter a gamertag.";
  if (gamertag.size() > 15) return "Gamertags can be up to 15 characters.";
  if (!std::isalpha(static_cast<unsigned char>(gamertag[0])))
    return "Gamertags must start with a letter.";
  for (size_t i = 0; i < gamertag.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(gamertag[i]);
    if (c == ' ') {
      if (gamertag[i - 1] == ' ' || i + 1 == gamertag.size())
        return "Gamertags can't have double or trailing spaces.";
    } else if (!std::isalnum(c)) {
      return "Gamertags can only use letters, numbers and spaces.";
    }
  }
  return {};
}

namespace {

int64_t Now() {
  return static_cast<int64_t>(std::time(nullptr));
}

class MockBackend final : public Backend {
 public:
  explicit MockBackend(std::filesystem::path data_dir)
      : account_path_(std::move(data_dir) / "mock_account.txt") {
    LoadRemembered();
  }

  void Update(double now) override {
    now_ = now;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now >= it->at) {
        auto run = std::move(it->run);
        it = pending_.erase(it);
        run();
      } else {
        ++it;
      }
    }
  }

  bool signed_in() const override { return signed_in_; }
  const Profile& me() const override { return me_; }
  std::string remembered_gamertag() const override { return remembered_.gamertag; }
  int remembered_gamerpic() const override { return remembered_.gamerpic; }

  void CreateAccount(const std::string& gamertag, const std::string& password, int gamerpic,
                     Callback done) override {
    Later(1.2, [this, gamertag, password, gamerpic, done] {
      if (auto error = ValidateGamertag(gamertag); !error.empty()) return done({false, error});
      if (password.size() < 6) return done({false, "Passwords need at least 6 characters."});
      if (gamertag == "Taken") return done({false, "That gamertag is already taken."});
      Profile profile{gamertag, 0x0009000000000100, gamerpic, 0, ""};
      Remember(profile);
      FinishSignIn(profile);
      done({});
    });
  }

  void SignIn(const std::string& gamertag, const std::string& password, Callback done) override {
    Later(1.0, [this, gamertag, password, done] {
      if (password.empty()) return done({false, "Enter your password."});
      Profile profile = remembered_.gamertag == gamertag ? remembered_
                                                         : Profile{gamertag, 0x0009000000000100, 0, 1250, ""};
      Remember(profile);
      FinishSignIn(profile);
      done({});
    });
  }

  void SignInRemembered(Callback done) override {
    Later(0.6, [this, done] {
      if (remembered_.gamertag.empty()) return done({false, "No saved account."});
      FinishSignIn(remembered_);
      done({});
    });
  }

  void SignOut() override {
    signed_in_ = false;
    me_ = {};
    friends_.clear();
    requests_.clear();
    notifications_.clear();
    pending_.clear();
    remembered_ = {};
    std::error_code ec;
    std::filesystem::remove(account_path_, ec);
  }

  void SetGamerpic(int gamerpic) override {
    me_.gamerpic = gamerpic;
    Remember(me_);
  }

  void SetMotto(const std::string& motto) override {
    me_.motto = motto;
    Remember(me_);
  }

  const std::vector<Friend>& friends() const override { return friends_; }
  const std::vector<FriendRequest>& friend_requests() const override { return requests_; }

  void SendFriendRequest(const std::string& gamertag, Callback done) override {
    Later(0.8, [this, gamertag, done] {
      if (auto error = ValidateGamertag(gamertag); !error.empty()) return done({false, error});
      if (gamertag == me_.gamertag) return done({false, "You can't add yourself."});
      const bool known = std::any_of(friends_.begin(), friends_.end(), [&](const Friend& f) {
        return f.profile.gamertag == gamertag;
      });
      if (known) return done({false, gamertag + " is already on your friends list."});
      done({});
      // Pretend they accept a few seconds later.
      Later(6.0, [this, gamertag] {
        friends_.push_back({{gamertag, 0x0009000000000006, 4, 3200, ""}, true, "Online", "", 0});
        Push(NotificationKind::kFriendOnline, gamertag, 4, "accepted your friend request");
      });
    });
  }

  void AnswerFriendRequest(const std::string& gamertag, bool accept) override {
    auto it = std::find_if(requests_.begin(), requests_.end(),
                           [&](const FriendRequest& r) { return r.from.gamertag == gamertag; });
    if (it == requests_.end()) return;
    if (accept) friends_.push_back({it->from, true, "Online", "", 0});
    requests_.erase(it);
    std::erase_if(notifications_, [&](const Notification& n) {
      return n.kind == NotificationKind::kFriendRequest && n.from == gamertag;
    });
  }

  void RemoveFriend(const std::string& gamertag) override {
    std::erase_if(friends_, [&](const Friend& f) { return f.profile.gamertag == gamertag; });
  }

  void SendInvite(const std::string&, const std::string&, Callback done) override { done({}); }

  void SetTitle(const std::string&, const std::string&) override {}
  void ReportAchievements(const std::string&, const std::vector<AchievementRecord>&) override {}

  LiveSnapshot Snapshot() const override {
    LiveSnapshot snapshot;
    snapshot.signed_in = signed_in_;
    snapshot.connected = signed_in_;
    snapshot.me = me_;
    snapshot.friends = friends_;
    return snapshot;
  }

  const std::vector<Notification>& notifications() const override { return notifications_; }

  void MarkRead(uint64_t id) override {
    for (auto& n : notifications_) {
      if (n.id == id) n.unread = false;
    }
  }

  void Dismiss(uint64_t id) override {
    std::erase_if(notifications_, [&](const Notification& n) { return n.id == id; });
  }

  const std::vector<Message>& messages() const override { return messages_; }

  void SendTextMessage(const std::string& gamertag, const std::string& text, Callback done) override {
    messages_.insert(messages_.begin(), {next_message_++, gamertag, GamerpicOf(gamertag), true, text, Now(), false});
    done({});
  }

  void MarkConversationRead(const std::string& gamertag) override {
    for (auto& m : messages_) {
      if (m.with == gamertag) m.unread = false;
    }
  }

  std::vector<int> gamerpic_choices() const override {
    std::vector<int> choices;
    for (int i = 0; i < assets::kGamerpicCount; ++i) choices.push_back(i);
    return choices;
  }
  std::string ServerGamerpic(int) override { return {}; }
  void UploadPhoto(std::string, Callback done) override { done({false, "Photos need a RexLiveless server."}); }
  const std::vector<ServerTheme>& themes() const override { return themes_; }
  bool themes_ready() const override { return true; }
  int theme_quota() const override { return 0; }
  void SaveTheme(int, const std::string&, const std::string&, bool, std::function<void(const Result&, int)> done) override {
    done({false, "Themes need a RexLiveless server."}, 0);
  }
  void LikeTheme(int, bool, Callback done) override { done({false, "Themes need a RexLiveless server."}); }
  void SetThemeInUse(int) override {}
  void SetThemeImage(int, std::string, Callback done) override { done({false, "Themes need a RexLiveless server."}); }
  void DeleteTheme(int, Callback done) override { done({false, "Themes need a RexLiveless server."}); }
  std::string ThemeImage(int, int64_t) override { return {}; }

  const std::vector<GamerpicGame>* gamerpic_games(std::string* error) override {
    if (error) *error = "The Gamerpic Database needs a RexLiveless server.";
    return nullptr;
  }
  void GameGamerpics(const std::string&, std::function<void(const Result&, std::vector<std::string>)> done) override {
    done({false, "The Gamerpic Database needs a RexLiveless server."}, {});
  }
  std::string GameGamerpicImage(const std::string&, const std::string&, bool* missing, bool) override {
    if (missing) *missing = true;
    return {};
  }
  void SetGameGamerpic(const std::string&, const std::string&, Callback done) override {
    done({false, "The Gamerpic Database needs a RexLiveless server."});
  }

  bool is_admin() const override { return false; }
  void AchievementIconIds(const std::string&, std::function<void(const Result&, std::vector<uint32_t>)> done) override {
    done({false, "Achievement icons need a RexLiveless server."}, {});
  }
  void UploadAchievementIcon(const std::string&, uint32_t, std::string, Callback done) override {
    done({false, "Achievement icons need a RexLiveless server."});
  }
  std::string AchievementIconImage(const std::string&, uint32_t, int64_t) override { return {}; }

  const TitleInfo* title(const std::string&) const override { return nullptr; }
  std::string TitleIcon(const std::string&, int64_t) override { return {}; }
  void ReportTitleInfo(const std::string&, const std::string&, uint32_t, uint32_t,
                       const std::map<uint32_t, std::string>&) override {}
  void PlayerGames(const std::string&, std::function<void(const Result&, std::vector<PlayedGame>)> done) override {
    done({false, "Profiles' games need a RexLiveless server."}, {});
  }
  void PlayerGame(const std::string&, const std::string&,
                  std::function<void(const Result&, PlayedGameDetail)> done) override {
    done({false, "Profiles' games need a RexLiveless server."}, {});
  }

  bool connected() const override { return signed_in_; }
  std::string server_name() const override { return "Mock server (offline preview)"; }
  std::string unofficial_server() const override { return "mock server"; }

 private:
  struct Pending {
    double at;
    std::function<void()> run;
  };

  int GamerpicOf(const std::string& gamertag) const {
    for (const Friend& f : friends_) {
      if (f.profile.gamertag == gamertag) return f.profile.gamerpic;
    }
    return 0;
  }

  void Later(double seconds, std::function<void()> run) {
    pending_.push_back({now_ + seconds, std::move(run)});
  }

  void FinishSignIn(const Profile& profile) {
    signed_in_ = true;
    me_ = profile;
    const int64_t now = Now();
    friends_ = {
        {{"PinataPal", 0x0009000000000001, 1, 41250, "Sweet dreams are made of candy"}, true,
         "Viva Piñata: TiP - In a garden", "4D53085F", 0},
        {{"Sprinklez", 0x0009000000000002, 3, 18760, ""}, true, "Online", "", 0},
        {{"GooberGardener", 0x0009000000000003, 6, 9020, "Seedos for days"}, false, "", "", now - 3 * 3600},
        {{"Hudsonhorstachio", 0x0009000000000004, 7, 66310, ""}, false, "", "", now - 9 * 86400},
    };
    requests_ = {{{"Fizzlybear", 0x0009000000000005, 2, 5150, ""}, now - 1800}};
    notifications_.clear();
    messages_ = {
        {2, "PinataPal", 1, false, "Come see my new Bunnycombs!", now - 600, true},
        {1, "PinataPal", 1, true, "Want to trade crates later?", now - 4000, false},
        {0, "Sprinklez", 3, false, "gg", now - 2 * 86400, false},
    };
    Push(NotificationKind::kInvite, "PinataPal", 1, "invited you to their garden", false);
    Push(NotificationKind::kCrate, "GooberGardener", 6, "sent you a crate", false);
    Push(NotificationKind::kFriendRequest, "Fizzlybear", 2, "wants to be your friend", false);
    // A friend signing in a little later, to show the toast.
    Later(20.0, [this] {
      for (auto& f : friends_) {
        if (f.profile.gamertag == "GooberGardener") {
          f.online = true;
          f.status = "Viva Piñata: TiP - Main menu";
          Push(NotificationKind::kFriendOnline, f.profile.gamertag, f.profile.gamerpic,
               "is now online");
        }
      }
    });
  }

  void Push(NotificationKind kind, const std::string& from, int gamerpic, const std::string& text,
            bool toast = true) {
    Notification n{next_id_++, kind, from, 0, gamerpic, text, Now(), true, ""};
    notifications_.insert(notifications_.begin(), n);
    if (toast && on_notification) on_notification(n);
  }

  void Remember(const Profile& profile) {
    remembered_ = profile;
    std::error_code ec;
    std::filesystem::create_directories(account_path_.parent_path(), ec);
    std::ofstream file(account_path_, std::ios::trunc);
    file << profile.gamertag << '\n' << profile.gamerpic << '\n' << profile.gamerscore << '\n'
         << profile.motto << '\n';
  }

  void LoadRemembered() {
    std::ifstream file(account_path_);
    if (!file) return;
    std::string pic, score;
    std::getline(file, remembered_.gamertag);
    std::getline(file, pic);
    std::getline(file, score);
    std::getline(file, remembered_.motto);
    remembered_.gamerpic = std::atoi(pic.c_str());
    remembered_.gamerscore = static_cast<uint32_t>(std::strtoul(score.c_str(), nullptr, 10));
  }

  std::filesystem::path account_path_;
  Profile remembered_;
  Profile me_;
  bool signed_in_ = false;
  std::vector<Friend> friends_;
  std::vector<FriendRequest> requests_;
  std::vector<Notification> notifications_;
  std::vector<Message> messages_;
  std::vector<ServerTheme> themes_;
  uint64_t next_message_ = 100;
  std::vector<Pending> pending_;
  uint64_t next_id_ = 1;
  double now_ = 0.0;
};

}  // namespace

std::unique_ptr<Backend> CreateMockBackend(std::filesystem::path data_dir) {
  return std::make_unique<MockBackend>(std::move(data_dir));
}

}  // namespace rexglue_guide
