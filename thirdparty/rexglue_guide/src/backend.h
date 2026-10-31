#pragma once
// What the Guide shows and does, independent of where it comes from. The
// server backend talks to RexLiveless; the mock backend is canned data for
// working on the UI without a server.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rexglue_guide {

// Gamerpic numbers from here up are pictures uploaded to the server.
constexpr int kFirstServerGamerpic = 1000;

struct Profile {
  std::string gamertag;
  uint64_t xuid = 0;
  int gamerpic = 0;  // 0..8 embedded, kFirstServerGamerpic+ uploaded to the server
  uint32_t gamerscore = 0;
  std::string motto;
  // The systems they've played on, from the first: "windows", "linux",
  // "macos", "android", "ios", "switch", "playstation".
  std::vector<std::string> platforms;
};

// The system this Guide is running on, as Profile::platforms names it.
const char* ThisPlatform();

struct Friend {
  Profile profile;
  bool online = false;
  std::string status;     // presence text while online
  std::string title_id;   // title they're playing, hex, while online
  int64_t last_seen = 0;  // unix seconds
};

struct FriendRequest {
  Profile from;
  int64_t time = 0;
};

enum class NotificationKind { kInvite, kCrate, kBlueprint, kFriendRequest, kFriendOnline, kMessage, kSystem };

struct Notification {
  uint64_t id = 0;
  NotificationKind kind = NotificationKind::kSystem;
  std::string from;  // gamertag, or empty for system messages
  uint64_t from_xuid = 0;
  int from_gamerpic = 0;
  std::string text;
  int64_t time = 0;
  bool unread = true;
  std::string session_id;  // invites: hex XNKID of the host's session
};

struct Message {
  uint64_t id = 0;
  std::string with;  // the other person's gamertag
  int with_gamerpic = 0;
  bool sent = false;  // true: from us
  std::string text;
  int64_t time = 0;
  bool unread = false;  // received and not read yet
};

// A theme made in the Theme Designer (themes.h has what's in `data`).
struct ServerTheme {
  int id = 0;
  std::string name;
  std::string author;
  uint64_t author_xuid = 0;
  std::string data;  // JSON: {light, dark, image_alpha, image_blur}
  bool has_image = false;
  bool nsfw = false;
  int likes = 0;
  int users = 0;      // players with it on
  bool liked = false;  // by this player
  int64_t created = 0;
  int64_t updated = 0;
};

struct Result {
  bool ok = true;
  std::string error;
  std::string note;  // succeeded, with something the player should know
};
using Callback = std::function<void(const Result&)>;

struct AchievementRecord {
  uint32_t id = 0;
  uint32_t gamerscore = 0;
  std::string label;
  int64_t unlocked = 0;  // unix seconds
  int64_t icon = 0;      // on a profile: its icon's version on the server, 0 if it has none
};

// A game as the server lists it: what the games report, with what admins set
// over that (a name, an icon, leaderboard names).
struct TitleInfo {
  std::string title_id;  // hex
  std::string name;
  int64_t icon = 0;  // the icon's version; 0: none
  std::map<uint32_t, std::string> boards;  // leaderboards to list, by view id
  std::vector<uint32_t> hidden_boards;     // ones an admin took off the list
  uint32_t achievements_total = 0;
  uint32_t gamerscore_total = 0;
};

// One game on a player's profile, with what they have of it.
struct PlayedGame {
  std::string title_id;
  std::string name;
  int64_t icon = 0;
  int64_t last_played = 0;
  uint32_t achievements = 0, achievements_total = 0;
  uint32_t gamerscore = 0, gamerscore_total = 0;
};

// A player's place on one of a game's leaderboards.
struct BoardPlace {
  uint32_t id = 0;
  std::string name;
  uint32_t rank = 0, total = 0;
  double rating = 0.0;
};

// A game in the gamerpic archive (xboxgamer.pics).
struct GamerpicGame {
  std::string title_id;  // lowercase hex, as the archive has it
  std::string name;
  int count = 0;
};

