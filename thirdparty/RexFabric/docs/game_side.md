# The game's own use of mods

Besides loading mods, RexFabric gives the game's own C++ a few things: hooks on guest functions in the same chains as the mods, the mods' folders for content the game loads itself, texture packs, and a way in for a game that had a mod list of its own.

## Hooking guest functions

Mods hook through the C API. The game's own code works with `PPCContext` anyway, so it hooks with `rexfabric/guest_hooks.h`:

```cpp
rexfabric::HookGuest(0x821F0B18, [](rexfabric::GuestCall& call) {
  BeforeView(call.ctx(), call.base());
  call.Next();
  AfterView(call.ctx(), call.base());
});
```

| Call | Does |
|---|---|
| `HookGuest(address, fn)` | Adds `fn` to the chain on the guest function at `address`. Returns its id, or 0 when the address isn't hookable. |
| `UnhookGuest(id)` | Takes it off again. |
| `call.ctx()`, `call.base()` | The guest's registers and memory. |
| `call.address()` | The hooked function. |
| `call.Next()` | Runs the rest of the chain and the original function. A hook that returns without calling it replaces the function. |

The hook runs on the guest thread making the call. It belongs to `Host()`, so a game hook added after the mods load runs after their hooks on the same function.

Use it where the game needs to run code around a call it doesn't own. `REX_HOOK` still works for replacing a function outright, but only one `REX_HOOK` can hold a function, while chains share it with mods.

## Content from mods

A game that loads files from mods itself (models, data, its own formats) reads the mods folder through these, all in `rexfabric/rexfabric.h`:

| Call | Is |
|---|---|
| `ModsDir()` | The mods folder. |
| `EnabledModDirs()` | The enabled mods' folders, in load order. The first one with a file wins. |
| `InstalledMods()` | Every mod, enabled or not, with its folder in `dir`. |
| `ModsRevision()` | A number that goes up whenever a mod is turned on or off, moved, renamed or installed in this process. A cache of mod content keeps the revision it loaded at and reloads when it changes. |
| `SetModEnabled(folder, enabled)` | Turns a mod on or off, as the menu does. A folder the game just made is on already, as any new folder is; enabling it writes it into the load order and counts as a change, so content caches reload. False when there's no such mod. |
| `RenameMod(from, to, error)` | Renames a mod's folder, keeping its place in the load order. |

Each reads the folder as it is on disk, so they work before `Start` and from any thread. A game tool that writes into a mod (an editor saving its work as a mod, say) creates the folder, then `SetModEnabled` turns it on.

## Texture packs

`rexfabric_attach(my_game WITH_TEXTURES)` adds `rexfabric/textures.h`, for a game whose renderer uploads guest textures itself. A texture is known by a hash of its raw guest bytes, so a file named after the hash replaces that texture wherever it turns up.

```
Dumps         <root>/dumps/textures/<hash16>_<w>x<h>_<format>.dds   (or .png)
Replacements  mods/<mod>/textures/<hash16>.dds or .png              (an earlier mod in the load order wins)
```

The renderer keeps one `TextureReplacement(root)`. For each texture it uploads:

```cpp
const uint64_t hash = rexfabric::TextureReplacement::HashGuestData(bytes, size);
if (REXCVAR_GET(texture_load_enabled)) {
  if (const auto* replacement = textures.FindReplacement(hash)) { UploadRgba8(*replacement); return; }
}
if (REXCVAR_GET(texture_dump_enabled)) textures.DumpTexture(hash, width, height, pitch_blocks, tiled, format, endian, bytes, size);
```

| Call | Does |
|---|---|
| `HashGuestData(data, size)` | XXH3 over the guest bytes, tiled and big endian, as the GPU reads them. |
| `FindReplacement(hash)` | The replacement as RGBA8 with its mip count, or null. Each file is read once and kept. Valid until the next `Rescan`. |
| `DumpTexture(...)` | Untiles, byte swaps and writes the texture. Compressed formats dump as DDS with their raw BC blocks; the rest as RGBA8. |
| `DecodeToRgba8(...)` | The same decode, to memory, for a game tool that shows textures. |
| `Rescan()` | Reads the enabled mods' textures folders again. Call it when `ModsRevision()` changes. |

| Cvar | Default | Does |
|---|---|---|
| `texture_load_enabled` | off | Use replacements. |
| `texture_dump_enabled` | off | Dump textures as they upload. |
| `texture_dump_format` | `dds` | `dds` or `png`. PNG dumps are RGBA8 throughout, with BC formats decoded in software. |
| `texture_dump_skip_video_sizes` | on | Skip dumping 640x360 and 1280x720 textures, the sizes of video frames, which change every frame. |

The renderer checks `texture_load_enabled` and `texture_dump_enabled` itself, as above; `DumpTexture` reads the other two.

Video replacements (`.mp4`) are skipped with a warning: the SDK's FFmpeg has no video decoder. PNG files are read with the SDK's image decoder and written with stb_image_write.

## Moving from a mod list of the game's own

A game that kept its own list of mods before RexFabric hands it over once, in `OnPreSetup` after `Setup`:

```cpp
rexfabric::SeedLoadOrder({"FirstMod", "SecondMod"});
```

When the mods folder has no `mods.json` yet, those folders start enabled in that order and every other one starts off, and it returns true. Names that match no folder are skipped. Once there's a `mods.json` it does nothing and returns false, so the player's own order is never overwritten.
