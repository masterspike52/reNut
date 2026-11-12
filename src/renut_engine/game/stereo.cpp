#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "miniaudio.h"

#include "renut_engine/app/app_paths.h"
#include "renut_engine/game/banner.h"
#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/custom_worlds.h"
#include "renut_engine/game/stereo.h"

REX_EXTERN(__imp__sub_8261FE38);  // Stereo: button down
REX_EXTERN(__imp__sub_8261FEA8);  // Stereo: button up
REX_EXTERN(__imp__sub_8261FB60);  // Stereo: tick
REX_EXTERN(__imp__sub_82620128);  // Stereo: turns off
REX_EXTERN(__imp__sub_8261FA98);  // Stereo: destroy
REX_EXTERN(__imp__sub_82620008);  // Stereo: plays its track (part, vehicle)
REX_EXTERN(__imp__sub_8261C7C8);  // a part: starts an entry's cue in a slot
REX_EXTERN(__imp__sub_8261C888);  // a part: stops a sound slot's cue
REX_EXTERN(__imp__sub_82211450);  // a sound slot: whether its cue plays
REX_EXTERN(__imp__sub_82261608);  // starts a cue (..., double volume)
REX_EXTERN(__imp__sub_82B610B0);  // a wavebank: wanted (want, keep)
REX_EXTERN(__imp__sub_82B4D518);  // the audio system: reads the cue list
REX_EXTERN(__imp__rex_XACT__CEngine__Pause);
REX_EXTERN(__imp__rex_audioMainPlayHash);  // (&cue, , workspace, double volume): a 2D cue
REX_EXTERN(__imp__sub_8239F790);       // a cue record: stops its cue (record, now, )