struct PlayedGameDetail {
  std::vector<AchievementRecord> achievements;  // unlocked, newest first
  std::vector<BoardPlace> boards;
};

// Copy of the signed-in state for code running on game threads.
struct LiveSnapshot {
  bool signed_in = false;
  bool connected = false;
  Profile me;
  std::vector<Friend> friends;
  std::string public_ip;
  std::string token;        // for server calls made from game threads
  std::string server_host;  // RexLiveless server
  uint16_t server_port = 0;
  bool server_tls = false;
  uint16_t xts_port = 0;    // title server (crates), on server_host
  uint16_t relay_port = 0;  // UDP relay for game traffic, on server_host
  std::string relay_key;    // 16 bytes, hex: authorises relay registrations
};

class Backend {
 public:
  virtual ~Backend() = default;

  // Finishes pending requests and raises events. Called every frame.
  virtual void Update(double now) = 0;

  // Account
  virtual bool signed_in() const = 0;
  virtual const Profile& me() const = 0;
  virtual std::string remembered_gamertag() const = 0;  // empty if none
  virtual int remembered_gamerpic() const = 0;
  virtual void CreateAccount(const std::string& gamertag, const std::string& password,
                             int gamerpic, Callback done) = 0;
  virtual void SignIn(const std::string& gamertag, const std::string& password,
                      Callback done) = 0;
  virtual void SignInRemembered(Callback done) = 0;
  virtual void SignOut() = 0;
  virtual void SetGamerpic(int gamerpic) = 0;
  virtual void SetMotto(const std::string& motto) = 0;

  // Social
  virtual const std::vector<Friend>& friends() const = 0;
  virtual const std::vector<FriendRequest>& friend_requests() const = 0;
  virtual void SendFriendRequest(const std::string& gamertag, Callback done) = 0;
  virtual void AnswerFriendRequest(const std::string& gamertag, bool accept) = 0;
  virtual void RemoveFriend(const std::string& gamertag) = 0;
  virtual void SendInvite(const std::string& gamertag, const std::string& session_id, Callback done) = 0;

  // Notifications, newest first
  virtual const std::vector<Notification>& notifications() const = 0;
  virtual void MarkRead(uint64_t id) = 0;
  virtual void Dismiss(uint64_t id) = 0;

  // Messages with friends, newest first
  virtual const std::vector<Message>& messages() const = 0;
  virtual void SendTextMessage(const std::string& gamertag, const std::string& text, Callback done) = 0;
  virtual void MarkConversationRead(const std::string& gamertag) = 0;

  // Gamerpics to choose from: the embedded ones, then any on the server.
  virtual std::vector<int> gamerpic_choices() const = 0;
  // PNG of a server gamerpic, or empty while it downloads.
  virtual std::string ServerGamerpic(int gamerpic) = 0;
  // Makes a 256x256 PNG the account's own photo and its gamerpic.
  virtual void UploadPhoto(std::string png, Callback done) = 0;

  // The Gamerpic Database (the xboxgamer.pics archive). Its games, by name: null
  // while the list loads (asking starts it), with *error set if it couldn't.
  virtual const std::vector<GamerpicGame>* gamerpic_games(std::string* error) = 0;
  // One game's gamerpic ids.
  virtual void GameGamerpics(const std::string& title_id,
                             std::function<void(const Result&, std::vector<std::string>)> done) = 0;
  // A game gamerpic's PNG, or empty while it downloads; *missing: the archive
  // hasn't got it (it isn't coming). Ask every frame it's shown: the newest
  // asked for download first, and one not asked for lately (scrolled away)
  // isn't downloaded. fetch false: only one that's here already.
  virtual std::string GameGamerpicImage(const std::string& title_id, const std::string& id, bool* missing = nullptr,
                                        bool fetch = true) = 0;
  // Puts one on the profile (it becomes a server gamerpic number).
  virtual void SetGameGamerpic(const std::string& title_id, const std::string& id, Callback done) = 0;

