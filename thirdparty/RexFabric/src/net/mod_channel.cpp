// The mod channel (include/rexfabric/mod_channel.h, docs/mod_channel.md): topics and their handlers, and each message
// framed as its topic's hash (4 bytes, big endian) followed by its payload.

#include <rexfabric/mod_channel.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "../log.h"

namespace rexfabric::channel {
namespace {

constexpr size_t kTopicBytes = 4;

struct Topic {
  std::string name;
  std::shared_ptr<const Handler> handler;
};

// The transport and the topics, by hash. Handlers are shared so one can run while another thread changes the topics.
struct State {
  std::mutex mutex;
  Transport transport;
  std::unordered_map<uint32_t, Topic> topics;
  std::set<uint32_t> unheard;  // topics received with no handler, logged once each
};

State& GetState() {
  static auto* state = new State();
  return *state;
}

bool Send(Target target, Member member, std::string_view topic, std::span<const uint8_t> payload) {
  State& state = GetState();
  std::function<bool(Target, Member, std::span<const uint8_t>)> send;
  size_t max_frame = 0;
  {
    const std::lock_guard lock(state.mutex);
    send = state.transport.send;
    max_frame = state.transport.max_frame;
  }
  if (!send || topic.empty() || kTopicBytes + payload.size() > max_frame) return false;
  Writer frame;
  frame.U32(Fnv1a(topic)).Bytes(payload);
  return send(target, member, frame.bytes());
}

}  // namespace

void SetTransport(Transport transport) {
  State& state = GetState();
  const std::lock_guard lock(state.mutex);
  state.transport = std::move(transport);
}

void Receive(Member from, std::span<const uint8_t> frame) {
  Reader read(frame);
  const std::optional<uint32_t> hash = read.U32();
  if (!hash) return;
  State& state = GetState();
  std::shared_ptr<const Handler> handler;
  std::string name;
  {
    const std::lock_guard lock(state.mutex);
    const auto found = state.topics.find(*hash);
    if (found == state.topics.end()) {
      if (state.unheard.insert(*hash).second) RF_WARN("mod channel: a message of topic {:08X} came, which nothing here listens to; this box may run other mods", *hash);
      return;
    }
    handler = found->second.handler;
    name = found->second.name;
  }
  (*handler)(Message{.topic = name, .from = from, .payload = frame.subspan(kTopicBytes)});
}

bool Listen(std::string_view topic, Handler handler) {
  if (topic.empty() || !handler) {
    RF_ERROR("mod channel: a topic needs a name and a handler");
    return false;
  }
  State& state = GetState();
  const std::lock_guard lock(state.mutex);
  const uint32_t hash = Fnv1a(topic);
  if (const auto found = state.topics.find(hash); found != state.topics.end()) {
    if (found->second.name == topic) {
      RF_ERROR("mod channel: topic {} is listened to already", topic);
    } else {
      RF_ERROR("mod channel: topic {} has the hash of topic {}; rename one", topic, found->second.name);
    }
    return false;
  }
  state.topics.emplace(hash, Topic{.name = std::string(topic), .handler = std::make_shared<const Handler>(std::move(handler))});
  return true;
}

bool Unlisten(std::string_view topic) {
  State& state = GetState();
  const std::lock_guard lock(state.mutex);
  const auto found = state.topics.find(Fnv1a(topic));
  if (found == state.topics.end() || found->second.name != topic) return false;
  state.topics.erase(found);
  return true;
}

Role CurrentRole() {
  State& state = GetState();
  std::function<Role()> role;
  {
    const std::lock_guard lock(state.mutex);
    role = state.transport.role;
  }
  return role ? role() : Role::kAlone;
}

size_t MaxPayload() {
  State& state = GetState();
  const std::lock_guard lock(state.mutex);
  return state.transport.max_frame > kTopicBytes ? state.transport.max_frame - kTopicBytes : 0;
}

bool SendToHost(std::string_view topic, std::span<const uint8_t> payload) { return Send(Target::kHost, 0, topic, payload); }

bool SendToAll(std::string_view topic, std::span<const uint8_t> payload) { return Send(Target::kAll, 0, topic, payload); }

bool SendTo(Member member, std::string_view topic, std::span<const uint8_t> payload) { return Send(Target::kMember, member, topic, payload); }

// Writer ---------------------------------------------------------------------------------------------------------------

Writer& Writer::U8(uint8_t value) {
  bytes_.push_back(value);
  return *this;
}

Writer& Writer::U16(uint16_t value) { return U8(static_cast<uint8_t>(value >> 8)).U8(static_cast<uint8_t>(value)); }

Writer& Writer::U32(uint32_t value) { return U16(static_cast<uint16_t>(value >> 16)).U16(static_cast<uint16_t>(value)); }

Writer& Writer::Bytes(std::span<const uint8_t> bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return *this;
}

// Reader ---------------------------------------------------------------------------------------------------------------

std::optional<uint32_t> Reader::Take(size_t count) {
  if (failed_ || left() < count) {
    failed_ = true;
    return std::nullopt;
  }
  uint32_t value = 0;
  for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes_[at_ + i];
  at_ += count;
  return value;
}

std::optional<uint8_t> Reader::U8() {
  const std::optional<uint32_t> value = Take(1);
  return value ? std::optional<uint8_t>(static_cast<uint8_t>(*value)) : std::nullopt;
}

std::optional<uint16_t> Reader::U16() {
  const std::optional<uint32_t> value = Take(2);
  return value ? std::optional<uint16_t>(static_cast<uint16_t>(*value)) : std::nullopt;
}

std::optional<uint32_t> Reader::U32() { return Take(4); }

std::optional<std::span<const uint8_t>> Reader::Bytes(size_t count) {
  if (failed_ || left() < count) {
    failed_ = true;
    return std::nullopt;
  }
  const std::span<const uint8_t> bytes = bytes_.subspan(at_, count);
  at_ += count;
  return bytes;
}

}  // namespace rexfabric::channel