namespace {

using Clock = std::chrono::steady_clock;

// How loud the Stereo plays the player's own music.
constexpr float kOwnMusicVolume = 0.8f;

// The Stereo part's fields (vehicleBlockStereo).
constexpr uint32_t kSoundSlot = 1152;
constexpr uint32_t kTrack = 1156;
constexpr uint32_t kOn = 1160;
constexpr uint32_t kTracks = 1164;
constexpr uint32_t kHeld = 1168;
constexpr uint32_t kHandled = 1176;
// A part's sound slots: vector {u16 slot size, ..., + 4 first}; a slot's + 0
// its cue handle.
constexpr uint32_t kPartSlots = 816;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// The wavebank manager's list and an AudioWavebank_c's fields.
constexpr uint32_t kWavebankManager = 0x82DC1448;
constexpr uint32_t kWavebanksFirst = 72;
constexpr uint32_t kWavebanksEnd = 76;
constexpr uint32_t kWavebankId = 32;
constexpr uint32_t kWavebankWanted = 44;
constexpr uint32_t kWavebankState = 48;
constexpr uint32_t kWavebankFailed = 1;
constexpr uint32_t kWavebankPrepared = 6;
constexpr uint32_t kWavebankKept = 2;
// The vehicle's rigid body and its position (part_scripts.cpp).
constexpr uint32_t kVehicleBody = 0x7C0;
constexpr uint32_t kBodyPosition = 0x110;
// The camera's world matrix (meCalculateView, 0x822BB8F0: the inverse of the
// view at 0x82E1B450, rows right, up, forward, position).
constexpr uint32_t kCameraRight = 0x82E1B4D0;
constexpr uint32_t kCameraPosition = 0x82E1B500;

constexpr uint32_t kRadioClickClockWood = 0x7EE19746;
constexpr uint32_t kRadioFreezeezyPeak = 0xCDC00E78;
// The game's other songs: music cues and the wavebanks each plays from; music
// cues have no 3D distances, so the Stereo plays them without a position.
constexpr size_t kMaxSongWavebanks = 3;
struct GameSong {
  uint32_t cue;
  const char *xsb_name;  // its name in the "Music" soundbank
  uint32_t wavebanks[kMaxSongWavebanks];  // 0: none
  const char *title;
};
constexpr uint32_t kRaceFanfares = 0x12D24BF8;  // MusicRaceFanfares
constexpr uint32_t kPreRaceDrums = 0x1225F73D;  // MusicPreRaceDrums
constexpr GameSong kGameSongs[] = {
    {0x1BFBF80D, "MainTheme", {0x1242B3DC}, "Main Theme (Rawk 'n Roll)"},                          // MusicMainTheme
    {0x05002046, "MumbosMoutain", {0x12A81462}, "Mumbo's Mountain"},                               // MusicMumbosMountain
    // Interactive cues: variable 1 is the race song (0 the drum roll, 2 and 3
    // the win and lose fanfares).
    {0xC216EA2D, "Spiral_Challenge", {0x128D8F6C, kRaceFanfares, kPreRaceDrums}, "Spiral Challenge"},  // MusicSpiralChallenge
    {0x1CE2085D, "Grunty_Final", {0x12742C94, kRaceFanfares, kPreRaceDrums}, "Grunty's Challenge"},    // MusicGruntyChallenge
};

// Challenges that play a game song while they run (ChallengeMusicTick).
struct ChallengeSong {
  uint32_t challenge;  // its asset id
  uint32_t cue;        // a kGameSongs cue
};
constexpr ChallengeSong kChallengeSongs[] = {
    {0x3DF04CE0, 0xC216EA2D},  // aid_challenge_banjox_spiralmountain_sixofthebest: Spiral Challenge
    {0x3D07E839, 0x1CE2085D},  // aid_challenge_banjox_spiralmountain_showdown: Grunty's Challenge
};
// A cue record (as a part's sound slot's): {cue handle, -1, -1, 0}, then the
// cue id audioMainPlayHash reads.
constexpr uint32_t kCueRecordSize = 20;
constexpr uint32_t kCueRecordId = 16;
constexpr uint32_t kNoWorkspace = 0;  // audioMainPlayHash: no world's

// XACT categories: level music is under music_normal, paused while a game song
// plays; the songs' sounds are moved to music_radio so they keep playing.
constexpr uint16_t kCategoryMain = 26, kCategoryRace = 27, kCategoryOthers = 28, kCategoryFanfare = 152;
constexpr uint16_t kCategoryMusicRadio = 16, kCategoryMusicNormal = 17;
// The audio system (sub_82222610) and its IXACTEngine (R1 InitXACT).
constexpr uint32_t kAudioSystem = 0x82DC13A0;
constexpr uint32_t kAudioEngine = 4;
// The cue list (sub_82B4D518's r4): + 24 soundbank count, + 28 soundbanks
// (24 bytes: {size, XACT soundbank, ...}).
constexpr uint32_t kCueListSoundbankCount = 24;
constexpr uint32_t kCueListSoundbanks = 28;
constexpr uint32_t kSoundbankEntrySize = 24;
// An XACT soundbank (v43, big-endian): header offsets, cue and variation entry
// sizes, and a sound's category (+ 1, u16) and volume (+ 3, u8).
constexpr uint32_t kSoundbankMagic = 0x4B424453;
constexpr uint32_t kXsbSimpleCount = 0x13, kXsbComplexCount = 0x15;
constexpr uint32_t kXsbSimpleCues = 0x22, kXsbComplexCues = 0x26, kXsbCueNames = 0x2A, kXsbName = 0x4A;
constexpr uint32_t kSimpleCueSize = 5, kComplexCueSize = 15, kVariationSize = 16;
constexpr uint32_t kSoundCategory = 1, kSoundVolume = 3;
constexpr const char *kRadioXsbName = "Radio_ClickClockWood";

// Two presses this close are a double press.
constexpr auto kDoublePress = std::chrono::milliseconds(300);
// A cue not playing this long after it started has ended (or never started).
constexpr auto kStartGrace = std::chrono::seconds(2);
// The silent radio song under a file starts again this often.
constexpr auto kSilentRefresh = std::chrono::seconds(60);
// A game song's wavebank not prepared by then: the song is left out.
constexpr auto kWavebankWait = std::chrono::seconds(15);
// Music stops while its Stereo isn't updated this long, and is let go after
// the longer.
constexpr auto kPauseAfter = std::chrono::milliseconds(250);
constexpr auto kForgetAfter = std::chrono::seconds(30);
// Full volume within kNear of the camera, silent from kFar.
constexpr float kNear = 6.0f;
constexpr float kFar = 70.0f;

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

uint16_t Load16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

void Store16(uint8_t *base, uint32_t va, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 2);
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

Vec3 LoadVec3(const uint8_t *base, uint32_t va) {
  return {LoadFloat(base, va), LoadFloat(base, va + 4), LoadFloat(base, va + 8)};
}

// sub_82261608 starts its cue at this volume when >= 0 (game thread).
thread_local double t_cue_volume = -1.0;

// --- The Stereos ---------------------------------------------------------------

enum class Kind { Radio, Game, File };

// What the playlist holds; two quick presses go to the next.
enum class Source { All, Game, Yours };

struct Track {
  Kind kind = Kind::Radio;
  uint32_t cue = 0;     // Radio, Game
  uint32_t second = 0;  // the entry's second word (Radio: the game's)
  const GameSong *song = nullptr;
  std::filesystem::path file;
  std::string title;
};

// A radio entry {cue, second} from the Stereo's own list.
struct RadioEntry {
  uint32_t cue = 0, second = 0;
};

// A game song's wavebanks reNut wanted (owned; those the game already wanted
// aren't).
struct HeldWavebanks {
  const GameSong *song = nullptr;
  std::vector<uint32_t> owned;
};

struct Stereo {
  bool ready = false;
  std::vector<RadioEntry> radio;  // the Stereo's list as the game made it
  std::vector<Track> playlist;
  std::unordered_set<uint32_t> unavailable;  // game songs that didn't play here
  size_t index = 0;
  bool pending_next = false;  // a press waiting to see whether a second follows
  Clock::time_point pending_since{};
  // The Stereo's cue: when it started, whether it was seen playing.
  Clock::time_point started{};
  bool seen_playing = false;
  // A game song waiting for its wavebanks.
  bool waiting = false;
  Clock::time_point waiting_since{};
  HeldWavebanks held;
  std::unique_ptr<ma_sound> sound;  // a file playing
  bool paused = false;
  Clock::time_point last_tick{};
  bool holds_music = false;  // a game song plays: the level's music paused
};

// A challenge's song (kChallengeSongs).
struct ChallengeMusic {
  const GameSong *song = nullptr;
  HeldWavebanks held;
  uint32_t record = 0;  // guest cue record (kCueRecordSize)
  Clock::time_point since{};  // the song chosen, or its cue started
  bool started = false;
  bool holds_music = false;  // its cue plays: the level's music paused
};

// A mod world's music (renut.worlds.add's music), looping while the world is played.
struct WorldMusic {
  std::filesystem::path file;
  std::unique_ptr<ma_sound> sound;
  bool paused = false;
  bool failed = false;  // the file wouldn't play: not tried again in this world
  Clock::time_point last_tick{};
  bool holds_music = false;  // it plays: the level's music paused
};

// A sound file played once (PlayOneShot: an NPC's voice clip).
struct OneShot {
  std::unique_ptr<ma_sound> sound;
  bool paused = false;
};

// A mod's sound (PlayModSound): placed from the camera each tick when it has a position.
struct ModSound {
  std::unique_ptr<ma_sound> sound;
  bool paused = false;
  bool loop = false;
  float volume = 1.0f;
  bool positional = false;
  Vec3 at;
  float range = 70.0f;  // silent this far from the camera
};

std::mutex g_mutex;  // the stereos, the source, the engine, the challenge's song, the world's music, the one-shots
std::unordered_map<uint32_t, Stereo> g_stereos;
ChallengeMusic g_challenge;
WorldMusic g_world_music;
std::unordered_map<uint32_t, OneShot> g_one_shots;  // by channel
std::unordered_map<uint32_t, ModSound> g_mod_sounds;  // by handle
uint32_t g_next_mod_sound = 1;
Clock::time_point g_one_shots_tick{};
uint32_t g_npc_scene = 0;  // the scene NpcSoundsTick last saw
HeldWavebanks g_voice_wavebanks;  // the NPCs' game voices' dialog wavebanks reNut wanted
std::vector<uint32_t> g_voice_wavebanks_seen;  // those looked at in this scene
bool g_music_paused = false;  // reNut paused music_normal
std::filesystem::path g_music_override;  // renut.sound.music's file, under g_mutex; empty for none
float g_music_override_volume = 1.0f;
Source g_source = Source::All;
ma_engine g_engine;
bool g_engine_ready = false, g_engine_failed = false;
std::atomic<bool> g_watchdog_started{false};

// The Stereo's state, its list read before reNut rewrites its first entry.
Stereo &StereoOf(uint8_t *base, uint32_t part) {
  Stereo &s = g_stereos[part];
  if (s.ready)
    return s;
  s.ready = true;
  if (const uint32_t tracks = Load32(base, part + kTracks)) {
    const uint32_t entry_size = Load16(base, tracks);
    const uint32_t first = Load32(base, tracks + 4), end = Load32(base, tracks + 8);
    std::unordered_set<uint32_t> seen;
    for (uint32_t i = 0; entry_size >= 8 && first + (i + 1) * entry_size <= end && i < 64; ++i) {
      const uint32_t at = first + i * entry_size;
      const uint32_t cue = Load32(base, at);
      if (cue && seen.insert(cue).second)
        s.radio.push_back({cue, Load32(base, at + 4)});
    }
  }
  if (s.radio.empty())
    s.radio.push_back({kRadioFreezeezyPeak, 0});
  return s;
}

std::filesystem::path MusicFolder() {
  return renut::paths::WritableFolder() / "music";
}

bool IsMusicFile(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".mp3" || extension == ".ogg" || extension == ".oga" || extension == ".flac" ||
         extension == ".wav";
}

std::string FileTitle(const std::filesystem::path &file) {
  const std::u8string stem = file.stem().u8string();
  return std::string(stem.begin(), stem.end());
}

std::string RadioTitle(uint32_t cue, size_t number) {
  if (cue == kRadioClickClockWood)
    return "Click Clock Wood (Radio)";
  if (cue == kRadioFreezeezyPeak)
    return "Freezeezy Peak (Radio)";
  return "Radio song " + std::to_string(number + 1);
}

const char *SourceName(Source source) {
  switch (source) {
  case Source::Game:
    return "Nuts & Bolts music";
  case Source::Yours:
    return "your music";
  default:
    return "all music";
  }
}

// The source's tracks: the radio songs and the game's songs (but those that
// didn't play here), the music folder's files.
std::vector<Track> BuildPlaylist(const Stereo &s, Source source) {
  std::vector<Track> playlist;
  if (source != Source::Yours) {
    for (const RadioEntry &radio : s.radio)
      playlist.push_back({Kind::Radio, radio.cue, radio.second, 0, {}, RadioTitle(radio.cue, playlist.size())});
    for (const GameSong &song : kGameSongs) {
      if (!s.unavailable.contains(song.cue))
        playlist.push_back({Kind::Game, song.cue, 0, &song, {}, song.title});
    }
  }
  if (source != Source::Game) {
    std::error_code ec;
    const std::filesystem::path folder = MusicFolder();
    std::filesystem::create_directories(folder, ec);
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec) && IsMusicFile(it->path()))
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto &file : files)
      playlist.push_back({Kind::File, 0, 0, 0, file, FileTitle(file)});
  }
  return playlist;
}