  // Themes everyone made, newest first; listed without signing in.
  virtual const std::vector<ServerTheme>& themes() const = 0;
  // A list has come from the server (themes() may still be empty).
  virtual bool themes_ready() const = 0;
  // Themes the account may own: -1 any number, 0 none.
  virtual int theme_quota() const = 0;
  // Makes a theme (id 0) or changes one; `done` gets its id.
  virtual void SaveTheme(int id, const std::string& name, const std::string& data, bool nsfw,
                         std::function<void(const Result&, int id)> done) = 0;
  virtual void LikeTheme(int id, bool like, Callback done) = 0;
  // The server theme the player has on (0: none), for its user count.
  virtual void SetThemeInUse(int id) = 0;
  // A theme's background picture (JPEG or PNG); empty removes it.
  virtual void SetThemeImage(int id, std::string image, Callback done) = 0;
  virtual void DeleteTheme(int id, Callback done) = 0;
  // A theme's background picture file, or empty while it downloads.
  virtual std::string ThemeImage(int id, int64_t updated) = 0;

  // The game being played, for presence; hex title id.
  virtual void SetTitle(const std::string& title_id, const std::string& presence) = 0;
  // Unlocked achievements of the current title; sent once each.
  virtual void ReportAchievements(const std::string& title_id,
                                  const std::vector<AchievementRecord>& unlocked) = 0;

  // Games. The server's listing of one, or null if it has none (yet).
  virtual const TitleInfo* title(const std::string& title_id) const = 0;
  // A game's icon file (PNG or JPEG) at `version`, or empty while it downloads.
  virtual std::string TitleIcon(const std::string& title_id, int64_t version) = 0;
  // What the Guide knows of the game it runs in; sent once a session.
  virtual void ReportTitleInfo(const std::string& title_id, const std::string& name, uint32_t achievements_total,
                               uint32_t gamerscore_total, const std::map<uint32_t, std::string>& boards) = 0;
  // A player's games, most recently played first.
  virtual void PlayerGames(const std::string& gamertag,
                           std::function<void(const Result&, std::vector<PlayedGame>)> done) = 0;
  // A player's achievements and leaderboard places in one game.
  virtual void PlayerGame(const std::string& gamertag, const std::string& title_id,
                          std::function<void(const Result&, PlayedGameDetail)> done) = 0;

  // Achievement icons, for profiles: an admin's Guide uploads the running
  // game's that the server hasn't got. The account is an admin.
  virtual bool is_admin() const = 0;
  // The ids of a game's achievements whose icon the server has.
  virtual void AchievementIconIds(const std::string& title_id,
                                  std::function<void(const Result&, std::vector<uint32_t>)> done) = 0;
  virtual void UploadAchievementIcon(const std::string& title_id, uint32_t id, std::string image, Callback done) = 0;
  // An icon's file at `version` (AchievementRecord::icon), or empty while it downloads.
  virtual std::string AchievementIconImage(const std::string& title_id, uint32_t id, int64_t version) = 0;

  virtual bool connected() const = 0;
  virtual std::string server_name() const = 0;
  // Empty when talking to the RexLiveless server; otherwise the server it is
  // talking to instead (a local one, say), which the Guide warns about.
  virtual std::string unofficial_server() const = 0;
  virtual LiveSnapshot Snapshot() const = 0;

  // Raised from Update() for anything that should pop a toast.
  std::function<void(const Notification&)> on_notification;
};

std::unique_ptr<Backend> CreateMockBackend(std::filesystem::path data_dir);
std::unique_ptr<Backend> CreateServerBackend(std::filesystem::path data_dir);

// Gamertag rules from the 360: 1-15 characters, letters, digits and single
// spaces, starting with a letter. Returns an error message, or empty if valid.
std::string ValidateGamertag(const std::string& gamertag);

}  // namespace rexglue_guide
