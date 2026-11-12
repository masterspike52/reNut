# Internals: addresses, structures and reNut's hooks

An appendix for advanced modders working with [hooks and memory](tutorials/hooks-and-memory.md). Everything here is
about Title Update 3 of *Banjo-Kazooie: Nuts & Bolts* as reNut runs it. It comes from reNut's source
(`src/renut_engine/game/*.cpp`, `src/renut_engine/online/net_api.cpp`) and the Online Co-op mod's Lua
(`mods/renut_coop/scripts`), both of which use these values. None of it is a stable API: where a
[`renut` function](renut-api.md) covers what you need, use it instead.

Conventions: addresses are guest addresses; "`+ 128`" is an offset from the start of a structure; all values are
big-endian (`rex.mem` handles that); "heap" means a pointer in `0x40000000`-`0xBFFFFFFF`. Function arguments are
listed in register order (`r3`, `r4`, ...); `&x` is a pointer to `x`.

## Contents

- [Global addresses](#global-addresses)
- [Players and profiles](#players-and-profiles)
- [Flags and counters](#flags-and-counters)
- [Worlds and scenes](#worlds-and-scenes)
- [The challenge manager](#the-challenge-manager)
- [Objects, characters and net objects](#objects-characters-and-net-objects)
- [Vehicles and parts](#vehicles-and-parts)
- [Pads](#pads)
- [Camera](#camera)
- [HUD and UI](#hud-and-ui)
- [Multiplayer sessions](#multiplayer-sessions)
- [Object messages](#object-messages)
- [Useful game functions](#useful-game-functions)
- [Assets, bundles and CAFF](#assets-bundles-and-caff)
- [Functions reNut replaces](#functions-renut-replaces)
- [The underscore functions](#the-underscore-functions)

## Global addresses

| Address | Type | What |
|---|---|---|
| `0x82E1B280` | `u32 x 8` | Player profiles: `+ 4 * n` is player `n + 1`'s profile, `0` without one. Players 2-8 hold a challenge's CPU placeholder profiles during a single-player challenge. |
| `0x82E1B2F6` | `u8` | Player 1 is signed in (the co-op mod saves only then). |
| `0x82E19AB4` | `u32` | The running world (level), or `0`. Set when a world becomes the running one (`0x82289908`). |
| `0x82E18154` | 4 x 32 bytes | The world slots: the app's scene stack. See [Worlds and scenes](#worlds-and-scenes). |
| `0x82E19934` | `u8` | Multiplayer: set while the multiplayer front end (`MultiPlayerMenu_c`) lives, lobby and matches. The challenge code acts as in a match while it's set. |
| `0x82E19924` | `f32` | `g_frameDeltaSeconds`, the game's frame time; `0x82E19920` its copy. |
| `0x82D8BED0` | `u32` | The pause menu's handle, `0xFFFFFFFF` while not paused. |
| `0x82E1B4D0` | `f32 x 16` | The camera's world matrix: rows right, up, forward (or back, see [Camera](#camera)), position (`+ 48`). 16 bytes a row. |
| `0x82E1B6B4` | `f32` | The vertical field of view, degrees. |
| `0x82E1B5F0`, `0x82E182A8` | `u32` | The current camera node; the player's viewport camera node. |
| `0x82E0B158` | `u32 x 4` | `g_padInputs[port]`. See [Pads](#pads). |
| `0x82E0B14C` | `u32` | `g_connectedPadMask`. |
| `0x82E19A60` | `u32` | The UI owner: `+ 72` the handles, `+ 80` the HUD. |
| `0x82E19A70` | | The UI sounds (`uiSystemSfxPlayOperation`'s): `0` select, `1` back, `2` move. |
| `0x82101914` | 20-byte entries | The story worlds by number - 1 (1-8): `+ 0` a pointer to the name ("nuttyacres"). |
| `0x82E10200`, `0x82E10204`, `0x82E1020C`, `0x82E10210`, `0x82E10218` | `u32` | Scene asset ids of the menus, the intro and the garage (the co-op mod treats these as "not a world"). |
| `0x82E10214` | `u32` | The `timeofdaysettings` asset's id. |
| `0x82D8F128` | `u32 x 247` | The game counters' names (`gameCounter_BanjoX_...`), one pointer per counter id. |
| `0x82DF1CF0`, `0x82DF1CF4` | `u32`, pointer | The game flags' names: their count and the array. |
| `0x82DF1CDC` | | The flag table set (counts at `+ 20`, `+ 28`, `+ 36` for the normal, global and volatile tables). |
| `0x82D99454` | `u32 x 448` | The objTag names; an objTag's value is its index + 1. |
| `0x82D9D474` | pointers | The strategy state names (`actorStrategyId_...`), ids from 1001. |
| `0x82D9D5A8` | pointers | The body state names (`actorBodyStateId_...`), id = index. |
| `0x82D99434`, `0x82D99444` | | The engine's class table (ids 0-35) and a pointer to the game's. Each: `{entries, -, first id, end id}`, 12-byte entries `{name, parent id, getter}`. Class 5 is every character. |
| `0x82D99448`, `0x82D9944C` | | The class name map (`{name, id, -}` sorted by name) and its count. |
| `0x82E19E4C`, `0x82E19E50`, `0x82E19E58` | | The garage inventory: begin, end, and the stride per player slot. 64-byte entries `{part aid, ...}`; `+ 36` total, `+ 40` available. |
| `0x82DE8D70` | 128 x `{u32 id, u32 refs}` | The loaded bundles. |
| `0x82E1B754` | | The model pool (`+ 12` its free entries). reNut enlarges it to 4096 entries. |
| `0x82E32F98` | | `entityAvatarComponentCrate`'s vtable (built at run time). |

## Players and profiles

A **profile** (`mem.u32(0x82E1B280 + 4 * n)`) is a player's state, including their save's flags and counters.

| Offset | Type | What |
|---|---|---|
| `+ 89` | `u8` | Set on the game's placeholder profiles ("BanjoHolder"), which a single-player challenge puts in players 2-8 for its CPU racers. |
| `+ 128` | `u32` | The player's slot (and pad port): `0`-`3`; `4` or more for none. |
| `+ 276` | `u32 x 3` | The flag tables (normal, global, volatile): a pointer each to a bit set `{bit count, byte count, data at + 8}`. |
| `+ 308` | `u32` | A pointer to the counters: an `int32` per counter id, at `id * 4`. |
| `+ 324` | `u8` | Progress not saved yet. |

A player's avatar (Banjo) in the running world: `mem.u32(world + 2628 + 8 * slot)`, with `slot` from the profile's
`+ 128`.

Functions:

| Address | Arguments | What |
|---|---|---|
| `0x82285DC8` | (profile) | Its player slot. |
| `0x82357FB8` | (slot) | The match's Player object with that slot, or `0`. |
| `0x822859F0` | (profile, slot) | Maps a profile to a player slot. |
| `0x82286078` | (profile) | Removes a profile's player mapping. |
| `0x82212D18` | (player, 0) | The player's profile. |
| `0x8221A350` | (profile) | Its index (as the HUD's per-player lists use it). |

## Flags and counters

Prefer `renut.game.flag`, `set_flag`, `counter`, `set_counter` and `award`, which take the game's names.

- Normal flags (table 0) are a bit each in the profile's normal bit set, by the game's id.
- `gameFlagsSetFlag` (`0x82332728`): (profile, table, flag, unsaved). Tables `0` and `1` are the saved ones.
- `counterAdd` (`0x824AC1C0`): (profile, counter, amount, quiet). `quiet = 0` shows the HUD's count pop-up.
- `0x824E8FA0`: (profile) marks its progress as changed (not saved).
- `0x824AC030`: (&counter reference) the counter's id, as a challenge's parameters hold one.

Counter ids reNut uses: `57` Jiggies banked (what world doors cost). The old notes also listed `58`-`66` as the Jiggies
won per world (BanjoLand, CPUWorld, JollyDodger, NuttyAcres, SpiralMountain, Terrorium, TrophyThomas, WeirdWest,
WorldOfSport) and `90` as Notes; check them against the names table (`0x82D8F128`) before relying on them.

## Worlds and scenes

### The running world

`mem.u32(0x82E19AB4)`: the world (level) being played, `0` in menus.

| Offset | Type | What |
|---|---|---|
| `+ 0` | `u32` | Its scene's asset id. |
| `+ 88` | `u32` | Its number, from 1 (the story worlds' table above). |
| `+ 0x67C` | `u32` | Its environment: 16 + 448-byte sets, the active set's index at `+ 912`. Fog in a set: mode `+ 112`, colour `+ 144`, near / far / most `+ 192` / `+ 196` / `+ 200`. |
| `+ 1292` | `u32` | Its first object's list node (`object + 56`); a node's `+ 4` is the next. |
| `+ 1744` | `u32` | Its physics manager; `+ 1744 + 8` holds the `hkpWorld` (vtable `0x82055DE8`). |
| `+ 2628` | 8 bytes each | Each player slot's avatar. |
| `+ 3120` | | Its effects. |
| `+ 5552` | `u32` | Its game data / scene object: `0` once it's torn down. |

The scene object (`mem.u32(world + 5552)`):

| Offset | What |
|---|---|
| `+ 464` | Its session. |
| `+ 7008` | The [challenge manager](#the-challenge-manager), inline. |
| `+ 0x1B6C` (7020) | Busy (a cutscene or dialog) while set. |
| `+ 0x4AB8` | Its dialog manager (`+ 744` the talking actor). |
| `+ 19136 + 8` | The crate HUD's parts list. |

### The world slots

`0x82E18154`: four 32-byte slots, the app's scene stack.

| Offset | What |
|---|---|
| `+ 0` | The parent slot's world (in a match: the lobby's). |
| `+ 4` | The world, or `0` for an empty slot. |
| `+ 8` | Non-zero while it's ending. |
| `+ 16` | Its kind: `1` for a match world. |

The co-op mod finds the match world as the slot with a world, not ending, of kind 1.

### Changing worlds

| Address | Arguments | What |
|---|---|---|
| `0x82548110` | (&scene aid, running world, 1, &placement or 0) | `supportGameLoadNewScene`: goes to a scene as a door does once its sequence ends. The id is copied at once. |
| `0x82289810` | (&aid, arg) | A door's or act menu's scene change: ends every loaded world, then loads the new one. |
| `0x82289640` | (world, 0) | Ends a world. |
| `0x82289720` | (parent, &aid, kind, 1, arg, screen) | `appScenePush`: loads a scene in a slot under `parent`. |
| `0x82289908` | () | A world becomes the running one (reNut: `renut.game.on("world_enter")`). |
| `0x82289C30` | (slot) | A slot's world (`+ 4`) is torn down (reNut: `world_leave`). |
| `0x824B2158` | | Returns `&aid` of the scene a match loads (the co-op mod replaces it). |
| `0x82549318` | (world, profile, marker, &Player) | Whether a start marker is the player's. |
| `0x82549250` | (start, &member) | This machine's match member a player start is for. Starts hold their index at `+ 52`. |
| `0x824B1468` | | A match's Initialise: the host adds AI teammates. |
| `0x823EA9F0` | (time-of-day data) | Picks Showdown Town's scene for the time of day (reNut replaces it). |
| `0x82286E48` | (profile) | Applies a loaded save (after a load). |
| `0x824E8F30`, `0x824E9148`, `0x824E9410` | (), (profile, buffer), (profile, data, size) | A save's size; writes a profile's save to a buffer; reads one into a profile. The last two return a `bool`. |
| `0x824E8838` | (profile) | `gameSaveSave`: starts saving; returns whether a save began. |

## The challenge manager

The running challenge manager is inline in the scene object: `scene + 7008`, that is
`mem.u32(mem.u32(0x82E19AB4) + 5552) + 7008`. `renut.game.challenge()` reads its running challenge.

| Offset | Type | What |
|---|---|---|
| `+ 4` | `u32` | Its parameters: a chain of blocks `{type at + 0, size at + 12}`; block type 1 is the main one. In it, `+ 604` is the high-score counter reference and `+ 684` "lower is better". |
| `+ 8` | `u32` | The match's challenge net object (`challengeManNetObj`, its challengeMan at `+ 104`), or `0`. |
| `+ 12` | `u32` | The running challenge's asset id, `0` for none. |
| `+ 16` | `u32` | The next challenge. |
| `+ 20` | `u32` | Its state (below). |
| `+ 96 * slot` | | Each player slot's record (slot from `0x82285DC8`): `+ 1428` score, `+ 1432` time (`f32`), `+ 1444` outcome (`1` won, `2` lost). |
| `+ 632` | `u32` | The stage being played (an id; `0x825015F0` (man, id) finds it). A stage: `+ 164` timed (`u8`), `+ 168` time limit, `+ 172` elapsed (`f32`). |
| `+ 636` | `u32` | The result: `1` won, `2` lost. |
| `+ 652` | | Restart. |
| `+ 712` | | A prize pending. |
| `+ 764` | | The fade. |
| `+ 3044` | `u32` | The timer is held while set (`renut.game.challenge_hold`). |

States (`+ 20`), as reNut and the co-op mod use them:

| State | What |
|---|---|
| `6` | Placing the players at their start markers (`0x824FCFD0`): each slot at the start (type 21) whose index (`+ 52`) is its slot; CPU placeholders at theirs. |
| `7` | Placing the challenge's objects (`0x824FCA88`). |
| `8` | Waiting at the start screen's "ready". |
| `9` | The countdown. |
| `13` | Being played. |
| `14` | Finished, settling: `0x82503568` moves it to `15` once, with player 1's outcome. |
| `15` | Settled. |
| `20` | Restarting. |

States `6` to `17` are "in play" (reNut's challenge music and frame-rate rules use that range).

Functions:

| Address | Arguments | What |
|---|---|---|
| `0x821BD590` | (challengeMan) | `challengeManTick`, once a frame from the level update (`0x82207320`). |
| `0x824FDC38` | (challengeMan, &aid, flag) | A challenge begins; its id is at `+ 12` afterwards (`0` if it didn't). |
| `0x824FEDD8` | (challengeMan, state) | Sets the state. |
| `0x82503568` | (challengeMan) | State 14's settle. |
| `0x824FE248` | (challengeMan, &marker, profile) | The start screen's "ready". |
| `0x82502940` | (challengeMan) | The countdown's "go". |
| `0x824FE2C8` | (challengeMan, restart) | The pause menu's quit or restart. |
| `0x824FC790` | (challengeMan, ...) | A world's end tears its challenge down. |
| `0x824FF2F0` | (challengeMan, bool) | `challengeManTeardown`. |
| `0x824FF188` | (challengeMan, profile) | Keeps a won challenge's best time or score. |
| `0x82500D98` | (challengeMan, profile, &vehicle aid, Banjo, ...) | Builds a slot's vehicle in state 6. |
| `0x823D5808` | (challengeManNetObj) | The match's challenge net object's update. |
| `0x823D7280` | (challengeManNetObj) | Binds it to the match world's challengeMan. |
| `0x82462AF8` | (racer state, vehicle, actor) | A CPU racer's race; `racer state + 116` is set once it crosses the line. |
| `0x825AC150`, `0x825AC490`, `0x825AC780`, `0x825ACCE0`, `0x825AD0B0`, `0x825AD538`, `0x825ADD90`, `0x825AE1E8` | | The challenge start screen (LOG's or the player's choice of vehicle): built, set up, updated, its pages and buttons. |

## Objects, characters and net objects

Every object (entity) in a world:

| Offset | Type | What |
|---|---|---|
| `+ 0` | `u32` | Its vtable (in `0x82000000`-`0x82FFFFFF`); `vtable + 12` handles [object messages](#object-messages). |
| `+ 4` | `u32` | Its class id. |
| `+ 8` | `u32` | Its objTag. |
| `+ 28` | `u32` | Networked (non-zero). |
| `+ 32` (`0x20`) | `u32` | Its net object, or `0`. |
| `+ 36` (`0x24`) | `u32` | Set on another machine's copy. |
| `+ 56` | | Its node in the world's object list. |
| `+ 76` | `u32` | The level (world) it's in. |
| `+ 0x50` | `f32 x 3` | Its position. |
| `+ 0xC0` | `f32 x 4` | Its rotation, a quaternion (x, y, z, w). |
| `+ 0x1A0` | `u32` | Its parent. |
| `+ 0x7C0` | `u32` | Its rigid body (vehicles: the Havok body). |
| `+ 0x7D0` | | Flags. `+ 0x7D4` is set while a built vehicle is inactive (`0x823A5D68` (entity, 1) enables it, `0x823A5D18` shows it). |

A net object: `+ 12` its id (8 bytes, the same on every machine), `+ 19` (`u8`) set once the id is valid. The co-op
mod reads `mem.bytes(net_object + 12, 8)`.

Characters (class 5 and its children: Banjo, NPCs, Gruntbots):

| Offset | What |
|---|---|
| `+ 0xAC0` | Its strategy (what it's doing; state at strategy `+ 60`). |
| `+ 0xAC4` | Its mind (goals, actions). |
| `+ 0xAC8` | Its body (animation states; the current state's id at body `+ 1648`). |
| `+ 0xBE4` (3044) | An avatar's profile (`0` for a respawned Banjo without one). |
| `+ 0xC3C` | An avatar's vehicle (what he drives), or `0`. |
| `+ 0xC54` | The vehicle a character rides. |

An objparams copy, as a level's placement fills it before `0x8236A570` makes the object:

| Offset | What |
|---|---|
| `+ 2` | Its objTag name (64 characters). |
| `+ 75` | Its class name. |
| `+ 132` | Its world. |
| `+ 136` | Its objparams asset id. |
| `+ 140` | Local (not networked) when set. |
| `+ 152`, `+ 164`, `+ 176` | Position, rotation (Euler radians), scale. |
| `+ 452` | A crate's flag name (64 characters). |

Functions:

| Address | Arguments | What |
|---|---|---|
| `0x8236A570` | (class id, objparams copy, net object or 0) | Makes an object; returns it. |
| `0x8224AA08` | (&aid) | A copy of the loaded objparams, or `0`. |
| `0x821E5370` | (&name) | A class id by name, or `0`. |
| `0x82358CD0` | (&objparams aid) | Whether it's in `live_networkenum` (`u8`). |
| `0x82384340` | (object, 0) | Removes an object and what it holds. |
| `0x823D1AD8` | (net object) | Makes another machine's Banjo here (`0`: the game tries again later). |
| `0x825F0368` | (net object) | Makes another machine's vehicle here. |
| `0x8243E2C8` | (controller, Banjo) | Message 56: puts the Banjo at its start. |
| `0x823D0AF0` | (avatar, vehicle or 0, seat part) | `avatarSetVehicle`: an avatar gets in or out. |
| `0x821D23E0` | (actor, &pos, &rotation or 0, 0, 0) | Moves an actor (message 46). |
| `0x82265BD0` | (entity, &name, suffix or 0) | Plays an animation. |
| `0x82224670` | (entity, &euler radians[3], 1) | Sets a rotation. |
| `0x822B0BC8` | (entity, &model aid, world or 0) | Makes an entity's model. |
| `0x823A3140` | (actor) | Defeats a character. |
| `0x82279920`, `0x82279988` | (body, f1 health), (body, f1 amount) | Revives (health and max), damages a body. |

## Vehicles and parts

A vehicle's address is what `v.address` holds; a part's is `part.instance`. From reNut's part API.

**Vehicle**

| Offset | Type | What |
|---|---|---|
| `+ 76` | `u32` | Its level. |
| `+ 0x50` | `f32 x 3` | A position the game keeps on it. |
| `+ 0x7C0` | `u32` | Its Havok body, `0` while it's rebuilt. The body: `+ 8` its `hkpWorld`, `+ 0x2C` collision filter info, `+ 0xD0` its `hkpMotion`, `+ 0x110` its motion state (position; rotation columns at `- 0x30`; velocity `+ 0x90`, angular velocity `+ 0xA0`; inverse mass `+ 0x8C`). |
| `+ 0xAAC` | `f32` | Its frame time. |
| `+ 0xAC0`, `+ 0xAC4` | `u32` | Its drive units: count, then the array. A unit's `+ 0x9C` points at its input: steer, stick y, brake, accelerate (`f32` from `+ 8`), braking (`u8` at `+ 24`). |
| `+ 0xD0C`, `+ 0xD10`, `+ 0xD18`, `+ 0xD20` | `f32`, `f32`, `f32`, `u32` | Max health, health, health fraction, damage level. |
| `+ 0xEA0`, `+ 0xEAC`, `+ 0xEC4` | 3 each | Resources (fuel, ammo, a third): most, current (`f32`), infinite (`u32`). |
| `+ 0x1070` | `s32` | Disabled while above 0. |
| `+ 0x1074` | `u32` | The stick is flipped while set. |
| `+ 0x1498` | `u32 x 2` | Its parts: begin and end of 176-byte entries, the part at `+ 4`. |
| `+ 0x1830` | UTF-16 x 64 | Its name. |
| `+ 0x18E0` | `u32` | Flags: `0x10` invulnerable. |

**Part**

| Offset | Type | What |
|---|---|---|
| `+ 0x90` | `f32` rows | Its world matrix (rows right, up, forward; unconfirmed). |
| `+ 0xC0` | `f32 x 3` | Its world position (unconfirmed). |
| `+ 0xEC` | `u32` | Grid cell: x 11 bits, y 10, z 11, signed. |
| `+ 0xF0` | `u32` | Orientation, `0`-`23`. |
| `+ 0x26C`, `+ 0x270`, `+ 0x274`, `+ 0x280`, `+ 0x284` | | Max health, health, health fraction (`f32`), health changed, damage level. |
| `+ 0x380` | `u32` | Its model instance. `+ 0x3A0` its model's asset id. |
| `+ 0x388` | `u32` | Its paint, `0xRRGGBBAA`. |
| `+ 0x3AC` | `u32` | Its record's first control: the action button (`2` A, `3` B, `4` X, `0` none). |
| `+ 0x3DC` | `u32` | Its objparams asset id. |
| `+ 0x46C` | `u32` | Its vehicle, or `0`. |

**Weapon parts** (`vehicleBlockWeapon`): `+ 0x48C` trigger held (`u32`); `+ 0x668` pitch and `+ 0x670` yaw (radians);
`+ 0x690` pitch limits (two `f32` degrees); `+ 0x698` ammo per shot; `+ 0x6FC` projectiles made; `+ 0x700` a pool of
up to 30 projectiles; `+ 0x7E0` and `+ 0x7F0` muzzle position and direction, valid once `+ 0x854` is set.

Functions: `0x825D9C68` damage (vehicle, f1 amount, -, part or 0, attacker), `0x825D9F80` set health (vehicle, f1),
`0x825D9EB0` repair (vehicle), `0x82246278` use a resource (vehicle, kind, f1 amount), `0x8254B988` the driver
(vehicle), `0x82201350` the driver's pad (vehicle, 0), `0x821D3648` part damage (part, f1 amount), `0x82616198` break
a part off (part, 1, 0), `0x8260FC20` / `0x8260FD50` a part's button down / up (part, vehicle, control),
`0x8260AE08` paint (part, &rgba, effect), `0x825F6540` seat an actor (vehicle, actor, seat 2/4/8, part, 1),
`0x8254CAD0` / `0x8254CEF0` build a vehicle from a blueprint (builder, world, &pos, &quat) / (builder),
`0x8295DAE0` `hkpEntity::activate` (body), `0x82960310` `hkpWorld::castRay` (world, &input, &output).

## Pads

`mem.u32(0x82E0B158 + 4 * port)` is a port's input; `+ 52` its record. The record: `+ 0` the current entry's index
(`0` or `1`), then two 40-byte entries from `+ 4`. The player's port is their profile's `+ 128`. `padStatePush`
(`0x821CB628`, replaced by reNut) makes an entry current.

An entry:

| Offset | Type | What |
|---|---|---|
| `+ 0` | `u32` | Buttons pressed this frame. |
| `+ 4` | `u32` | Buttons released this frame. |
| `+ 12` | `u32` | Buttons held. |
| `+ 16`, `+ 20` | `f32` | Left stick x, y. |
| `+ 24`, `+ 28` | `f32` | Right stick x, y. |
| `+ 32`, `+ 36` | `f32` | Left and right triggers. |

Button bits (XInput's): d-pad up `0x0001`, down `0x0002`, left `0x0004`, right `0x0008`, Start `0x0010`, Back
`0x0020`, left stick `0x0040`, right stick `0x0080`, LB `0x0100`, RB `0x0200`, A `0x1000`, B `0x2000`, X `0x4000`,
Y `0x8000`. `renut.pad` reads these.

## Camera

- `0x82E1B4D0`: the camera's world matrix, built each frame from the current camera node by `meCalculateView`
  (replaced by reNut). Rows right, up, third, position. Whether the third row points where the camera looks or
  behind it depends on the node's quaternion; reNut works it out from gameplay frames (`renut.camera.facing_known()`).
- A camera node: position `+ 16`, rotation quaternion (x, y, z, w) `+ 64`, flags `+ 168` (bit 0: the quaternion isn't
  used).
- `0x82E1B6B4`: the vertical field of view, written by `0x821E4540` (lens, &fov degrees, f1 focal), which reNut
  replaces for its field-of-view setting and `renut.camera.set_fov`.
- `0x821CD438` (player, -, avatar, world): the player's camera each frame, on foot or driving (replaced).

## HUD and UI

- The HUD: `mem.u32(mem.u32(0x82E19A60) + 80)`. Its `+ 4` / `+ 8` are a list's begin and end; each player's
  container comes from `0x82228A88` (hud, last, profile). A HUD's `+ 232` is its camera.
- Message box: `0x8255A948` (profile, &UTF-16 text, 0) shows one without buttons; `0x8255AAE0` (profile, text,
  A key, callback, arg, B key, callback, arg) with two; `0x8255AD38` (profile, 1) closes it. A box: `+ 192` has two
  choices, `+ 196` / `+ 204` the arguments, `+ 240` / `+ 244` the buttons, `+ 336` swapped.
- Score/time panel: `0x825B2858` (container, profile, &params[13]) opens it; params `+ 0` show the time, `+ 4` show
  the score, `+ 24` score, `+ 28` time, `+ 44` thresholds. `0x8225E4E8` (hud, profile) its scene; `0x825AF8B0`
  (scene, f1 seconds), `0x825AF970` (scene, value) set it; `0x82553E88` (profile) closes it.
- The game's text is UTF-16, big-endian (`rex.mem.wstring`). `0x82A25780` (element, &text) sets an XUI text element.
- `0x82247D30` (handles, handle) a HUD by handle; `0x82260128` (HUDs, profile, sequence) a profile's HUD.
- The pause menu opens with `0x8255B428` and closes with `0x8255B598` (both replaced; `renut.game.on("paused")`).
- XUI lists: `0x823B2130` (list, count) inserts rows; `0x823B1EC8` fills the shown rows each frame; `0x821EED78` the
  selection moved; `0x823B3240` / `0x823B32F8` a scene's presses (all replaced for `renut.menu`). A list object:
  `+ 4` its handle, `+ 8` its rows `{u16 row size, begin, end}`; a row's text at `+ 8` (128 UTF-16 characters).

## Multiplayer sessions

From `net_api.cpp`:

| Address | What |
|---|---|
| `0x82D99B54` | The network has started (`u8`). |
| `0x82D99B58` | The match's session (net system "Main"), or `0`. |
| `0x82D99B5C` | The lobby's party session, or `0`. |
| session `+ 336` | Its state: `5` once it's up. |
| session `+ 4580` | This machine's peer; peer `+ 36` set on the host. |
| `0x82E29324` | The connections: a `std::map`'s head (`+ 4` the root; a node: `+ 0` left, `+ 8` right, `+ 16` value, `+ 21` nil flag). A connection's `+ 56` is its address (what `renut.net.peers()` lists). |
| `0x824C44D8` | () the match manager: `+ 147` co-op, `+ 189` Live. |
| `0x825A15E0` | (StartMenu, live) goes to multiplayer, as the main menu does. |
| `0x82158240` | The main menu scene's (`StartMenu`) vtable. |

reNut's `renut.net` uses its own UDP socket on the title's port (1500, shifted for each copy on one PC). A packet is
`"RNUT"`, a version byte (`1`), the name's length, the name, then the data; at most 1024 bytes.

## Object messages

The game talks to objects with messages. `0x822107C8` (message, id) clears a message to its size and sets its id;
then the object's vtable `+ 12` handles it: `rex.call(mem.u32(mem.u32(entity) + 12), entity, message)`. Ids and sizes
come from the table at `0x821051F8` (`{name, size}` by id). reNut uses a 128-byte buffer, with data from `+ 16`.

| Id | Name | Size | What |
|---|---|---|---|
| `46` | | | Moves an actor (`0x821D23E0` sends it). |
| `56` | | | Puts a Banjo at its start (`0x8243E2C8`). |
| `148` | `objMsgId_Object_GetIsContactingFloor` | 8 | The answer at `+ 4`. |
| `150` | `objMsgId_Object_GetScale` | 8 | `f32` at `+ 4`. |
| `151` | `objMsgId_Object_GetVelocity` | 32 | A vec4 at `+ 16`. |
| `165` | `objMsgId_Banjo_Actor_ApplyVelocityAccumulator` | 48 | A push. |
| `173` | `objMsgId_Banjo_Actor_KnockedOutOfVehicle` | 4 | Its driver gets out. |
| `252` | | | Collected (a collectable's `+ 3008` goes from 0 to 1). |

```lua
-- Ask an object whether it's standing on the floor (message 148).
local INIT_MESSAGE = 0x822107C8
local message = rex.call(0x82332A08, 128, 0)   -- once; keep it
local function on_floor(entity)
  rex.mem.set_bytes(message, string.rep("\0", 128))
  rex.call(INIT_MESSAGE, message, 148)
  rex.call(rex.mem.u32(rex.mem.u32(entity) + 12), entity, message)
  return rex.mem.u8(message + 4) ~= 0
end
```

`renut.actors.on_ground` does this for you.

## Useful game functions

| Address | Name | Arguments | What |
|---|---|---|---|
| `0x82332A08` | `meInternalAlloc` | (size, 0) | Allocates guest memory; returns it or `0`. |
| `0x82332C30` | `meInternalFree` | (pointer) | Frees it. |
| `0x8222E6F0` | `assetManOpenParams` | (&aid, open) | An asset's data, or `0` (replaced by reNut, `rex.call` works). |
| `0x82204DF0` | | (data) | Releases what `assetManOpenParams` opened. |
| `0x8228F0F0` | | (data) | Its size. |
| `0x82332728` | `gameFlagsSetFlag` | (profile, table, flag, unsaved) | Sets a flag. |
| `0x824AC1C0` | `counterAdd` | (profile, counter, amount, quiet) | Adds to a counter. |
| `0x824E8838` | `gameSaveSave` | (profile) | Starts a save. |
| `0x82548110` | `supportGameLoadNewScene` | (&aid, running world, 1, &placement or 0) | Goes to a scene. |
| `0x82554410` | | (HUD list, flag, show contents) | Awards a crate. |
| `0x8228BAC8` | | (owner world, &stream bundle aid, wait) | Loads a stream bundle. |
| `0x823F0890` | | | Starts a dialog. |
| `0x82211450` | | (cue record) | Whether a sound cue plays (`u8`). |
| `0x82207320` | | | The level's update, once a frame while a level runs (not replaced). |

## Assets, bundles and CAFF

**Asset ids.** The game names assets by a 32-bit id: the asset type in the top byte, and the low 24 bits of the
CRC-32 of the name without its `aid_<type>_` prefix, without CRC-32's final inversion:

```lua
local function asset_id(asset_type, name)
  local crc = 0xFFFFFFFF
  for i = 1, #name do
    crc = crc ~ name:byte(i)
    for _ = 1, 8 do crc = (crc & 1 ~= 0) and ((crc >> 1) ~ 0xEDB88320) or (crc >> 1) end
  end
  return (asset_type << 24) | (crc & 0xFFFFFF)   -- no final inversion
end

print(string.format("0x%08X", asset_id(0x4F, "banjox_common")))  -- aid_bundle_banjox_common
```

Types reNut uses: `0x04` model, `0x19` script (a scene), `0x1F` objparams (parts, doors, projectiles, objects),
`0x3D` challenge, `0x43` dialog, `0x44` chardata, `0x4F` data bundle, `0x50` stream bundle. `renut.game.challenge_id`
and NutTool compute ids for you; NutTool shows every asset's id (see [NutTool](nuttool/README.md)).

**Bundles.** The game's data is in `Bundle/4f/<id>` (resident data bundles, type `0x4F`) and `Bundle/50/<id>`
(stream bundles, type `0x50`), where `<id>` is the low 24 bits in six lowercase hex digits. The common bundle is
`0x4F685374` (`aid_bundle_banjox_common`) with stream bundle `0x50685374`. Each world loads its own bundles and unloads
them with it. A mod's `bundles/Bundle/4f|50/<id>` files are served as new game files and loaded with the common
bundle; its `worlds/Bundle/...` files are only served, for its worlds' scenes to load. A mod can't replace a bundle
the game has.

**CAFF.** A data bundle is a CAFF archive (`"CAFF07.08.06.0036"`), usually wrapped in Rare's LZX container (magic
`0x0FF512ED`). A CAFF holds named assets (symbols `aid_<type>_<name>,<timestamp>,<version>`), with a section table,
relocations and section data (`.data`, `.texturegpu`, `.gpu`, `.gpucached`, `.stream`); pointers inside are offsets
fixed up on load. A stream bundle (magic `0x438CB47C`) is a table of `(aid, offset, size)` entries, each an
uncompressed CAFF or a wave bank. Use NutTool or reNut's modtool to build them rather than writing them by hand.

## Functions reNut replaces

reNut replaces about 500 of the game's functions with its own C++ (`REX_HOOK` and `REX_HOOK_RAW` in its source). Most
(about 370) are the Direct3D layer and the renderer. A `rex.hook` or `rex.hook_after` on a replaced function is
accepted but **never runs**; `rex.call` on one runs reNut's version.

### The ones mods' hooks still run on

| Address | What | Mods' hooks |
|---|---|---|
| `0x821BD590` | `challengeManTick` | Around the game's tick; reNut's music and `renut.game_tick` after. |
| `0x824FDC38` | A challenge begins | Around the game's; `challenge_start` after. |
| `0x82503568` | A challenge settles | Around the game's; `challenge_finish` after. |
| `0x8236A570` | Makes an object | Around the game's; reNut learns the object's kind before and fires `object_spawn` after. |
| `0x824E8838` | `gameSaveSave` | Around the game's; `game_save` after a save began. |
| `0x824AC1C0` | `counterAdd` | Around all of reNut's hook (custom NPCs' jiggies): skipping it skips reNut's too. |

### Behind the renut events

| Address | What | Event |
|---|---|---|
| `0x825DBA20` | A vehicle's init | `renut.vehicle.on("spawn")` |
| `0x825EF480` | A vehicle leaves its level | `vehicle "removed"` |
| `0x821ECA98` | A vehicle's update after its parts' physics | `vehicle "tick"` |
| `0x825D9C68` | Vehicle damage | `vehicle "damage"` (the amount can be changed) |
| `0x825D9F80` | A vehicle's health is set | `vehicle "health"` |
| `0x825ED3E0` | A part goes onto a vehicle | `vehicle "part_added"` |
| `0x825EDDA0` | A part comes off | `vehicle "part_removed"` |
| `0x82216C78` | Which side a hit came from (the frame's strongest, from `0x8223F074`) | `vehicle "collide"` |
| `0x823D0AF0` | `avatarSetVehicle`: in or out | `vehicle "enter"`, `"exit"` |
| `0x821D3648` | Part damage | `renut.parts.on("damage")` |
| `0x82616198` | A part breaks off | `parts "break"` |
| `0x8260FC20`, `0x8260FD50` | A part's button down, up | `parts "use"`, `"release"` |
| `0x82289908` | A world becomes the running one | `renut.game.on("world_enter")` |
| `0x82289C30` | A world is torn down | `game "world_leave"` |
| `0x8255B428`, `0x8255B598` | The pause menu opens, closes | `game "paused"`, `"resumed"` |
| `0x823CD070` | A hit on a character's body | `renut.actors.on("hit")` |
| `0x823A31B8` | A character dies | `actors "defeated"` |
| `0x824400C0` | A collectable's tick | `renut.objects.on("collected")` |
| `0x82384340` | An object is removed | `objects "removed"` |
| `0x82554410` | A crate's award | `renut.crates.on("award")` |
| `0x825CA310` | A message box's button | `renut.hud.on("choice")` |
| `0x823B2130`, `0x823B1EC8`, `0x821EED78`, `0x823B3240`, `0x823AFCC0`, `0x823B32F8` | XUI lists and presses | `renut.menu` rows |
| `rex_appMainTickPreDraw` | The frame | `renut.net.on_tick` |

`renut.game.on("tick")`, `"load"`, `renut.player.on("state")`, zones and the HUD's timer are driven from the
`challengeManTick` hook.

### Other gameplay functions reNut replaces

| Area | Addresses |
|---|---|
| Camera and view | `0x821CD438` (the player's camera), `rex_meCalculateView` (the view), `0x821E4540` (field of view), `0x822C0448`, `0x821B42B8`, `0x821B4050`, `0x821B4128` (mouse camera input) |
| Input | `0x821CB628` (`padStatePush`), `XInputGetState`, `XInputGetKeystroke`, `XInputSetState` |
| Challenges | `0x824FF2F0` (teardown), `0x825AA5B0`, `0x825A75C8`, `0x825A6DB0` (results screen, replays) |
| Worlds and NPCs | `0x82364768` (a scene's markers), `0x8240E860` (door open state), `0x825CB1A0` (door stats panel), `0x82385678` (water), `0x822D7248`, `0x822D6780` (Havok meshes), `0x822237D0` (name lookups), `0x823F05B0` (a scene's character table), `0x823ED588`, `0x823EFCA0` (dialog lines), `0x82227CC0` (a character's body tick), `0x823EA9F0` (Showdown Town's scene for the time of day) |
| Assets and parts | `0x8222E6F0` (`assetManOpenParams`), `0x82204DF0`, `0x8228F0F0`, `0x8228ECA0`, `0x82241780`, `0x8224AA08`, `0x8254FAC0`, `0x824FAF08`, `0x8260AEE8`, `0x8260EFC8`, `0x824FA3D8` (garage inventory), `0x82363A50` (text lookup), `0x824FA620`, `0x82554288` (crate contents), `0x822B0BC8` (entity models), `rex_assetDbBundleLoadBundle`, `0x8228BAC8` (bundle loads) |
| Vehicles | `0x82201350`, `0x82200AA0`, `0x82618A18`, `0x8222E850`, `0x8223EBF0` (drivers and seats), `0x82633BA0`, `0x8254CEF0`, `0x8250FD28` (build limits), `0x8233FA80` (steering), `0x82353060` (the physics step) |
| Stereo and sound | `0x82261608`, `0x8261FE38`, `0x8261FEA8`, `0x8261FB60`, `0x82B4D518`, `0x82620128`, `0x8261FA98`, `0x8264EE20` |
| Menus | `0x82560958`, `0x82560C58`, `0x82560DF8`, `0x8258DA40`, `0x825A0F98`, `0x825613A8`, `0x82561960`, `0x82562CE8`, `0x8258C948`, `0x8258C7C0`, `0x8258B948`, `0x8258D038`, `0x825A0458`, `0x823B33E8`, `0x825D7F28`, `0x8259FDA8` (pause menu and settings pages), `0x823B7B50` (front end scene open) |
| Other | `0x82283BB0` (main loop pass), `0x82227F18` (`bodyGroundSlope`), `0x8220F350` (thread slots), `0x823333F0`, `0x8225B118`, `0x82333240` (level pools), `0x82BC3F98`, `0x8238D650`, `0x8238D8C8` (LOD), `0x82513DB8` (a world's messages), `0x826A2DA8` (session contexts), `0x825160B0` (photo capture) |

reNut also replaces the startup movie's player functions, the Guide's string check (`XStringVerify`), the Xam
allocators and the widescreen layout functions. To check an address not listed here, search reNut's source for
`sub_<address>` (or its `rex_` name in `config/renut_funcs.toml`) next to `REX_HOOK`.

## The underscore functions

The public `renut` functions are Lua written on top of these. They take addresses and plain values, skip the checks
the public functions make, and may change between reNut versions. Those that call into the game only work inside a
renut event (elsewhere they wait for the next game tick). Use them only for something the public API doesn't offer.

| Table | Functions |
|---|---|
| `renut.parts` | `_define`, `_name`, `_count`, `_model`, `_attach`, `_field`, `_field_text`, `_type`, `_block`, `_behaviour`, `_commit` (adding a part); `_count_all`, `_info`, `_lua_def`; `_vehicle_of`, `_class`, `_is_a`, `_aid`, `_aid_of`, `_id_of`, `_position`, `_cell`, `_orientation`, `_health`, `_set_health`, `_damage`, `_break`, `_press`, `_release`; `_fire`, `_fire_as`, `_can_fire_as`, `_projectile_aid`, `_hold`, `_muzzle`, `_turret`, `_aim`, `_axis`, `_bone_count`, `_bone`, `_shape_index`, `_shape`, `_set_shape`, `_paint`, `_set_paint`, `_owned` |
| `renut.vehicle` | `_has_body`, `_get`, `_set`, `_inverse_mass`, `_force`, `_force_at`, `_impulse_at`, `_angular_impulse`, `_input`, `_driver`, `_health`, `_set_health`, `_repair`, `_damage`, `_disabled`, `_invulnerable`, `_set_invulnerable`, `_resource`, `_set_resource`, `_set_infinite`, `_use`, `_name`, `_set_name`, `_level`, `_part_count`, `_part`, `_in_air`, `_list`, `_blueprints`, `_spawn`, `_seat`, `_remove`, `_rotation`, `_set_rotation`, `_scale_mass` |
| `renut.game` | `_world`, `_world_name`, `_scene`, `_challenge`, `_challenge_id`, `_challenge_start`, `_challenge_end`, `_challenge_time`, `_challenge_hold`, `_award`, `_flag`, `_set_flag`, `_counter`, `_set_counter`, `_paused`, `_set_paused`, `_save`, `_time_scale`, `_set_time_scale`, `_in_event` |
| `renut.player` | `_avatar`, `_avatar_of`, `_vehicle`, `_teleport`, `_play`, `_set_model`, `_get` |
| `renut.actors` | `_list`, `_find`, `_spawn`, `_place`, `_turn`, `_play`, `_state`, `_set_state`, `_body_state`, `_body_now`, `_action`, `_walk_to`, `_stop`, `_remove`, `_vehicle`, `_talk`, `_health`, `_set_health`, `_damage`, `_defeat`, `_follow`, `_unfollow`, `_target`, `_model`, `_velocity`, `_push`, `_on_ground`, `_scale`, `_exit_vehicle`, `_yaw` |
| `renut.objects` | `_kinds`, `_list`, `_spawn`, `_remove`, `_kind`, `_is_character`, `_position`, `_move`, `_turn`, `_yaw` |
| `renut.camera` | `_get(row, axis)` (the matrix), `_fov`, `_facing`, `_set`, `_release`, `_set_fov`, `_shake` |
| `renut.mouse` | `_get(which)`: `0` x, `1` y, `2` the window's aspect, `3` inside |
| `renut.pad` | `_get(player, which)`: `0` held, `1` pressed, `2` released (button bits); `3`-`6` the sticks; `7`, `8` the triggers |
| `renut.world` | `_ray`, `_ray_past`, `_ray_level` (the hit fraction, `1` for none), `_normal(axis)` (the last hit's), `_fog`, `_fog_get`, `_env`, `_set_env` |
| `renut.hud` | `_message`, `_close`, `_choice`, `_panel`, `_panel_close`, `_timer`, `_time`, `_shown`, `_counter` |
| `renut.sound` | `_play`, `_set`, `_stop`, `_pitch`, `_playing`, `_music`, `_cue`, `_cue_stop`, `_cue_playing` |
| `renut.menu` | `_add`, `_set_text`, `_remove`, `_lists`, `_reset` |
| `renut.net` | `_send(peer, name, data)` (data already encoded), `_peers`, `_state`, `_is_host`, `_multiplayer`, `_open_multiplayer` |
| `renut.crates` | `_list`, `_contents`, `_swap`, `_give`, `_in_world`, `_spawn` |
| `renut.zones` | `_add`, `_move`, `_remove`, `_list`, `_count`, `_inside` |
| `renut.fx` | `_play` |
| `renut.worlds`, `renut.doors`, `renut.npcs` | `worlds._add`, `_count`, `_info`, `_go`, `_player_model`, `_stat`, `_water`; `doors._add`; `npcs._add`, `_text`, `_flag`, `_jiggies`, `_challenge`, `_count`, `_info`, `_spawn`, `_place`, `_turn`, `_play`, `_voice`, `_clip`, `_enemy`, `_get`, `_progress`, `_set_health`, `_defeat` |
| `renut.materials`, `renut.shapes` | `materials._define`, `_count`, `_index`, `_name`, `_set`, `_set_emissive`, `_reset`; `shapes._define`, `_count`, `_index`, `_name`, `_get`, `_set` |
| `renut.data`, `renut.mods` | `data._get`, `_set`, `_has`, `_erase`, `_keys`, `_save_id`; `mods._list` |

Two things only they offer: a turret's pitch limits in degrees, `renut.parts._turret(part.instance, 2)` and
`(..., 3)`; and `renut.net._send`, which sends a string you encoded yourself.