// --- The player's files (miniaudio) ------------------------------------------

void StopFile(Stereo &s) {
  if (!s.sound)
    return;
  ma_sound_stop(s.sound.get());
  ma_sound_uninit(s.sound.get());
  s.sound.reset();
  s.paused = false;
}

// Pauses the files of stereos the game stopped updating, and lets go of those
// gone. Requires g_mutex.
void WatchStereos() {
  const auto now = Clock::now();
  for (auto it = g_stereos.begin(); it != g_stereos.end();) {
    Stereo &s = it->second;
    const auto idle = now - s.last_tick;
    if (idle > kForgetAfter) {
      StopFile(s);
      it = g_stereos.erase(it);
      continue;
    }
    if (s.sound && !s.paused && idle > kPauseAfter) {
      ma_sound_stop(s.sound.get());
      s.paused = true;
    }
    ++it;
  }
  // A world's music pauses with the game (its ticks stop), as the Stereos' files do.
  WorldMusic &m = g_world_music;
  if (m.sound && !m.paused && now - m.last_tick > kPauseAfter) {
    ma_sound_stop(m.sound.get());
    m.paused = true;
  }
  if (now - g_one_shots_tick > kPauseAfter) {
    for (auto &[channel, shot] : g_one_shots) {
      if (shot.sound && !shot.paused) {
        ma_sound_stop(shot.sound.get());
        shot.paused = true;
      }
    }
    for (auto &[handle, mod] : g_mod_sounds) {
      if (mod.sound && !mod.paused) {
        ma_sound_stop(mod.sound.get());
        mod.paused = true;
      }
    }
  }
}

bool EnsureEngine() {
  if (g_engine_ready)
    return true;
  if (g_engine_failed)
    return false;
  ma_engine_config config = ma_engine_config_init();
  // No sound device for your own music (miniaudio): the game's songs still play.
  if (ma_engine_init(&config, &g_engine) != MA_SUCCESS) {
    g_engine_failed = true;
    return false;
  }
  g_engine_ready = true;
  if (!g_watchdog_started.exchange(true)) {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(g_mutex);
        WatchStereos();
      }
    }).detach();
  }
  return true;
}

