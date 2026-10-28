/**
 * @file    game/stereo.cpp
 * @brief   N&B's Stereo part, owned from the start, with the game's music and
 *          the player's own.
 *
 * N&B (TU3, IDA): the Stereo is a Jinjo Bingo prize:
 * aid_objparams_banjox_vehicleblock_gadgets_stereo (class vehicleBlockStereo,
 * category "accessory", type "stereo", which the garage's accessory grouping
 * lists), its model, icon and garage text in the parts bundle, and a track
 * list (objparams + 968, aid_misc_banjox_stereotracklists_default) of the
 * game's two radio songs, cues Music_Radio_ClickClockWood (0x7EE19746) and
 * Music_Radio_FreezeeyPark (0xCDC00E78). Its descriptor (getter 0x8261F8C0,
 * fill sub_8261F908):
 *   init sub_8261F9D8: the track list into a vector at + 1164 ({u16 entry
 *     size, ..., + 4 first, + 8 end}; entries {cue, ?}), track + 1156 = 0
 *   [7] tick sub_8261FB60(part, vehicle, dt): held (+ 1168) for a second,
 *     not yet handled (+ 1176), turns it off (sub_82620128); keeps the
 *     playing cue (sound slot + 1152, -1 for none) at the part
 *   [18] button down sub_8261FE38: off (+ 1160 0) -> on, plays the track
 *     (sub_82620008(part, vehicle): stops the slot's cue, starts the track's
 *     entry where the part is), handled; held
 *   [19] button up sub_8261FEA8: on and not handled -> the next track
 * A part's sound slots (any vehicle part): a vector at + 816 of 16-byte
 * slots {cue handle (0: none started), -1, -1, in use};
 * sub_8261C7C8(part, &entry, &position, positional, &slot) starts an entry's
 * cue in a free slot through sub_82261608(..., double volume 0-1),
 * sub_8261C888(part, &slot) stops one (slot -1 after); sub_82211450(&slot
 * record) is whether its cue still plays.
 *
 * Music: a music cue the Stereo plays takes the level's music's place, which
 * comes back once the cue ends (the radio songs; reNut's first versions: a
 * file played with the Stereo's cue stopped, a silent radio song under
 * another music cue silenced that cue). A cue plays from wavebanks: the cue
 * list (aid_xcuelist_banjox_default) makes every wavebank at startup
 * (sub_82B59F38; 160-byte AudioWavebank_c: + 32 its asset id, + 44 wanted
 * (0 no, 1 until idle, 2 kept), + 48 state (1 failed, 6 prepared)), in the
 * wavebank manager (dword_82DC1448; + 72 first, + 76 end); each world loads
 * its own music's. sub_82B610B0(wavebank, want, keep) sets + 44, and the
 * wavebank's tick opens, loads (streaming) or unloads it.
 *
 * reNut gives it (renut_stereo: restored_parts.cpp, custom_parts.cpp adds it
 * to the garage inventory) and plays a playlist: the radio songs and four of
 * the game's songs (kGameSongs: Main Theme, Mumbo's Mountain, Spiral
 * Challenge, Grunty's Challenge; their wavebanks loaded when the level hasn't,
 * played without a position, as music cues have no 3D distances; the level's
 * music paused meanwhile, see kCategoryMusicNormal), the player's own music (the files in the "music" folder beside
 * renut.exe: MP3, Ogg Vorbis, FLAC, WAV; read again each time), or both. A
 * press is the next track, two quick presses the next source (all, the
 * game's, yours), holding turns it off, as the game's does. A song ends: the
 * next track. Under a file a radio song plays silently (volume 0) in the
 * Stereo's slot, started again every minute, keeping the level's music away.
 * reNut plays the files (miniaudio, on a sound device of its own) as if from
 * the vehicle: louder the nearer the camera, panned to its side. They pause
 * while the Stereo isn't updated (the game paused, the level gone).
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
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
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/banner.h"
#include "renut_engine/game/stereo.h"

REXCVAR_DEFINE_BOOL(renut_stereo, true, "Nuts&Bolts/Vehicles",
                    "Gives you the Stereo (a Jinjo Bingo prize) from the start: on a vehicle its "
                    "action button plays the game's music and your own (the 'music' folder beside "
                    "renut.exe). Press: next track. Press twice: all music, the game's, or yours. "
                    "Hold: off.");
REXCVAR_DEFINE_DOUBLE(renut_stereo_volume, 0.8, "Nuts&Bolts/Vehicles",
                      "How loud the Stereo plays your own music (0 to 1).")
    .range(0.0, 1.0);

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
REX_EXTERN(__imp__audioMainPlayHash);  // (&cue, , workspace, double volume): a 2D cue
REX_EXTERN(__imp__sub_8239F790);       // a cue record: stops its cue (record, now, )

namespace {

using Clock = std::chrono::steady_clock;

// The Stereo (see the file comment).
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
// The wavebank manager and a wavebank (see the file comment).
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
// The game's other songs: music cues and the wavebanks each plays from (cue
// list aid_xcuelist_banjox_default, by name; the stream bundle 50/685374 holds
// the wavebanks). Music cues have no 3D distances (the radio songs': 20 to
// 70), so the Stereo plays them without a position (PlayEntry2D). MusicTitle
// (Rawk 'n Roll) has no cue; its waves are MusicMainTheme's, byte for byte.
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
    // Interactive cues: the cue list's Music_Interactive_*_Race records play
    // them with their variable at 1 (the race song; 0 the drum roll, 2 and 3
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

// The level's music keeps playing under a song played without a position, so
// the Stereo pauses it: XACT pauses a category's playing cues
// (CEngine::Pause(engine, category, pause), IXACTEngine vtable 0x82003E58
// + 68; it reads each cue's category from its sound in the soundbank, which
// XACT plays in place). All level music is under music_normal (its main, race
// and others); at startup reNut moves the songs' sounds to music_radio, the
// radio songs' category, so pausing music_normal leaves them playing.
// XACT categories (the cue list's global settings).
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
// An XACT soundbank (v43, big-endian; offsets from its start): "KBDS", cue
// counts, tables, its name. A simple cue: {u8 flags, u32 sound};
// a complex cue: {u8 flags (4: one sound), u32 sound or variation table,
// ...} (15 bytes); a variation table {u16 type, u16 count, u32}, then
// {u32 sound, f32, f32, u32} entries; a sound's category at + 1 (u16), its
// volume at + 3 (u8; the radio songs' 180, the four songs' 101 to 127).
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

std::mutex g_mutex;  // the stereos, the source, the engine, the challenge's song
std::unordered_map<uint32_t, Stereo> g_stereos;
ChallengeMusic g_challenge;
bool g_music_paused = false;  // reNut paused music_normal
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
}

bool EnsureEngine() {
  if (g_engine_ready)
    return true;
  if (g_engine_failed)
    return false;
  ma_engine_config config = ma_engine_config_init();
  if (ma_engine_init(&config, &g_engine) != MA_SUCCESS) {
    g_engine_failed = true;
    RNUT_ERROR("Stereo: no sound device for your own music (miniaudio); the game's songs still play");
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
  if (result != MA_SUCCESS) {
    RNUT_WARN("Stereo: can't play '{}' (miniaudio error {})", FileTitle(file), int(result));
    return false;
  }
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
  ma_sound_set_volume(s.sound.get(), gain * float(REXCVAR_GET(renut_stereo_volume)));
  ma_sound_set_pan(s.sound.get(), pan);
}

// --- The game's cues and wavebanks ------------------------------------------------

void StopSlot(PPCContext &ctx, uint8_t *base, uint32_t part);

// The Stereo's slot plays 'cue' at 'volume' (< 0: the game's), through its
// first track entry: where the vehicle is, as the game's Stereo does, or
// without a position (a music cue: sub_82261608 then plays it as the game
// plays music, audioMainPlayHash).
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

// Moves the songs' sounds from the level music's categories to music_radio in
// the cue list's "Music" soundbank, before XACT makes the soundbanks, and
// makes them as loud as the radio songs: each song's loudest sound gets
// Radio_ClickClockWood's volume, its others keep their difference.
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
    uint32_t moved = 0;
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
        ++moved;
      }
    }
    RNUT_INFO("Stereo: {} sounds of the game's songs moved to the radio's category (volume {})", moved,
              radio_volume);
    return;
  }
  RNUT_WARN("Stereo: no Music soundbank in the cue list; the level's music will play under game songs");
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

// The level's music paused while any Stereo or the challenge plays a game
// song (again each update: XACT pauses only the cues already playing),
// resumed after. Requires g_mutex.
void UpdateLevelMusic(PPCContext &ctx, uint8_t *base) {
  const bool hold = g_challenge.holds_music ||
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
    if (!WantWavebanks(ctx, base, s.held, *track.song)) {
      RNUT_WARN("Stereo: '{}' has a wavebank missing; left out", track.title);
      s.unavailable.insert(track.cue);
      return false;
    }
    const uint32_t state = SongState(base, *track.song);
    if (state == kWavebankFailed) {
      RNUT_WARN("Stereo: a wavebank of '{}' failed to load; left out", track.title);
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

// After the Stereo's update: a single press's next track, a game song's
// wavebanks prepared, a song's end (the next), the silent radio song under a
// file, where the file is. Requires g_mutex.
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
      RNUT_WARN("Stereo: the wavebanks of '{}' didn't load ({}); left out", track.title,
                state == kWavebankFailed ? "failed" : "timed out");
      s.unavailable.insert(track.cue);
      Next(ctx, base, s, part, vehicle);
    } else if (ended) {
      PlaySilent(ctx, base, s, part, vehicle);
    }
    return;
  }
  if (track.kind != Kind::File) {
    if (ended) {
      if (track.kind == Kind::Game && !s.seen_playing) {
        RNUT_WARN("Stereo: '{}' didn't play; left out", track.title);
        s.unavailable.insert(track.cue);
      }
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
  __imp__audioMainPlayHash(ctx, base);
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

}  // namespace

// The challenge's song: its wavebanks wanted when the challenge is chosen,
// its cue started once it's in play and they're prepared, again whenever it
// ends, stopped at the results or with the challenge.
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
  const auto give_up = [&](const char *why) {
    RNUT_WARN("Stereo: '{}' {}; the challenge keeps its music", m.song->title, why);
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
    if (!m.record && (m.record = REX_KERNEL_MEMORY()->SystemHeapAlloc(kCueRecordSize)))
      std::memset(base + m.record, 0, kCueRecordSize);
    if (!m.record || !WantWavebanks(ctx, base, m.held, *song))
      give_up("has a wavebank missing");
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
      give_up("didn't load");
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
