# The mod channel

Mods that change a co-op game need to tell the other boxes about it: which kind of ground a brush poured, what a player
chose in a mod's menu, later which mods a box runs. The mod channel carries such messages between the boxes of a game.
It is transport neutral: RexFabric keeps the topics, their handlers and the framing, and the game gives it a transport
that carries frames over the game's own network stream, so the channel knows nothing of any one game's network.

The header is `include/rexfabric/mod_channel.h`, the code `src/net/mod_channel.cpp`.

## Topics

A message has a topic, a name every box agrees on (`"tip.packets"`, `"mymod.scores"`), and a payload of bytes. A box
listens to a topic with one handler; a message of a topic nobody listens to here is dropped and logged once per topic,
as a box running other mods would cause.

```cpp
rexfabric::channel::Listen("mymod.scores", [](const rexfabric::channel::Message& message) {
  rexfabric::channel::Reader read(message.payload);
  const std::optional<uint8_t> kind = read.U8();
  const std::optional<uint32_t> score = read.U32();
  if (!score) return;  // cut short: every read after the end is none
  ...
});
```

`message.from` is the sending box as the transport numbers it, which `SendTo` takes to answer it. `Unlisten` stops
listening. A topic travels as the FNV-1a hash of its name (`channel::Fnv1a`, 32 bits), so `Listen` refuses a second
handler for a topic, and a topic whose hash another topic already has; rename one of them.

Keep a kind byte at the front of a topic's payloads, so later kinds can be added and an older box drops what it doesn't
know.

## Sending

| Function | Sends to |
|---|---|
| `SendToHost(topic, payload)` | The box hosting the game |
| `SendToAll(topic, payload)` | Every other box in the game |
| `SendTo(member, topic, payload)` | One box, as `Message::from` named it |

Each returns false when there's no transport, the payload is larger than `MaxPayload()`, or the transport couldn't send
it (no game to send in, or a thread it doesn't send from). `CurrentRole()` says what this box is: `kAlone`, `kHost` or
`kGuest`.

`Writer` writes a payload big endian (`U8`, `U16`, `U32`, `Bytes`), and `Reader` reads it back; a read past the end gives
none and leaves the reader failed, so a message cut short reads as nothing rather than as garbage.

## Threads

Handlers run on the thread the transport delivers frames on, and sends run on the caller's thread; the transport says
which threads it takes sends from. A game whose network is ticked by its game thread wants both there, which also
keeps handlers next to the game's own state.

## The game's side

```cpp
rexfabric::channel::SetTransport({.send = Send, .role = Role, .max_frame = 1252});
rexfabric::channel::Receive(from, frame);  // for each frame the game's stream delivers
```

| Transport member | Is |
|---|---|
| `send(target, member, frame)` | Carries a whole frame to `Target::kHost`, `kAll` or `kMember` (`member` only then); true when it went |
| `role()` | What this box is now |
| `max_frame` | The largest frame, in bytes, the stream takes in one message |

A frame is the topic's hash (4 bytes, big endian) followed by the payload, so a payload can be `max_frame - 4` bytes.
The transport carries frames whole and in order where its stream does; reliability is the stream's.

Viva Pinata: Trouble in Paradise's transport is game message 82 on the game's own reliable message stream (DeTiP,
`src/modding/net/game_channel.cpp`, and its docs/modding.md, The mod channel): 1252 bytes a frame, the game thread only,
and only while a garden is up, where the game handles its garden messages.

## Not yet

* Lua and native mod bindings. The game's own C++ uses the channel today; `rex.channel` for Lua mods is the next step.
* Mod list sharing before a join, which will be a topic of RexFabric's own on top of this.