// Starts a file from its beginning. Requires g_mutex.
bool PlayFile(Stereo &s, const std::filesystem::path &file) {
  StopFile(s);
  if (!EnsureEngine())
    return false;
  auto sound = std::make_unique<ma_sound>();
  const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
  const ma_result result = ma_sound_init_from_file_w(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#else
  const ma_result result = ma_sound_init_from_file(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
  if (result != MA_SUCCESS)
    return false;
  ma_sound_set_volume(sound.get(), 0.0f);  // until it is placed
  ma_sound_start(sound.get());
  s.sound = std::move(sound);
  s.paused = false;
  return true;
}

// The file's loudness and side from where the vehicle is to the camera.
void PlaceFile(const uint8_t *base, Stereo &s, uint32_t vehicle) {
  const uint32_t body = vehicle ? Load32(base, vehicle + kVehicleBody) : 0;
  if (!s.sound || !body)
    return;
  const Vec3 at = LoadVec3(base, body + kBodyPosition);
  const Vec3 camera = LoadVec3(base, kCameraPosition);
  const Vec3 right = LoadVec3(base, kCameraRight);
  const Vec3 d{at.x - camera.x, at.y - camera.y, at.z - camera.z};
  const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!std::isfinite(distance))
    return;
  float gain = std::clamp((kFar - distance) / (kFar - kNear), 0.0f, 1.0f);
  gain *= gain;
  float pan = 0.0f;
  if (distance > 0.5f)
    pan = std::clamp((d.x * right.x + d.y * right.y + d.z * right.z) / distance * 0.8f, -0.8f, 0.8f);
  ma_sound_set_volume(s.sound.get(), gain * kOwnMusicVolume);
  ma_sound_set_pan(s.sound.get(), pan);
}

// --- The game's cues and wavebanks ------------------------------------------------

void StopSlot(PPCContext &ctx, uint8_t *base, uint32_t part);

// Plays 'cue' at 'volume' (< 0: the game's) in the Stereo's slot through its
// first track entry, at the vehicle or without a position (music cues).
void PlayEntry(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle, uint32_t cue,
               uint32_t second, double volume, bool positional = true) {
  const uint32_t tracks = Load32(base, part + kTracks);
  const uint32_t first = tracks ? Load32(base, tracks + 4) : 0;
  if (!first)
    return;
  Store32(base, first, cue);
  Store32(base, first + 4, second);
  Store32(base, part + kTrack, 0);
  const double outer = std::exchange(t_cue_volume, volume);
  if (positional) {
    ctx.r3.u64 = part;
    ctx.r4.u64 = vehicle;
    __imp__sub_82620008(ctx, base);
  } else {
    StopSlot(ctx, base, part);
    ctx.r3.u64 = part;
    ctx.r4.u64 = first;
    ctx.r5.u64 = 0;  // no position
    ctx.r6.u64 = 0;
    ctx.r7.u64 = part + kSoundSlot;
    __imp__sub_8261C7C8(ctx, base);
  }
  t_cue_volume = outer;
  s.started = Clock::now();
  s.seen_playing = false;
}

// The silent radio song (under a file, a game song's wavebank loading).
void PlaySilent(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle) {
  PlayEntry(ctx, base, s, part, vehicle, s.radio.front().cue, s.radio.front().second, 0.0);
}

void StopSlot(PPCContext &ctx, uint8_t *base, uint32_t part) {
  if (Load32(base, part + kSoundSlot) == kNoSlot)
    return;
  ctx.r3.u64 = part;
  ctx.r4.u64 = part + kSoundSlot;
  __imp__sub_8261C888(ctx, base);
}

// Whether the Stereo's slot's cue plays.
bool SlotPlaying(PPCContext &ctx, uint8_t *base, uint32_t part) {
  const uint32_t slot = Load32(base, part + kSoundSlot);
  const uint32_t slots = Load32(base, part + kPartSlots);
  if (slot == kNoSlot || !slots)
    return false;
  const uint32_t record = Load16(base, slots) * slot + Load32(base, slots + 4);
  if (!record || !Load32(base, record))
    return false;
  ctx.r3.u64 = record;
  __imp__sub_82211450(ctx, base);
  return (ctx.r3.u32 & 0xFF) != 0;
}

// A wavebank by its asset id, or 0.
uint32_t FindWavebank(const uint8_t *base, uint32_t id) {
  const uint32_t manager = Load32(base, kWavebankManager);
  if (!manager)
    return 0;
  const uint32_t first = Load32(base, manager + kWavebanksFirst);
  const uint32_t end = Load32(base, manager + kWavebanksEnd);
  for (uint32_t at = first; at && at < end && at - first < 4 * 1024; at += 4) {
    const uint32_t wavebank = Load32(base, at);
    if (wavebank && Load32(base, wavebank + kWavebankId) == id)
      return wavebank;
  }
  return 0;
}

// A game song's wavebanks: all prepared (kWavebankPrepared), one failed or
// missing (kWavebankFailed), or still loading (0).
uint32_t SongState(const uint8_t *base, const GameSong &song) {
  bool prepared = true;
  for (const uint32_t id : song.wavebanks) {
    if (!id)
      continue;
    const uint32_t wavebank = FindWavebank(base, id);
    const uint32_t state = wavebank ? Load32(base, wavebank + kWavebankState) : kWavebankFailed;
    if (state == kWavebankFailed)
      return kWavebankFailed;
    prepared &= state == kWavebankPrepared;
  }
  return prepared ? kWavebankPrepared : 0;
}

void SetWanted(PPCContext &ctx, uint8_t *base, uint32_t wavebank, bool wanted) {
  ctx.r3.u64 = wavebank;
  ctx.r4.u64 = wanted;
  ctx.r5.u64 = wanted;
  __imp__sub_82B610B0(ctx, base);
}

// Lets go of the wavebanks reNut wanted (their cue stopped first), but those
// the game wants now.
void ReleaseWavebanks(PPCContext &ctx, uint8_t *base, HeldWavebanks &held) {
  for (const uint32_t id : held.owned) {
    const uint32_t wavebank = FindWavebank(base, id);
    if (wavebank && Load32(base, wavebank + kWavebankWanted) == kWavebankKept)
      SetWanted(ctx, base, wavebank, false);
  }
  held.owned.clear();
  held.song = nullptr;
}

// Wants a game song's wavebanks (kept until released) that the game doesn't.
// False when one is missing.
bool WantWavebanks(PPCContext &ctx, uint8_t *base, HeldWavebanks &held, const GameSong &song) {
  if (held.song == &song)
    return true;
  ReleaseWavebanks(ctx, base, held);
  for (const uint32_t id : song.wavebanks) {
    if (id && !FindWavebank(base, id))
      return false;
  }
  for (const uint32_t id : song.wavebanks) {
    const uint32_t wavebank = id ? FindWavebank(base, id) : 0;
    if (wavebank && !Load32(base, wavebank + kWavebankWanted)) {
      SetWanted(ctx, base, wavebank, true);
      held.owned.push_back(id);
    }
  }
  held.song = &song;
  return true;
}

// --- The level's music --------------------------------------------------------

// main, race (and its fanfare), others: music_normal's.
bool IsLevelMusicCategory(uint16_t category) {
  return category == kCategoryMain || category == kCategoryRace || category == kCategoryOthers ||
         category == kCategoryFanfare;
}

// Moves the songs' sounds in the "Music" soundbank to music_radio, and matches
// their loudest sound to Radio_ClickClockWood's volume.
void MoveSongsToRadio(uint8_t *base, uint32_t cue_list) {
  const uint32_t count = Load32(base, cue_list + kCueListSoundbankCount);
  const uint32_t table = Load32(base, cue_list + kCueListSoundbanks);
  for (uint32_t i = 0; table && i < count && i < 1024; ++i) {
    const uint32_t size = Load32(base, table + i * kSoundbankEntrySize);
    const uint32_t xsb = Load32(base, table + i * kSoundbankEntrySize + 4);
    if (!xsb || size < kXsbName + 64 || Load32(base, xsb) != kSoundbankMagic ||
        std::strncmp(reinterpret_cast<const char *>(base + xsb + kXsbName), "Music", 64) != 0)
      continue;
    const uint32_t simple = Load16(base, xsb + kXsbSimpleCount);
    const uint32_t complex = Load16(base, xsb + kXsbComplexCount);
    uint32_t name = Load32(base, xsb + kXsbCueNames);
    // A cue's sounds (a variation table's, or its one).
    const auto cue_sounds = [&](uint32_t cue) {
      std::vector<uint32_t> sounds;
      if (cue < simple) {
        sounds.push_back(Load32(base, xsb + Load32(base, xsb + kXsbSimpleCues) + cue * kSimpleCueSize + 1));
      } else if (const uint32_t entry = xsb + Load32(base, xsb + kXsbComplexCues) + (cue - simple) * kComplexCueSize,
                 target = Load32(base, entry + 1);
                 base[entry] & 4) {
        sounds.push_back(target);
      } else if (target < size) {
        const uint32_t variations = Load16(base, xsb + target + 2);
        for (uint32_t v = 0; v < variations && v < 64; ++v)
          sounds.push_back(Load32(base, xsb + target + 8 + v * kVariationSize));
      }
      std::erase_if(sounds, [&](uint32_t sound) { return sound + kSoundVolume + 1 > size; });
      return sounds;
    };
    std::vector<std::vector<uint32_t>> songs;
    int radio_volume = -1;
    for (uint32_t cue = 0; name < size && cue < simple + complex; ++cue) {
      const char *text = reinterpret_cast<const char *>(base + xsb + name);
      const size_t length = strnlen(text, size - name);
      name += uint32_t(length) + 1;
      if (std::strcmp(text, kRadioXsbName) == 0) {
        if (const auto sounds = cue_sounds(cue); !sounds.empty())
          radio_volume = base[xsb + sounds.front() + kSoundVolume];
        continue;
      }
      if (std::any_of(std::begin(kGameSongs), std::end(kGameSongs),
                      [&](const GameSong &song) { return std::strcmp(song.xsb_name, text) == 0; }))
        songs.push_back(cue_sounds(cue));
    }
    for (const auto &sounds : songs) {
      int loudest = 0;
      for (const uint32_t sound : sounds)
        loudest = std::max<int>(loudest, base[xsb + sound + kSoundVolume]);
      for (const uint32_t sound : sounds) {
        if (!IsLevelMusicCategory(Load16(base, xsb + sound + kSoundCategory)))
          continue;
        Store16(base, xsb + sound + kSoundCategory, kCategoryMusicRadio);
        if (radio_volume >= 0)
          base[xsb + sound + kSoundVolume] =
              uint8_t(std::clamp(base[xsb + sound + kSoundVolume] + radio_volume - loudest, 0, 255));
      }
    }
    return;
  }
  // No Music soundbank in the cue list: the level's music plays under game songs.
}

// Pauses (or resumes) the level's music: XACT's music_normal category.
void PauseLevelMusic(PPCContext &ctx, uint8_t *base, bool pause) {
  const uint32_t system = Load32(base, kAudioSystem);
  const uint32_t engine = system ? Load32(base, system + kAudioEngine) : 0;
  if (!engine)
    return;
  ctx.r3.u64 = engine;
  ctx.r4.u64 = kCategoryMusicNormal;
  ctx.r5.u64 = pause;
  __imp__rex_XACT__CEngine__Pause(ctx, base);
}

// Pauses the level's music (each update) while any Stereo or the challenge
// plays a game song, resumes it after. Requires g_mutex.
void UpdateLevelMusic(PPCContext &ctx, uint8_t *base) {
  const bool hold = g_challenge.holds_music || g_world_music.holds_music ||
                    std::any_of(g_stereos.begin(), g_stereos.end(),
                                [](const auto &entry) { return entry.second.holds_music; });
  if (hold) {
    PauseLevelMusic(ctx, base, true);
    g_music_paused = true;
  } else if (g_music_paused) {
    PauseLevelMusic(ctx, base, false);
    g_music_paused = false;
  }
}

// Plays the playlist's current track. False when it can't (a game song
// without a wavebank: left out from now on). Requires g_mutex.
bool PlayCurrent(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle) {
  if (s.playlist.empty())
    return false;
  s.index %= s.playlist.size();
  const Track track = s.playlist[s.index];
  StopFile(s);
  StopSlot(ctx, base, part);
  s.waiting = false;
  if (track.kind != Kind::Game || track.song != s.held.song)
    ReleaseWavebanks(ctx, base, s.held);
  switch (track.kind) {
  case Kind::Radio:
    PlayEntry(ctx, base, s, part, vehicle, track.cue, track.second, -1.0);
    break;
  case Kind::Game: {
    // A wavebank missing, or one that failed to load: the song is left out.
    if (!WantWavebanks(ctx, base, s.held, *track.song)) {
      s.unavailable.insert(track.cue);
      return false;
    }
    const uint32_t state = SongState(base, *track.song);
    if (state == kWavebankFailed) {
      ReleaseWavebanks(ctx, base, s.held);
      s.unavailable.insert(track.cue);
      return false;
    }
    if (state == kWavebankPrepared) {
      PlayEntry(ctx, base, s, part, vehicle, track.cue, 0, -1.0, false);
    } else {
      s.waiting = true;
      s.waiting_since = Clock::now();
      PlaySilent(ctx, base, s, part, vehicle);
    }
    break;
  }
  case Kind::File:
    PlaySilent(ctx, base, s, part, vehicle);
    if (!PlayFile(s, track.file))
      return false;
    PlaceFile(base, s, vehicle);
    break;
  }
  renut::game::ShowBanner("Stereo: " + track.title);
  return true;
}

// The next track, past any that can't play. Requires g_mutex.
void Next(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle) {
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    const std::string previous = s.playlist.empty() ? std::string() : s.playlist[s.index % s.playlist.size()].title;
    s.playlist = BuildPlaylist(s, g_source);
    const size_t count = s.playlist.size();
    if (!count)
      return;
    // Where the previous track is now (the list may have lost songs).
    for (size_t i = 0; i < count; ++i) {
      if (s.playlist[i].title == previous) {
        s.index = i;
        break;
      }
    }
    s.index = (s.index + 1) % count;
    if (PlayCurrent(ctx, base, s, part, vehicle))
      return;
  }
}

