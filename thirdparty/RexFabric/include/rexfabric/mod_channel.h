// The mod channel (docs/mod_channel.md): messages between the boxes of a co-op game, for the game's own mod code and
// for mods. A message has a topic, a name every box agrees on ("tip.packets"), and a payload of bytes. RexFabric keeps
// the topics and their handlers and frames each message; the game gives it a transport that carries the frames over
// its own network stream, so the channel knows nothing of the game's network.
//
//   rexfabric::channel::Listen("mymod.scores", [](const rexfabric::channel::Message& message) {
//     rexfabric::channel::Reader read(message.payload);
//     const std::optional<uint32_t> score = read.U32();
//   });
//   rexfabric::channel::Writer payload;
//   payload.U8(1).U32(score);
//   rexfabric::channel::SendToHost("mymod.scores", payload.bytes());
//
// The game's side, once at start and then for every frame its stream delivers:
//
//   rexfabric::channel::SetTransport({.send = Send, .role = Role, .max_frame = 1252});
//   rexfabric::channel::Receive(from, frame);
//
// Handlers run on the thread the transport calls Receive on; sends run on the caller's thread, and the transport says
// which threads it takes them from (a game whose stream is ticked by its game thread wants them there).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rexfabric::channel {

// FNV-1a over the bytes of `text`, 32 bits: what a topic travels as, and a hash a game can use for its own names so
// every box computes the same number for the same name.
constexpr uint32_t Fnv1a(std::string_view text) {
  uint32_t hash = 0x811C9DC5u;
  for (const char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x01000193u;
  }
  return hash;
}

// A box of the game as the transport numbers it: stable while that box stays in the game.
using Member = uint32_t;

// What this box is in the game the transport carries: playing alone (nothing to send to), hosting it, or a guest in
// another box's game.
enum class Role : uint8_t { kAlone, kHost, kGuest };

// Who a frame goes to.
enum class Target : uint8_t { kHost, kAll, kMember };

struct Message {
  std::string_view topic;
  Member from = 0;
  std::span<const uint8_t> payload;
};

using Handler = std::function<void(const Message& message)>;

// The game's carrier of frames. `send` takes a whole frame to `target` (`member` only for kMember) and says whether it
// went; `role` says what this box is now; `max_frame` is the largest frame in bytes the stream takes in one message.
struct Transport {
  std::function<bool(Target target, Member member, std::span<const uint8_t> frame)> send;
  std::function<Role()> role;
  size_t max_frame = 0;
};

// Sets the transport; a later call replaces it. Without one every send fails and the role is kAlone.
void SetTransport(Transport transport);
// A frame the stream delivered from `from`: handed to its topic's handler, or dropped (logged once per topic) when no
// handler listens to it here.
void Receive(Member from, std::span<const uint8_t> frame);

// Listens to a topic: every message of it this box receives goes to `handler`. One handler per topic; false (logged)
// for an empty topic, one already listened to, or one whose hash another topic has.
bool Listen(std::string_view topic, Handler handler);
// Stops listening; false when nothing listened.
bool Unlisten(std::string_view topic);

// What this box is now, kAlone without a transport.
Role CurrentRole();
// The largest payload one message carries, 0 without a transport.
size_t MaxPayload();

// Sends a message: to the host, to every other box, or to one. False when there's no transport, the payload is larger
// than MaxPayload, or the transport couldn't send it (no game to send in, for one).
bool SendToHost(std::string_view topic, std::span<const uint8_t> payload);
bool SendToAll(std::string_view topic, std::span<const uint8_t> payload);
bool SendTo(Member member, std::string_view topic, std::span<const uint8_t> payload);

// A payload written big endian, a field after another.
class Writer {
 public:
  Writer& U8(uint8_t value);
  Writer& U16(uint16_t value);
  Writer& U32(uint32_t value);
  Writer& Bytes(std::span<const uint8_t> bytes);

  std::span<const uint8_t> bytes() const { return bytes_; }
  size_t size() const { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};

// A payload read back as a Writer wrote it. A read past the end gives none and leaves the reader failed, so a message
// cut short reads as nothing rather than as garbage.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  std::optional<uint8_t> U8();
  std::optional<uint16_t> U16();
  std::optional<uint32_t> U32();
  std::optional<std::span<const uint8_t>> Bytes(size_t count);

  bool failed() const { return failed_; }
  size_t left() const { return bytes_.size() - at_; }

 private:
  std::optional<uint32_t> Take(size_t count);

  std::span<const uint8_t> bytes_;
  size_t at_ = 0;
  bool failed_ = false;
};

}  // namespace rexfabric::channel
