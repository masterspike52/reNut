#include "audio.h"

#include <array>

#include <SDL3/SDL.h>

namespace rexglue_guide::audio {
namespace {

struct Clip {
  Uint8* data = nullptr;
  Uint32 size = 0;
};

struct State {
  bool tried = false;
  bool ready = false;
  bool owns_subsystem = false;
  bool enabled = true;
  SDL_AudioStream* ui = nullptr;
  SDL_AudioStream* notify = nullptr;
  std::array<Clip, size_t(assets::Sound::kCount)> clips{};
};

State g_state;

bool EnsureReady() {
  if (g_state.tried) return g_state.ready;
  g_state.tried = true;
  // Built without sounds (assets.h): nothing to play, so no audio device.
  bool any = false;
  for (size_t i = 0; i < g_state.clips.size(); ++i) any = any || !assets::SoundWav(assets::Sound(i)).empty();
  if (!any) return false;
  // A reference of our own even when audio is already up: on Linux the runtime
  // shares this SDL, and its audio driver quits the subsystem when the title
  // shuts XAudio down (after the logo movie, for one). Without ours, that quit
  // would be the last one and SDL would destroy these streams with it.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
  g_state.owns_subsystem = true;
  SDL_AudioSpec spec{};
  for (size_t i = 0; i < g_state.clips.size(); ++i) {
    const auto wav = assets::SoundWav(assets::Sound(i));
    if (wav.empty()) continue;  // this one plays nothing
    SDL_IOStream* io = SDL_IOFromConstMem(wav.data(), wav.size());
    SDL_LoadWAV_IO(io, true, &spec, &g_state.clips[i].data, &g_state.clips[i].size);
  }
  // Every clip is 48 kHz s16 stereo (assets.cpp).
  g_state.ui = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  g_state.notify =
      SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  for (SDL_AudioStream* stream : {g_state.ui, g_state.notify}) {
    if (!stream) continue;
    SDL_SetAudioStreamGain(stream, 0.7f);
    SDL_ResumeAudioStreamDevice(stream);
  }
  g_state.ready = g_state.ui != nullptr;
  return g_state.ready;
}

}  // namespace

void Play(assets::Sound sound) {
  if (!g_state.enabled || !EnsureReady()) return;
  const Clip& clip = g_state.clips[size_t(sound)];
  if (!clip.data) return;
  SDL_AudioStream* stream =
      sound == assets::Sound::kNotify && g_state.notify ? g_state.notify : g_state.ui;
  SDL_ClearAudioStream(stream);
  SDL_PutAudioStreamData(stream, clip.data, int(clip.size));
}

void SetEnabled(bool enabled) {
  g_state.enabled = enabled;
}

void Shutdown() {
  for (SDL_AudioStream* stream : {g_state.ui, g_state.notify}) {
    if (stream) SDL_DestroyAudioStream(stream);
  }
  for (Clip& clip : g_state.clips) SDL_free(clip.data);
  if (g_state.owns_subsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  g_state = {};
}

}  // namespace rexglue_guide::audio