// Plays the source's first track (all music when it has none: your music, an
// empty folder). Requires g_mutex.
void PlaySource(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle) {
  s.playlist = BuildPlaylist(s, g_source);
  if (s.playlist.empty()) {
    renut::game::ShowBanner("Stereo: no music in " + MusicFolder().string());
    g_source = Source::All;
    s.playlist = BuildPlaylist(s, g_source);
  }
  s.index = 0;
  if (!PlayCurrent(ctx, base, s, part, vehicle))
    Next(ctx, base, s, part, vehicle);
}

}  // namespace

// sub_82261608(..., double volume): starts a cue; reNut's silent ones at 0.
REX_HOOK_RAW(sub_82261608) {
  if (t_cue_volume >= 0.0)
    ctx.f1.f64 = t_cue_volume;
  __imp__sub_82261608(ctx, base);
}

// sub_8261FE38(part, vehicle): the action button down. A second press soon
// after one goes to the next source; turning on plays the playlist.
REX_HOOK_RAW(sub_8261FE38) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  Stereo &s = StereoOf(base, part);
  s.last_tick = Clock::now();
  if (s.pending_next && Clock::now() - s.pending_since <= kDoublePress) {
    s.pending_next = false;
    g_source = g_source == Source::All ? Source::Game : g_source == Source::Game ? Source::Yours : Source::All;
    // Held (holding still turns it off), and this press's release skips nothing.
    Store32(base, part + kHeld, 1);
    Store32(base, part + kHandled, 1);
    PlaySource(ctx, base, s, part, vehicle);
    if (!s.playlist.empty())
      renut::game::ShowBanner(std::string("Stereo: ") + SourceName(g_source) + " - " +
                    s.playlist[s.index % s.playlist.size()].title);
    return;
  }
  if (Load32(base, part + kOn)) {
    __imp__sub_8261FE38(ctx, base);
    return;
  }
  // Turning on: the game's own (its state, its look), then the playlist.
  __imp__sub_8261FE38(ctx, base);
  s.playlist = BuildPlaylist(s, g_source);
  if (s.playlist.empty()) {
    PlaySource(ctx, base, s, part, vehicle);
    return;
  }
  if (!PlayCurrent(ctx, base, s, part, vehicle))
    Next(ctx, base, s, part, vehicle);
}

// sub_8261FEA8(part, vehicle): the button up. A press (on, not handled) is the
// next track once no second press follows (the tick).
REX_HOOK_RAW(sub_8261FEA8) {
  const uint32_t part = ctx.r3.u32;
  if (Load32(base, part + kOn) && !Load32(base, part + kHandled)) {
    std::lock_guard lock(g_mutex);
    Stereo &s = StereoOf(base, part);
    s.pending_next = true;
    s.pending_since = Clock::now();
    Store32(base, part + kHeld, 0);
    Store32(base, part + kHandled, 0);
    return;
  }
  __imp__sub_8261FEA8(ctx, base);
}

namespace {

// After the Stereo's update: handles pending presses, song ends, waiting
// wavebanks and the playing file. Requires g_mutex.
void UpdateStereo(PPCContext &ctx, uint8_t *base, Stereo &s, uint32_t part, uint32_t vehicle) {
  const auto now = Clock::now();
  s.last_tick = now;
  if (!Load32(base, part + kOn)) {
    s.pending_next = false;
    s.waiting = false;
    StopFile(s);
    return;
  }
  if (s.pending_next && now - s.pending_since > kDoublePress) {
    s.pending_next = false;
    Next(ctx, base, s, part, vehicle);
    return;
  }
  if (s.playlist.empty())
    return;
  const Track track = s.playlist[s.index % s.playlist.size()];
  const bool playing = SlotPlaying(ctx, base, part);
  s.seen_playing |= playing;
  const bool ended = !playing && now - s.started > kStartGrace;
  if (s.waiting) {
    const uint32_t state = SongState(base, *track.song);
    if (state == kWavebankPrepared) {
      s.waiting = false;
      PlayEntry(ctx, base, s, part, vehicle, track.cue, 0, -1.0, false);
    } else if (state == kWavebankFailed || now - s.waiting_since > kWavebankWait) {
      s.unavailable.insert(track.cue);
      Next(ctx, base, s, part, vehicle);
    } else if (ended) {
      PlaySilent(ctx, base, s, part, vehicle);
    }
    return;
  }
  if (track.kind != Kind::File) {
    if (ended) {
      if (track.kind == Kind::Game && !s.seen_playing)
        s.unavailable.insert(track.cue);
      Next(ctx, base, s, part, vehicle);
    }
    return;
  }
  if (ended || now - s.started > kSilentRefresh)
    PlaySilent(ctx, base, s, part, vehicle);
  if (s.sound) {
    if (s.paused) {
      ma_sound_start(s.sound.get());
      s.paused = false;
    }
    if (ma_sound_at_end(s.sound.get())) {
      Next(ctx, base, s, part, vehicle);
      return;
    }
    PlaceFile(base, s, vehicle);
  }
}

}  // namespace

// sub_8261FB60(part, vehicle, double dt): the Stereo's update, then reNut's
// (UpdateStereo); the level's music paused while it plays a game song.
REX_HOOK_RAW(sub_8261FB60) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  __imp__sub_8261FB60(ctx, base);
  std::lock_guard lock(g_mutex);
  Stereo &s = StereoOf(base, part);
  UpdateStereo(ctx, base, s, part, vehicle);
  s.holds_music = Load32(base, part + kOn) && !s.waiting && !s.playlist.empty() &&
                  s.playlist[s.index % s.playlist.size()].kind == Kind::Game;
  UpdateLevelMusic(ctx, base);
}

// sub_82B4D518(audio system, cue list, ...): reads the cue list, making the
// soundbanks; the game songs' sounds go to the radio's category first.
REX_HOOK_RAW(sub_82B4D518) {
  MoveSongsToRadio(base, ctx.r4.u32);
  __imp__sub_82B4D518(ctx, base);
}

// sub_82620128(part): turns the Stereo off (a held button, or the game).
REX_HOOK_RAW(sub_82620128) {
  const uint32_t part = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  const auto it = g_stereos.find(part);
  const bool held_off = it != g_stereos.end() && Load32(base, part + kOn) && Load32(base, part + kHeld);
  ctx.r3.u64 = part;
  __imp__sub_82620128(ctx, base);
  if (it == g_stereos.end())
    return;
  it->second.pending_next = false;
  it->second.waiting = false;
  it->second.holds_music = false;
  StopFile(it->second);
  ReleaseWavebanks(ctx, base, it->second.held);
  UpdateLevelMusic(ctx, base);
  if (held_off)
    renut::game::ShowBanner("Stereo: off");
}

// sub_8261FA98(part): the Stereo goes.
REX_HOOK_RAW(sub_8261FA98) {
  const uint32_t part = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  StopSlot(ctx, base, part);
  if (auto it = g_stereos.find(part); it != g_stereos.end()) {
    StopFile(it->second);
    ReleaseWavebanks(ctx, base, it->second.held);
    g_stereos.erase(it);
    UpdateLevelMusic(ctx, base);
  }
  ctx.r3.u64 = part;
  __imp__sub_8261FA98(ctx, base);
}

// --- A challenge's song -----------------------------------------------------------

namespace {

// Whether a cue record's cue plays (sub_82211450).
bool RecordPlaying(PPCContext &ctx, uint8_t *base, uint32_t record) {
  if (!Load32(base, record))
    return false;
  ctx.r3.u64 = record;
  __imp__sub_82211450(ctx, base);
  return (ctx.r3.u32 & 0xFF) != 0;
}

void StopRecord(PPCContext &ctx, uint8_t *base, uint32_t record) {
  if (!record || !Load32(base, record))
    return;
  ctx.r3.u64 = record;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0;
  __imp__sub_8239F790(ctx, base);
}

// Starts 'cue' without a position into a cue record, as the game plays its
// music (audioMainPlayHash). False when it didn't.
bool StartRecord(PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t cue) {
  Store32(base, record + kCueRecordId, cue);
  ctx.r3.u64 = record + kCueRecordId;
  ctx.r5.u64 = kNoWorkspace;
  ctx.f1.f64 = 1.0;
  __imp__rex_audioMainPlayHash(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  Store32(base, record, handle);
  Store32(base, record + 4, kNoSlot);
  Store32(base, record + 8, kNoSlot);
  return handle != 0;
}

// Requires g_mutex.
void StopChallengeSong(PPCContext &ctx, uint8_t *base) {
  StopRecord(ctx, base, g_challenge.record);
  ReleaseWavebanks(ctx, base, g_challenge.held);
  g_challenge.song = nullptr;
  g_challenge.started = false;
  g_challenge.holds_music = false;
}

// Requires g_mutex.
void StopWorldMusic() {
  WorldMusic &m = g_world_music;
  if (m.sound) {
    ma_sound_stop(m.sound.get());
    ma_sound_uninit(m.sound.get());
    m.sound.reset();
  }
  m.paused = false;
  m.holds_music = false;
}

// Requires g_mutex.
void StopOneShot(OneShot &shot) {
  if (shot.sound) {
    ma_sound_stop(shot.sound.get());
    ma_sound_uninit(shot.sound.get());
    shot.sound.reset();
  }
  shot.paused = false;
}

}  // namespace

// Loops the played world's music (none outside mod worlds), paused while a challenge's song or a
// Stereo plays and with the game; the level's own music is paused under it.
void renut::game::WorldMusicTick(PPCContext &ctx, uint8_t *base, uint32_t scene) {
  std::filesystem::path file;
  float volume = 0;
  bool wanted;
  {
    std::lock_guard lock(g_mutex);
    file = g_music_override;  // a mod's (renut.sound.music), over the world's own
    volume = g_music_override_volume;
  }
  wanted = !file.empty() || renut::game::custom_worlds::WorldMusic(scene, &file, &volume);
  std::lock_guard lock(g_mutex);
  WorldMusic &m = g_world_music;
  if (!wanted) {
    if (m.sound || m.holds_music) {
      StopWorldMusic();
      UpdateLevelMusic(ctx, base);
    }
    m.file.clear();
    m.failed = false;
    return;
  }
  m.last_tick = Clock::now();
  if (file != m.file) {
    StopWorldMusic();
    m.file = file;
    m.failed = false;
  }
  if (!m.sound && !m.failed) {
    auto sound = std::make_unique<ma_sound>();
    const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
    const bool ok = EnsureEngine() &&
                    ma_sound_init_from_file_w(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get()) == MA_SUCCESS;
#else
    const bool ok = EnsureEngine() &&
                    ma_sound_init_from_file(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get()) == MA_SUCCESS;
#endif
    if (!ok) {
      m.failed = true;
      return;
    }
    ma_sound_set_looping(sound.get(), MA_TRUE);
    m.sound = std::move(sound);
    m.paused = true;  // started below
  }
  if (!m.sound)
    return;
  // Another song takes the floor: the challenge's, or a Stereo's.
  const bool other = g_challenge.holds_music ||
                     std::any_of(g_stereos.begin(), g_stereos.end(), [](const auto &entry) {
                       return entry.second.holds_music || (entry.second.sound && !entry.second.paused);
                     });
  if (other && !m.paused) {
    ma_sound_stop(m.sound.get());
    m.paused = true;
  } else if (!other && m.paused) {
    ma_sound_start(m.sound.get());
    m.paused = false;
  }
  ma_sound_set_volume(m.sound.get(), volume);
  m.holds_music = !other;
  UpdateLevelMusic(ctx, base);
}

void renut::game::SetMusicOverride(const std::filesystem::path &file, float volume) {
  std::lock_guard lock(g_mutex);
  g_music_override = file;
  g_music_override_volume = volume;
}

void renut::game::PlayOneShot(const std::filesystem::path &file, float volume, uint32_t channel) {
  std::lock_guard lock(g_mutex);
  OneShot &shot = g_one_shots[channel];
  StopOneShot(shot);
  if (!EnsureEngine())
    return;
  auto sound = std::make_unique<ma_sound>();
  const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
  const ma_result result = ma_sound_init_from_file_w(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#else
  const ma_result result = ma_sound_init_from_file(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
  if (result != MA_SUCCESS)
    return;
  ma_sound_set_volume(sound.get(), volume);
  ma_sound_start(sound.get());
  shot.sound = std::move(sound);
  g_one_shots_tick = Clock::now();  // the watchdog waits for the next tick
}

namespace {

void StopModSoundLocked(ModSound &mod) {
  if (mod.sound) {
    ma_sound_stop(mod.sound.get());
    ma_sound_uninit(mod.sound.get());
    mod.sound.reset();
  }
  mod.paused = false;
}

// Its loudness and side from where it is to the camera, as a Stereo's file is placed; without a position, its volume.
void PlaceModSound(const uint8_t *base, ModSound &mod) {
  if (!mod.sound)
    return;
  if (!mod.positional) {
    ma_sound_set_volume(mod.sound.get(), mod.volume);
    ma_sound_set_pan(mod.sound.get(), 0.0f);
    return;
  }
  const Vec3 camera = LoadVec3(base, kCameraPosition);
  const Vec3 right = LoadVec3(base, kCameraRight);
  const Vec3 d{mod.at.x - camera.x, mod.at.y - camera.y, mod.at.z - camera.z};
  const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!std::isfinite(distance))
    return;
  const float near = mod.range * (kNear / kFar);
  float gain = std::clamp((mod.range - distance) / std::max(mod.range - near, 0.01f), 0.0f, 1.0f);
  gain *= gain;
  float pan = 0.0f;
  if (distance > 0.5f)
    pan = std::clamp((d.x * right.x + d.y * right.y + d.z * right.z) / distance * 0.8f, -0.8f, 0.8f);
  ma_sound_set_volume(mod.sound.get(), gain * mod.volume);
  ma_sound_set_pan(mod.sound.get(), pan);
}

} // namespace

uint32_t renut::game::PlayModSound(const std::filesystem::path &file, float volume, float pitch, bool loop,
                                   bool positional, float x, float y, float z, float range) {
  std::lock_guard lock(g_mutex);
  if (!EnsureEngine())
    return 0;
  auto sound = std::make_unique<ma_sound>();
  const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
  const ma_result result = ma_sound_init_from_file_w(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#else
  const ma_result result = ma_sound_init_from_file(&g_engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
  if (result != MA_SUCCESS)
    return 0;
  const uint32_t handle = g_next_mod_sound++;
  if (!g_next_mod_sound)
    g_next_mod_sound = 1;
  ModSound &mod = g_mod_sounds[handle];
  mod.loop = loop;
  mod.volume = std::clamp(volume, 0.0f, 4.0f);
  mod.positional = positional;
  mod.at = {x, y, z};
  mod.range = std::max(range, 1.0f);
  ma_sound_set_looping(sound.get(), loop ? MA_TRUE : MA_FALSE);
  ma_sound_set_pitch(sound.get(), std::clamp(pitch, 0.05f, 8.0f));
  // A placed sound starts silent until the next tick puts it where it is.
  ma_sound_set_volume(sound.get(), positional ? 0.0f : mod.volume);
  ma_sound_start(sound.get());
  mod.sound = std::move(sound);
  g_one_shots_tick = Clock::now();
  return handle;
}

void renut::game::SetModSound(uint32_t handle, float volume, bool positional, float x, float y, float z) {
  std::lock_guard lock(g_mutex);
  const auto found = g_mod_sounds.find(handle);
  if (found == g_mod_sounds.end())
    return;
  ModSound &mod = found->second;
  mod.volume = std::clamp(volume, 0.0f, 4.0f);
  mod.positional = positional;
  mod.at = {x, y, z};
  if (!positional && mod.sound)
    ma_sound_set_volume(mod.sound.get(), mod.volume);
}

void renut::game::SetModSoundPitch(uint32_t handle, float pitch) {
  std::lock_guard lock(g_mutex);
  const auto found = g_mod_sounds.find(handle);
  if (found != g_mod_sounds.end() && found->second.sound)
    ma_sound_set_pitch(found->second.sound.get(), std::clamp(pitch, 0.05f, 8.0f));
}

void renut::game::StopModSound(uint32_t handle) {
  std::lock_guard lock(g_mutex);
  const auto found = g_mod_sounds.find(handle);
  if (found == g_mod_sounds.end())
    return;
  StopModSoundLocked(found->second);
  g_mod_sounds.erase(found);
}

bool renut::game::ModSoundPlaying(uint32_t handle) {
  std::lock_guard lock(g_mutex);
  const auto found = g_mod_sounds.find(handle);
  return found != g_mod_sounds.end() && found->second.sound && (found->second.paused || !ma_sound_at_end(found->second.sound.get()));
}

// The one-shots resume after a pause and go once played; a new scene stops them and lets go of the
// last one's voice wavebanks, and wants the wavebanks of its NPCs' game voices.
void renut::game::NpcSoundsTick(PPCContext &ctx, uint8_t *base, uint32_t scene) {
  const std::vector<uint32_t> wavebanks = renut::game::custom_npcs::VoiceWavebanks(scene);
  std::lock_guard lock(g_mutex);
  g_one_shots_tick = Clock::now();
  if (scene != g_npc_scene) {
    for (auto &[channel, shot] : g_one_shots)
      StopOneShot(shot);
    g_one_shots.clear();
    for (auto &[handle, mod] : g_mod_sounds)
      StopModSoundLocked(mod);
    g_mod_sounds.clear();
    ReleaseWavebanks(ctx, base, g_voice_wavebanks);
    g_voice_wavebanks_seen.clear();
    g_npc_scene = scene;
  }
  for (auto it = g_one_shots.begin(); it != g_one_shots.end();) {
    OneShot &shot = it->second;
    if (!shot.sound || ma_sound_at_end(shot.sound.get())) {
      StopOneShot(shot);
      it = g_one_shots.erase(it);
      continue;
    }
    if (shot.paused) {
      ma_sound_start(shot.sound.get());
      shot.paused = false;
    }
    ++it;
  }
  for (auto it = g_mod_sounds.begin(); it != g_mod_sounds.end();) {
    ModSound &mod = it->second;
    if (!mod.sound || (!mod.paused && !mod.loop && ma_sound_at_end(mod.sound.get()))) {
      StopModSoundLocked(mod);
      it = g_mod_sounds.erase(it);
      continue;
    }
    if (mod.paused) {
      ma_sound_start(mod.sound.get());
      mod.paused = false;
    }
    PlaceModSound(base, mod);
    ++it;
  }
  // A wavebank not there yet is looked for again; one the game wants already is left to it.
  for (const uint32_t id : wavebanks) {
    if (std::find(g_voice_wavebanks_seen.begin(), g_voice_wavebanks_seen.end(), id) != g_voice_wavebanks_seen.end())
      continue;
    const uint32_t wavebank = FindWavebank(base, id);
    if (!wavebank)
      continue;
    g_voice_wavebanks_seen.push_back(id);
    if (!Load32(base, wavebank + kWavebankWanted)) {
      SetWanted(ctx, base, wavebank, true);
      g_voice_wavebanks.owned.push_back(id);
    }
  }
}

// Plays the challenge's song while it is in play (restarting when it ends),
// loading its wavebanks when the challenge is chosen.
void renut::game::ChallengeMusicTick(PPCContext &ctx, uint8_t *base, uint32_t challenge, bool in_play) {
  const GameSong *song = nullptr;
  for (const ChallengeSong &entry : kChallengeSongs) {
    if (entry.challenge != challenge)
      continue;
    for (const GameSong &game : kGameSongs) {
      if (game.cue == entry.cue)
        song = &game;
    }
  }
  std::lock_guard lock(g_mutex);
  ChallengeMusic &m = g_challenge;
  if (!song && !m.song)
    return;
  const auto now = Clock::now();
  // A song that can't play stays chosen (not tried again this challenge).
  const auto give_up = [&] {
    ReleaseWavebanks(ctx, base, m.held);
    m.started = true;
    m.since = Clock::time_point::max();
  };
  if (song != m.song) {
    StopChallengeSong(ctx, base);
    UpdateLevelMusic(ctx, base);
    if (!song)
      return;
    m.song = song;
    m.since = now;
    if (!m.record && (m.record = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(kCueRecordSize)))
      std::memset(base + m.record, 0, kCueRecordSize);
    if (!m.record || !WantWavebanks(ctx, base, m.held, *song))
      give_up();
    return;
  }
  if (m.since == Clock::time_point::max())
    return;
  if (!in_play) {
    if (m.started) {
      StopRecord(ctx, base, m.record);
      m.started = false;
      m.holds_music = false;
      UpdateLevelMusic(ctx, base);
    }
    m.since = now;  // the wait for the wavebanks counts from the start
    return;
  }
  if (!m.started) {
    const uint32_t state = SongState(base, *m.song);
    if (state == kWavebankPrepared) {
      StartRecord(ctx, base, m.record, m.song->cue);  // not started: again after kStartGrace
      m.started = true;
      m.since = now;
    } else if (state == kWavebankFailed || now - m.since > kWavebankWait) {
      give_up();
    }
    return;
  }
  m.holds_music = RecordPlaying(ctx, base, m.record);
  if (!m.holds_music && now - m.since > kStartGrace) {
    StartRecord(ctx, base, m.record, m.song->cue);
    m.since = now;
  }
  UpdateLevelMusic(ctx, base);
}
