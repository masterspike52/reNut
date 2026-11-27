#include "scriptgraph.hpp"

namespace nut::scriptgraph {

// Every node, one a line: kind | id | category | title | inputs | outputs | lua | help.
// Kinds: E event, C part callback, A action (white wire in and out), P pure (values only), F flow
// (its own white wires), D part definition. Pins: name:type[a/b]=default, "!" a field, "?" optional, [a/b] its dropdown.
// Types: exec any bool num str vec part vehicle npc table actor place. Lua places: see scriptgraph.cpp's expand.
extern const char* const kNodeSpec = R"spec(
# ---- Events
E|event.load|Events|On script load|||do\n  {_body}\nend|Runs once, when the script loads (and on Reload Scripts).
E|event.frame|Events|Every frame||dt:num|rex.on("frame", function({_params})\n  {_body}\nend)|Every frame, on every screen (menus and the garage too); dt is seconds since the last. Actions marked Event wait here for the game's next tick in a level, then go on.
E|event.key|Events|Key pressed|!key:str[F6/F7/Z/X/C/V/Space/Return/Numpad1/Numpad2]=F6||rex.on("frame", function()\n  local pressed = rex.input.pressed({key})\n  if pressed and rex.input.accepts() then\n    {_body}\n  end\nend)|When a key is pressed (names: A-Z, F1-F24, Numpad0-9, Space, Left...). Actions marked Event wait for the game's next tick, so Key pressed then Spawn character works as is.
E|event.every|Events|Every N seconds|!seconds:num=1||local __every{_id} = 0\nrex.on("frame", function(dt)\n  __every{_id} = __every{_id} + dt\n  if __every{_id} >= {seconds} then\n    __every{_id} = __every{_id} - {seconds}\n    {_body}\n  end\nend)|Repeats every so many seconds, from the frame. Actions marked Event wait for the game's next tick.
E|event.cvar|Events|Setting changed|!setting:str=mymod_setting|name:str,value:any|rex.cvar.on_change({setting}, function({_params})\n  {_body}\nend)|After a setting (cvar) changes, on the next frame.
E|event.raw|Events|Raw event|!event:str=renut.vehicle_tick|a1:any,a2:any,a3:any,a4:any|rex.on({event}, function({_params})\n  {_body}\nend)|Any rex.on event by name, its first four arguments.
E|event.hook|Events|Before a game function|!address:num=0x82000000|ctx:table|rex.hook({@address}, function({_params})\n  {_body}\nend)|Runs before the game's function at the address (ctx: its registers). A Lua node returning false skips it.
E|event.hook_after|Events|After a game function|!address:num=0x82000000|ctx:table|rex.hook_after({@address}, function({_params})\n  {_body}\nend)|Runs after the game's function at the address returns: its return value is ctx.r3 (or ctx.f1).
# ---- The game's events
E|game.on_world_enter|Game events|World entered||number:num,name:str,scene:num|renut.game.on("world_enter", function({_params})\n  {_body}\nend)|A world (or act) has loaded and is being played: its number, game name (nuttyacres) and scene's asset id.
E|game.on_world_leave|Game events|World left||number:num,name:str,scene:num|renut.game.on("world_leave", function({_params})\n  {_body}\nend)|The world is going (to another, or to the menus).
E|game.on_challenge_start|Game events|Challenge started||challenge:num|renut.game.on("challenge_start", function({_params})\n  {_body}\nend)|A challenge begins (its asset id).
E|game.on_challenge_finish|Game events|Challenge finished||challenge:num,won:bool,outcome:num|renut.game.on("challenge_finish", function({_params})\n  {_body}\nend)|Its outcome is settled: won for player 1; outcome 1 won, 2 lost, 0 neither.
E|game.on_paused|Game events|Game paused|||renut.game.on("paused", function()\n  {_body}\nend)|The pause menu opens.
E|game.on_resumed|Game events|Game resumed|||renut.game.on("resumed", function()\n  {_body}\nend)|The pause menu closes.
E|game.on_tick|Game events|Game tick|||renut.game.on("tick", function()\n  {_body}\nend)|Once a frame while a level runs, on foot or driving, inside the game's tick: everything works here.
E|crates.on_award|Game events|Crate given||flag:str,contents:table|renut.crates.on("award", function({_params})\n  {_body}\nend)|A crate is being given, before its parts are added (a Swap crate here still changes this one). contents: a list of {aid, count}.
E|objects.on_collected|Game events|Pickup collected||object:num,kind:str|renut.objects.on("collected", function({_params})\n  {_body}\nend)|A collectable (a challenge's pickups) was collected.
E|actors.on_hit|Game events|Character hit||actor:actor,attacker:num,damage:num|renut.actors.on("hit", function({_params})\n  {_body}\nend)|Any character (Banjo too) takes a hit.
E|actors.on_defeated|Game events|Character defeated||actor:actor,killer:num|renut.actors.on("defeated", function({_params})\n  {_body}\nend)|Any character dies, however.
E|zones.on_enter|Game events|Zone entered|!name:str|zone:str,object:num,what:str|renut.zones.on("enter", function(...)\n  local {_params} = ...\n  if {name} ~= "" and {name} ~= (...) then return end\n  {_body}\nend)|Something a zone watches came in (what: banjo, vehicle or character). Name a zone to only run for it; empty for every zone.
E|zones.on_exit|Game events|Zone left|!name:str|zone:str,object:num,what:str|renut.zones.on("exit", function(...)\n  local {_params} = ...\n  if {name} ~= "" and {name} ~= (...) then return end\n  {_body}\nend)|Something a zone watches went out. Name a zone to only run for it; empty for every zone.
E|hud.on_timer_done|Game events|Timer done|||renut.hud.on("timer_done", function()\n  {_body}\nend)|The HUD's countdown (Show timer, run down) reached 0.
E|hud.on_choice|Game events|Choice answered||choice:num,confirmed:bool|renut.hud.on("choice", function({_params})\n  {_body}\nend)|Any choice box was answered (Ask gives its number). Ask's own wires are simpler.
E|game.on_save|Game events|Game saved|||renut.game.on("save", function()\n  {_body}\nend)|The game has saved player 1's progress.
E|game.on_load|Game events|Save loaded|||renut.game.on("load", function()\n  {_body}\nend)|Player 1's save is up (loaded, or a new one begun): the place for Get saved value.
E|player.on_state|Game events|Player state changed||state:str,old:str|renut.player.on("state", function({_params})\n  {_body}\nend)|Banjo's body state changed (Jump, Land, Swim...): the new one and the one before.
E|player.on_jump|Game events|Player jumped||state:str|renut.player.on("jump", function({_params})\n  {_body}\nend)|Banjo jumps (from the ground, water, a pole or at a vehicle): the jump's state.
E|player.on_land|Game events|Player landed||state:str|renut.player.on("land", function({_params})\n  {_body}\nend)|Banjo lands from a jump or a fall: the landing's state.
E|objects.on_spawn|Game events|Object spawned||object:num,kind:str,character:bool|renut.objects.on("spawn", function({_params})\n  {_body}\nend)|Any object (a character too) was made in the world: its kind and whether it's a character.
E|objects.on_removed|Game events|Object removed||object:num,kind:str|renut.objects.on("removed", function({_params})\n  {_body}\nend)|Any object is leaving the world.
E|actors.on_spawn|Game events|Character spawned||actor:actor,kind:str|renut.actors.on("spawn", function({_params})\n  {_body}\nend)|A character was made in the world (Spawn character's too).
# ---- Vehicle events
E|vehicle.on_spawn|Vehicle events|Vehicle spawned||vehicle:vehicle|renut.vehicle.on("spawn", function({_params})\n  {_body}\nend)|A vehicle was made. Its parts are added after it (Part added).
E|vehicle.on_removed|Vehicle events|Vehicle removed||vehicle:vehicle|renut.vehicle.on("removed", function({_params})\n  {_body}\nend)|A vehicle is leaving the level.
E|vehicle.on_tick|Vehicle events|Vehicle tick||vehicle:vehicle,dt:num|renut.vehicle.on("tick", function({_params})\n  {_body}\nend)|Every frame for every vehicle, after its update.
E|vehicle.on_damage|Vehicle events|Vehicle damaged||vehicle:vehicle,amount:num,part:part,attacker:num|renut.vehicle.on("damage", function({_params})\n  {_body}\nend)|A vehicle is about to take damage; Set damage amount changes it.
E|vehicle.on_health|Vehicle events|Vehicle health set||vehicle:vehicle,health:num|renut.vehicle.on("health", function({_params})\n  {_body}\nend)|A vehicle's health was set.
E|vehicle.on_part_added|Vehicle events|Part added||vehicle:vehicle,part:part|renut.vehicle.on("part_added", function({_params})\n  {_body}\nend)|A part went onto a vehicle.
E|vehicle.on_part_removed|Vehicle events|Part removed||vehicle:vehicle,part:part|renut.vehicle.on("part_removed", function({_params})\n  {_body}\nend)|A part is coming off a vehicle.
E|vehicle.on_enter|Vehicle events|Driver got in||vehicle:vehicle,driver:num,player:bool|renut.vehicle.on("enter", function({_params})\n  {_body}\nend)|Someone gets in: driver is their address, player whether it's Banjo.
E|vehicle.on_exit|Vehicle events|Driver got out||vehicle:vehicle,driver:num,player:bool|renut.vehicle.on("exit", function({_params})\n  {_body}\nend)|Someone gets out.
E|vehicle.on_collide|Vehicle events|Vehicle collided||vehicle:vehicle,strength:num,part:part,impact:vec,other:num|renut.vehicle.on("collide", function({_params})\n  {_body}\nend)|It hit something hard (about 5 and up), its strongest hit of the frame: the part that took it, the hit's push and what it hit (an address, or nil).
E|parts.on_damage|Vehicle events|Any part damaged||part:part,amount:num|renut.parts.on("damage", function({_params})\n  {_body}\nend)|Any part in the game is about to lose health; Set damage amount changes it.
E|parts.on_break|Vehicle events|Any part breaks off||part:part|renut.parts.on("break", function({_params})\n  {_body}\nend)|Any part in the game is breaking off.
E|parts.on_use|Vehicle events|Any part used||part:part,control:num|renut.parts.on("use", function({_params})\n  {_body}\nend)|Any part's button was pressed (control: which of its controls).
E|parts.on_release|Vehicle events|Any part let go||part:part,control:num|renut.parts.on("release", function({_params})\n  {_body}\nend)|Any part's button was let go.
# ---- Parts of your own
D|part.define|Part definition|Define part|!id:str=mymod_part,!base:str,!name:str,!count:num,!model:str,!attach:str,!category:str[body/gadget/seat],!type:str,!mass:num,!health:num,!paint:str,!button:str[A/B/X/none],!base_action:bool=false,params:table?||(renut.parts.add)|Adds a part: with a base, a copy of that shipped part; without, a block of its own (model, attach and category needed). Its callbacks are the part callback nodes naming its id. Button: A, B, X or none.
C|part.on_spawn|Part callbacks|On spawn|!part:str=mymod_part|part:part|on_spawn = function({_params})\n  {_body}\nend,|The part was made (in the garage or with a vehicle).
C|part.on_attach|Part callbacks|On attach|!part:str=mymod_part|part:part|on_attach = function({_params})\n  {_body}\nend,|It was built onto a vehicle.
C|part.on_detach|Part callbacks|On detach|!part:str=mymod_part|part:part|on_detach = function({_params})\n  {_body}\nend,|It came off its vehicle.
C|part.on_tick|Part callbacks|On tick|!part:str=mymod_part|part:part,dt:num|on_tick = function({_params})\n  {_body}\nend,|Every frame while it's on a vehicle.
C|part.on_physics|Part callbacks|On physics|!part:str=mymod_part|part:part,dt:num|on_physics = function({_params})\n  {_body}\nend,|Every physics step on a vehicle: the place for Push.
C|part.on_throttle|Part callbacks|On throttle|!part:str=mymod_part|part:part,throttle:num,dt:num|on_throttle = function({_params})\n  {_body}\nend,|Every frame with the driver's throttle (-1 to 1).
C|part.on_use|Part callbacks|On use|!part:str=mymod_part|part:part|on_use = function({_params})\n  {_body}\nend,|Its action button was pressed.
C|part.on_release|Part callbacks|On release|!part:str=mymod_part|part:part|on_release = function({_params})\n  {_body}\nend,|Its action button was let go.
C|part.on_driver_in|Part callbacks|On driver in|!part:str=mymod_part|part:part|on_driver_in = function({_params})\n  {_body}\nend,|A driver got into its vehicle.
C|part.on_driver_out|Part callbacks|On driver out|!part:str=mymod_part|part:part|on_driver_out = function({_params})\n  {_body}\nend,|The driver got out.
C|part.on_damage|Part callbacks|On damage|!part:str=mymod_part|part:part,amount:num|on_damage = function({_params})\n  {_body}\nend,|It's about to lose health; Set damage amount changes it.
C|part.on_break|Part callbacks|On break|!part:str=mymod_part|part:part|on_break = function({_params})\n  {_body}\nend,|It's breaking off its vehicle.
C|part.on_destroy|Part callbacks|On destroy|!part:str=mymod_part|part:part|on_destroy = function({_params})\n  {_body}\nend,|The part is gone.
C|part.on_collide|Part callbacks|On collide|!part:str=mymod_part|part:part,strength:num,impact:vec,other:num|on_collide = function({_params})\n  {_body}\nend,|Its vehicle hit something hard and this part took it: the hit's strength, push and what it hit (an address, or nil).
)spec" R"spec(
# ---- Vehicle
P|vehicle.at|Vehicle|Vehicle at address|address:num|vehicle:vehicle|renut.vehicle.at({address})|A vehicle object for a vehicle's address.
P|vehicle.address|Vehicle|Vehicle address|vehicle:vehicle|address:num|{vehicle}.address|Its address in game memory.
A|vehicle.spawn|Vehicle|Spawn vehicle|blueprint:str=my_mod: dragster,at:place[front/banjo]=front,yaw:num=0|vehicle:vehicle|renut.vehicle.spawn({blueprint}, {at}, {yaw})|Event. Builds a whole vehicle from a blueprint, as the garage does: an enabled mod's vehicles/*.blueprint ("mod: name" or "name"), one of the save's vehicles ("save: name") or a .blueprint file's full path. At: front, banjo or x y z; yaw in degrees.
A|vehicle.seat|Vehicle|Seat character|vehicle:vehicle,actor:actor[banjo/nearest/last]=banjo,seat:str[driver/passenger/other]=driver||renut.vehicle.seat({vehicle}, {actor}, {seat})|Event. The character gets into the vehicle on its next update, as the game seats a challenge's driver.
P|vehicle.has_body|Vehicle|Has body|vehicle:vehicle|has:bool|{vehicle}:has_body()|Whether it has a physics body now (not while it rebuilds).
P|vehicle.position|Vehicle|Vehicle position|vehicle:vehicle|position:vec|{vehicle}:position()|Where it is.
A|vehicle.set_position|Vehicle|Set vehicle position|vehicle:vehicle,position:vec||{vehicle}:set_position({position})|Teleports it.
A|vehicle.move|Vehicle|Move vehicle|vehicle:vehicle,offset:vec||{vehicle}:move({offset})|Moves it by the offset.
P|vehicle.velocity|Vehicle|Velocity|vehicle:vehicle|velocity:vec|{vehicle}:velocity()|Units per second.
A|vehicle.set_velocity|Vehicle|Set velocity|vehicle:vehicle,velocity:vec||{vehicle}:set_velocity({velocity})|Doesn't wake a resting vehicle.
P|vehicle.angular_velocity|Vehicle|Angular velocity|vehicle:vehicle|spin:vec|{vehicle}:angular_velocity()|Radians per second about each axis.
A|vehicle.set_angular_velocity|Vehicle|Set angular velocity|vehicle:vehicle,spin:vec||{vehicle}:set_angular_velocity({spin})|
P|vehicle.axes|Vehicle|Vehicle axes|vehicle:vehicle|forward:vec,up:vec,right:vec|(function(v) return v:forward(), v:up(), v:right() end)({vehicle})|Its unit axes.
P|vehicle.speed|Vehicle|Speed|vehicle:vehicle|speed:num|{vehicle}:speed()|Along its forward axis, negative backwards.
P|vehicle.mass|Vehicle|Mass|vehicle:vehicle|mass:num|{vehicle}:mass()|
A|vehicle.scale_mass|Vehicle|Scale mass|vehicle:vehicle,scale:num=2||{vehicle}:scale_mass({scale})|Makes it handle so many times as heavy, until the game rebuilds its body.
P|vehicle.in_air|Vehicle|In the air|vehicle:vehicle,reach:num?|in_air:bool|{vehicle}:in_air({reach})|Event. Nothing within reach units (default 1.5) straight below its lowest parts.
A|vehicle.push|Vehicle|Push|vehicle:vehicle,force:vec,dt:num,point:vec?||{vehicle}:push({force}, {dt}{,point})|Event. A thrust for dt seconds (each On physics step); at a point it can spin it too. mass x a accelerates it by a.
A|vehicle.impulse|Vehicle|Impulse|vehicle:vehicle,impulse:vec,point:vec?||{vehicle}:impulse({impulse}{,point})|Event. A kick: velocity changes by impulse / mass.
A|vehicle.angular_impulse|Vehicle|Angular impulse|vehicle:vehicle,impulse:vec||{vehicle}:angular_impulse({impulse})|Event. A spin kick.
P|vehicle.throttle|Vehicle|Throttle|vehicle:vehicle|throttle:num|{vehicle}:throttle()|-1 (reverse or brake) to 1 (full throttle).
P|vehicle.steering|Vehicle|Steering|vehicle:vehicle|steering:num|{vehicle}:steering()|-1 left to 1 right.
P|vehicle.input|Vehicle|Driver input|vehicle:vehicle|steer:num,stick_y:num,brake:num,accelerate:num,braking:bool|(function(i) return i.steer, i.stick_y, i.brake, i.accelerate, i.braking end)({vehicle}:input())|The driver's controls.
A|vehicle.driver|Vehicle|Driver|vehicle:vehicle|driver:num|{vehicle}:driver()|Event. The driver (an address), or nil.
P|vehicle.health|Vehicle|Vehicle health|vehicle:vehicle|health:num,max:num,fraction:num,level:num|(function(v) return v:health(), v:max_health(), v:health_fraction(), v:damage_level() end)({vehicle})|Health, its most, 0-1, and damage level 0-3.
A|vehicle.set_health|Vehicle|Set vehicle health|vehicle:vehicle,health:num||{vehicle}:set_health({health})|Event. 0 disables it.
A|vehicle.repair|Vehicle|Repair vehicle|vehicle:vehicle||{vehicle}:repair()|Event. Full health.
A|vehicle.damage|Vehicle|Damage vehicle|vehicle:vehicle,amount:num,part:part?,attacker:num?||{vehicle}:damage({amount}{,part}{,attacker})|Event. The game's way.
P|vehicle.disabled|Vehicle|Disabled|vehicle:vehicle|disabled:bool|{vehicle}:disabled()|
P|vehicle.invulnerable|Vehicle|Invulnerable|vehicle:vehicle|invulnerable:bool|{vehicle}:invulnerable()|
A|vehicle.set_invulnerable|Vehicle|Set invulnerable|vehicle:vehicle,on:bool=true||{vehicle}:set_invulnerable({on})|
P|vehicle.resource|Vehicle|Resource|vehicle:vehicle,kind:str[fuel/ammo/power]=fuel|amount:num,most:num,infinite:bool|(function(v, k) return v:resource(k), v:max_resource(k), v:infinite(k) end)({vehicle}, {kind})|fuel, ammo or power.
A|vehicle.set_resource|Vehicle|Set resource|vehicle:vehicle,kind:str[fuel/ammo/power]=fuel,amount:num||{vehicle}:set_resource({kind}, {amount})|Clamped to 0-most.
A|vehicle.use|Vehicle|Use resource|vehicle:vehicle,kind:str[fuel/ammo/power]=fuel,amount:num=1|enough:bool|{vehicle}:use({kind}, {amount})|Event. Uses some up; false when there wasn't enough.
A|vehicle.set_infinite|Vehicle|Set infinite resource|vehicle:vehicle,kind:str[fuel/ammo/power]=fuel,on:bool=true||{vehicle}:set_infinite({kind}, {on})|
P|vehicle.parts|Vehicle|Vehicle parts|vehicle:vehicle|parts:table|{vehicle}:parts()|Every part on it (a list).
P|vehicle.find_parts|Vehicle|Find parts|vehicle:vehicle,class:str[vehicleBlockWheel/vehicleBlockEngine/vehicleBlockWeapon/vehicleBlockSeat/vehicleBlockHorn]=vehicleBlockWheel|parts:table|{vehicle}:find_parts({class})|Those of that class.
P|vehicle.name|Vehicle|Vehicle name|vehicle:vehicle|name:str|{vehicle}:name()|
P|vehicle.level|Vehicle|Vehicle level|vehicle:vehicle|level:num|{vehicle}:level()|The level it's in (an address).
P|vehicle.list|Vehicle|Vehicles here||vehicles:table|renut.vehicle.list()|Every vehicle in the world (a list).
P|vehicle.blueprints|Vehicle|Blueprints||blueprints:table|renut.vehicle.blueprints()|What Spawn vehicle takes: {name, file} for each of the enabled mods' blueprints ("mod: name") and the save's vehicles ("save: name").
A|vehicle.remove|Vehicle|Remove vehicle|vehicle:vehicle||{vehicle}:remove()|Event. It leaves the level.
P|vehicle.yaw|Vehicle|Vehicle yaw|vehicle:vehicle|yaw:num|{vehicle}:yaw()|Its facing in degrees (0 faces +z, 90 +x).
A|vehicle.set_yaw|Vehicle|Set vehicle yaw|vehicle:vehicle,yaw:num=0||{vehicle}:set_yaw({yaw})|Event. Stands it upright, facing yaw degrees.
A|vehicle.set_name|Vehicle|Set vehicle name|vehicle:vehicle,name:str||{vehicle}:set_name({name})|Event.
# ---- Part
P|part.vehicle|Part|Part's vehicle|part:part|vehicle:vehicle|{part}:vehicle()|The vehicle it's on, or nil.
P|part.info|Part|Part info|part:part|id:str,instance:num,class:str,aid:num,params:table|(function(p) return p.id, p.instance, p:class(), p:aid(), p.params end)({part})|Its id (yours only), address, class, record id and params.
P|part.is_a|Part|Is a|part:part,class:str[vehicleBlockWheel/vehicleBlockEngine/vehicleBlockWeapon/vehicleBlockSeat/vehicleBlockHorn]=vehicleBlockWheel|is:bool|{part}:is_a({class})|Whether it's that class or derives from it.
P|part.position|Part|Part position|part:part|position:vec|{part}:position()|Where it is in the world.
P|part.cell|Part|Part cell|part:part|x:num,y:num,z:num,orientation:num|(function(p) local x, y, z = p:cell() return x, y, z, p:orientation() end)({part})|Its grid cell and orientation (0-23).
P|part.axes|Part|Part axes|part:part|right:vec,up:vec,forward:vec|{part}:axes()|Its own axes in the world.
P|part.health|Part|Part health|part:part|health:num,max:num,fraction:num,level:num|(function(p) return p:health(), p:max_health(), p:health_fraction(), p:damage_level() end)({part})|
A|part.set_health|Part|Set part health|part:part,health:num||{part}:set_health({health})|
A|part.repair|Part|Repair part|part:part||{part}:repair()|
A|part.damage|Part|Damage part|part:part,amount:num|broke:bool|{part}:damage({amount})|Event. True if it broke off.
A|part.break_off|Part|Break off|part:part||{part}:break_off()|Event.
P|part.paint|Part|Part paint|part:part|r:num,g:num,b:num|{part}:paint()|Its paint, each 0-1.
A|part.set_paint|Part|Set part paint|part:part,colour:vec=1 0.5 0,effect:bool=false||{part}:set_paint({colour}, {effect})|Event. Repaints it (x y z = r g b, 0-1 each); effect plays the garage's paint effect.
A|part.press|Part|Press button|part:part,control:num=0||{part}:press({control})|Event. As a driver would.
A|part.release|Part|Release button|part:part,control:num=0||{part}:release({control})|Event.
A|part.fire|Part|Fire|part:part,position:vec,velocity:vec|shot:num|{part}:fire({position}, {velocity})|Event. Its base weapon's projectile; no ammo used.
A|part.fire_projectile|Part|Fire projectile|part:part,position:vec,velocity:vec,projectile:str[egg/eggbig/grenade/mumbobombo/laser/citrusslick/emp/clockworkkazooie/weldarsbreath/torpedile/freezeezy/rustbin]=egg|shot:num|{part}:fire({position}, {velocity}, {projectile = {projectile}})|Event. Any projectile: egg, eggbig, grenade, laser, emp...
A|part.can_fire|Part|Can fire|part:part,projectile:str[egg/eggbig/grenade/mumbobombo/laser/citrusslick/emp/clockworkkazooie/weldarsbreath/torpedile/freezeezy/rustbin]=egg|can:bool|{part}:can_fire({projectile = {projectile}})|Event.
A|part.hold|Part|Hold trigger|part:part,held:bool=true||{part}:hold({held})|Weapons: fires as the game's weapon does.
P|part.weapon|Part|Weapon info|part:part|ammo_per_shot:num,yaw:num,pitch:num|(function(p) local y, pi = p:aim_angles() return p:ammo_per_shot(), y, pi end)({part})|Weapons only.
P|part.muzzle|Part|Muzzle|part:part|position:vec,direction:vec|{part}:muzzle()|Weapons: where the barrel ends and points.
A|part.aim_at|Part|Aim at|part:part,direction:vec||{part}:aim_at({direction})|Turns a weapon's barrel along a world direction.
A|part.aim|Part|Aim|part:part,yaw:num,pitch:num||{part}:aim({yaw}, {pitch})|A weapon's barrel angles, radians.
A|part.bone|Part|Turn bone|part:part,index:num=0,angle:num,axis:vec?||{part}:bone({index}, {angle}{,axis})|Event. Radians about the axis (default x); each tick.
P|part.bone_count|Part|Bone count|part:part|count:num|{part}:bone_count()|
P|part.shape|Part|Part shape key|part:part,name:str|weight:num|{part}:shape({name})|This part's own weight.
A|part.set_shape|Part|Set part shape key|part:part,name:str,weight:num||{part}:set_shape({name}, {weight})|This part's own weight (each block can differ).
P|parts.list|Part|Custom parts||parts:table|renut.parts.list()|Every custom part loaded.
P|parts.wrap|Part|Part at address|address:num|part:part|renut.parts.wrap({address})|
P|parts.get|Part|Your part at address|address:num|part:part|renut.parts.get({address})|Your own part's object (with your fields), or nil.
P|parts.aid|Part|Part asset id|name:str=gadgets_horn|aid:num|renut.parts.aid({name})|A shipped part's asset id from its name.
P|parts.owned|Part|Parts owned|part:str=gadgets_horn|total:num,available:num|renut.parts.owned({part})|How many the player has in the garage, and how many aren't on a vehicle.
P|projectiles.get|Part|Projectile id|!name:str[egg/eggbig/grenade/mumbobombo/laser/citrusslick/emp/clockworkkazooie/weldarsbreath/torpedile/freezeezy/rustbin]=egg|aid:num|renut.projectiles[{name}]|A projectile's asset id.
A|game.set_amount|Part|Set damage amount|amount:num||renut.set_amount({amount})|Inside a damage event: the damage about to be dealt.
)spec" R"spec(
# ---- World, camera, player
A|fx.play|World|Play effect|effect:str=0x4582A557,position:place[banjo/front]=banjo,vehicle:vehicle?,seconds:num?|played:bool|renut.fx.play({effect}, {position}, {vehicle}{,seconds})|Event. A game particle effect by its id (reNut's docs/effects.md lists them) at banjo, front or x y z; a vehicle picks its level (default Banjo's). False when the effect isn't loaded here.
A|world.ray_cast|World|Ray cast|from:vec,to:vec,vehicle:vehicle?,past_vehicle:bool=false|hit:vec,normal:vec,fraction:num|renut.world.ray_cast({from}, {to}, {vehicle}, {past_vehicle})|Event. The first thing the line hits, or nil: in the vehicle's world, or the level's without one; past vehicle sees through the vehicle itself.
A|world.ground|World|Ground below|at:vec,reach:num?|hit:vec,normal:vec,fraction:num|renut.world.ground({at}, {reach})|Event. The ground under a place: the first thing a line down from just above it hits within reach (default 50), or nil.
A|world.fog|World|Set fog|near:num?,far:num?,most:num?,colour:num?||renut.world.fog{ {&near}{&far}{&most}{&colour}}|The world's fog until it goes; those left empty stay as they are. Which does what isn't confirmed: try values.
P|world.fog_get|World|Fog||near:num,far:num,most:num,colour:num|(function(f) return f.near, f.far, f.most, f.colour end)(renut.world.fog())|The world's fog now.
P|world.env|World|Environment value|offset:num=0|value:num|renut.world.env({offset})|A float of the world's active environment set (448 bytes: sun, ambient light, fog).
A|world.set_env|World|Set environment value|offset:num=0,value:num||renut.world.set_env({offset}, {value})|Writes one.
P|camera.get|World|Camera||position:vec,forward:vec,up:vec,right:vec,fov:num|renut.camera.position(), renut.camera.forward(), renut.camera.up(), renut.camera.right(), renut.camera.fov()|The camera this frame.
A|camera.set|World|Set camera|at:vec,look_at:vec|taken:bool|renut.camera.set({at}, {look_at})|Puts the player's camera there, looking at a point, until released; call it again to move it. False until reNut has seen the game's camera.
A|camera.release|World|Release camera|||renut.camera.release()|Gives the camera back to the game.
A|camera.set_fov|World|Set field of view|degrees:num=70||renut.camera.set_fov({degrees})|5-150 degrees; 0 gives the game's back.
P|mouse.get|World|Mouse||x:num,y:num,inside:bool|(function() local x, y = renut.mouse.position() return x, y, renut.mouse.inside() end)()|The cursor, 0-1 across and down.
P|mouse.down|World|Mouse button down|button:num=0|down:bool|renut.mouse.down({button})|0 left, 1 right, 2 middle.
P|mouse.ray|World|Mouse ray|toward:vec?|from:vec,direction:vec|renut.mouse.ray({toward})|The line from the camera through the cursor.
P|player.get|World|Player||position:vec,yaw:num,driving:bool|renut.player.position(), renut.player.yaw(), renut.player.driving()|Banjo's position, facing (degrees) and whether he drives.
P|player.vehicle|World|Player's vehicle||vehicle:vehicle,avatar:num|renut.player.vehicle(), renut.player.avatar()|The vehicle Banjo is in (or nil), and his object's address.
A|player.teleport|World|Teleport player|at:vec||renut.player.teleport({at})|Event. Moves Banjo there on foot (an error while he drives: move the vehicle).
A|player.play|World|Play player animation|animation:str||renut.player.play({animation})|Event. One of Banjo's animations by the game's name for it.
P|player.motion|World|Player motion||velocity:vec,on_ground:bool,swimming:bool,state:str|renut.player.velocity(), renut.player.on_ground(), renut.player.swimming(), renut.player.state()|Event. Banjo's velocity, whether he stands on the floor or swims, and his body state (Idle, Jump, Swim...).
A|player.push|World|Push player|force:vec||renut.player.push({force})|Event. Adds to Banjo's velocity.
A|player.exit_vehicle|World|Player gets out|||renut.player.exit_vehicle()|Event. Banjo gets out of his vehicle.
A|player.set_model|World|Set player model|model:str||renut.player.set_model({model})|The player model Banjo is made with from now on, in every world (empty: his own).
A|camera.shake|World|Shake camera|strength:num=1,seconds:num=0.5||renut.camera.shake({strength}, {seconds})|Shakes the camera (strength up to 50) for so many seconds.
A|camera.follow|World|Camera follows|target:any,offset:vec?,look_offset:vec?||renut.camera.follow({target}, {offset}, {look_offset})|Keeps the camera on a vehicle (offset along its own axes, default 0 4 -10), a character or object, or a position, until Release camera. Look offset: from it to where the camera looks (default 0 1 0).
P|camera.to_screen|World|To screen|position:vec|x:num,y:num,depth:num|renut.camera.to_screen({position})|Where a world position is on the screen (0-1 across and down) and how far in front it is; nil behind the camera.
# ---- The game's state
P|game.world|Game|World||number:num,name:str,scene:num|renut.game.world()|The world being played: 1 Showdown Town, 2 Nutty Acres, 3 LOG's Lost Challenges, 4 Banjoland, 5 Jiggoseum, 6 Terrarium of Terror, 7 Spiral Mountain, 8 the car park; nil when none.
P|game.challenge|Game|Challenge running||challenge:num|renut.game.challenge()|The running challenge's asset id, or nil.
P|game.challenge_id|Game|Challenge id|name:str|challenge:num|renut.game.challenge_id({name})|A challenge's asset id from its asset name.
P|game.paused|Game|Paused||paused:bool|renut.game.paused()|Whether the pause menu is open.
P|game.flag|Game|Game flag|name:str=gameFlag_Normal_|on:bool|renut.game.flag({name})|A game flag by its full name.
A|game.set_flag|Game|Set game flag|name:str=gameFlag_Normal_,on:bool=true||renut.game.set_flag({name}, {on})|Edits the save, as the flag editor does.
P|game.counter|Game|Game counter|name:str[Notes/JiggiesBanked]=Notes|value:num|renut.game.counter({name})|A counter by its name without gameCounter_BanjoX_, or its full name.
A|game.set_counter|Game|Set game counter|name:str[Notes/JiggiesBanked]=Notes,value:num=0||renut.game.set_counter({name}, {value})|
A|game.add_counter|Game|Add to game counter|name:str[Notes/JiggiesBanked]=Notes,amount:num=1||renut.game.add_counter({name}, {amount})|
A|game.award|Game|Award|counter:str[Notes/JiggiesBanked]=Notes,amount:num=1,show:bool=true||renut.game.award({counter}, {amount}, {show})|Event. Adds to a counter as the game awards it, with its pop-up when show is on.
A|game.challenge_start|Game|Start challenge|challenge:str=aid_challenge_||renut.game.challenge_start({challenge})|Event. Starts a challenge loaded in this world (its asset name or id), the player put at its start.
A|game.challenge_end|Game|End challenge|won:bool=true||renut.game.challenge_end({won})|Event. Ends the one being played, won or lost; its rewards follow.
A|game.challenge_time|Game|Challenge time|limit:num=60,elapsed:num=0||renut.game.challenge_time({limit}, {elapsed})|Event. Its stage's time limit and how much has gone, in seconds; a limit of 0 takes it away.
A|game.challenge_hold|Game|Hold challenge timer|held:bool=true||renut.game.challenge_hold({held})|
A|game.save|Game|Save game||saved:bool|renut.game.save()|Event. Saves player 1's progress, as the game does.
A|game.set_paused|Game|Set paused|paused:bool=true||renut.game.set_paused({paused})|Event. Opens or closes the pause menu.
P|game.time_scale|Game|Time scale||scale:num|renut.game.time_scale()|How fast the game runs (1 normal).
A|game.set_time_scale|Game|Set time scale|scale:num=1||renut.game.set_time_scale({scale})|How fast the game runs: 0.05 to 4 (1 normal).
P|world.time_of_day|Game|Time of day||time:str|renut.world.time_of_day()|reNut's time of day setting: Game, Morning, Afternoon, Evening or Night.
A|world.set_time_of_day|Game|Set time of day|time:str[Game/Morning/Afternoon/Evening/Night]=Night||renut.world.set_time_of_day({time})|Game keeps the world's own.
# ---- Crates
P|crates.list|Game|Crates||flags:table|renut.crates.list()|Every crate's flag (and LOG's shop, prizes...).
P|crates.contents|Game|Crate contents|flag:str|contents:table|renut.crates.contents({flag})|A list of {aid, count}.
A|crates.swap|Game|Swap crate|flag:str,other:str?||renut.crates.swap({flag}{,other})|This crate gives what the other gives from now on; no other undoes it.
A|crates.give|Game|Give crate|flag:str,show:bool=true||renut.crates.give({flag}, {show})|Event. As if collected.
P|crates.in_world|Game|Crates here||crates:table|renut.crates.in_world()|The crates still to collect in this world: {flag, position, object} each.
A|crates.spawn|Game|Spawn crate|flag:str,at:place[front/banjo]=front|object:num|renut.crates.spawn({flag}, {at})|Event. Places a crate for that flag (visit Showdown Town once first).
)spec" R"spec(
# ---- Characters: any of the game's, Banjo and mods'
A|actors.spawn|Characters|Spawn character|character:str[npc_mumbo/npc_humba/npc_bottles/npc_jinjo/npc_boggy/npc_jolly/npc_klungo/npc_mrfit/npc_grunty/npc_log/gruntbot_shooter/gruntbot_exploder3]=npc_mumbo,at:place[front/banjo]=front,yaw:num=0|actor:actor|renut.actors.spawn({character}, {at}, {yaw})|Event. Any of the game's 280 characters, in whichever world, or a mod's as world/id. At: front, banjo or x y z.
P|actors.find|Characters|Find character|who:str[nearest/last/banjo]=nearest|actor:actor|renut.actors.find({who})|nearest (to Banjo), last (spawned), banjo, or a character's name or kind; nil when none.
P|actors.list|Characters|Characters here||list:table|renut.actors.list()|{object, kind, position} for each character in this world.
A|actors.place|Characters|Move character|actor:actor[nearest/last/banjo]=nearest,at:place[banjo/front]=banjo||renut.actors.place({actor}, {at})|Event. Teleports it, as the game does.
A|actors.turn|Characters|Turn character|actor:actor[nearest/last/banjo]=nearest,yaw:num=0||renut.actors.turn({actor}, {yaw})|Event. To face yaw degrees (0 faces +z, 90 +x).
A|actors.play|Characters|Play character animation|actor:actor[nearest/last/banjo]=nearest,animation:str||renut.actors.play({actor}, {animation})|Event. Its animation of that name.
P|actors.state|Characters|Character state|actor:actor[nearest/last/banjo]=nearest|state:str|renut.actors.state({actor})|What its behaviour is doing (Idle, Talking, Follow...).
A|actors.set_state|Characters|Set character state|actor:actor[nearest/last/banjo]=nearest,state:str[Idle/Follow/Celebrate/Worried/KnockedDown/Explode/ReturnToSetupPosition/ScriptedAction]=Celebrate||renut.actors.set_state({actor}, {state})|Event. As its behaviour would.
A|actors.body_state|Characters|Set body state|actor:actor[nearest/last/banjo]=nearest,state:str[Walk/Talk/Hit/StandIdle]=Walk|ok:bool|renut.actors.body_state({actor}, {state})|Event. An animation state, without moving it; false when its body has none.
A|actors.action|Characters|Do action|actor:actor[nearest/last/banjo]=nearest,action:str[npc_idle/celebrate/worried]=celebrate|ok:bool|renut.actors.action({actor}, {action})|Event. One of its mind's named actions; false when it has none of that name.
A|actors.walk_to|Characters|Walk to|actor:actor[nearest/last/banjo]=nearest,at:place[banjo/front]=banjo||renut.actors.walk_to({actor}, {at})|Event. Its behaviour may send it elsewhere after: set it to ScriptedAction to keep it there.
A|actors.stop|Characters|Stop character|actor:actor[nearest/last/banjo]=nearest||renut.actors.stop({actor})|Event. It drops what it was going to do.
A|actors.remove|Characters|Remove character|actor:actor[nearest/last]=last||renut.actors.remove({actor})|Event. It goes (not Banjo).
P|actors.health|Characters|Character health|actor:actor[nearest/last/banjo]=nearest|health:num,max:num|renut.actors.health({actor})|Its body's health and most.
A|actors.set_health|Characters|Set character health|actor:actor[nearest/last/banjo]=nearest,health:num=1||renut.actors.set_health({actor}, {health})|Event. Brings a dead one back; 0 defeats it.
A|actors.damage|Characters|Damage character|actor:actor[nearest/last/banjo]=nearest,amount:num=1|left:num|renut.actors.damage({actor}, {amount})|Event. Takes that off its health; at 0 it's dead.
A|actors.defeat|Characters|Defeat character|actor:actor[nearest/last]=nearest||renut.actors.defeat({actor})|Event. It dies and goes (Banjo stays).
A|actors.follow|Characters|Follow|actor:actor[nearest/last]=last,target:actor[banjo/nearest/last]=banjo||renut.actors.follow({actor}, {target})|Event. It targets the other (a Gruntbot attacks it) and goes into Follow.
A|actors.unfollow|Characters|Unfollow|actor:actor[nearest/last]=last||renut.actors.unfollow({actor})|Event. It drops its target.
P|actors.target|Characters|Character target|actor:actor[nearest/last]=nearest|target:num|renut.actors.target({actor})|What it targets, or nil.
A|actors.set_model|Characters|Set character model|actor:actor[nearest/last/banjo]=nearest,model:str||renut.actors.set_model({actor}, {model})|Event. Another model loaded in this world (aid_model_..., its name or id) with a skeleton its animations fit.
P|actors.vehicle|Characters|Character's vehicle|actor:actor[nearest/last]=nearest|vehicle:vehicle|renut.actors.vehicle({actor})|The vehicle it drives or rides (a Gruntbot's), or nil.
A|actors.talk|Characters|Talk|actor:actor[nearest/last]=nearest,dialog:str|started:bool|renut.actors.talk({actor}, {dialog})|Event. It speaks a dialog loaded here; false over a cutscene or another dialog.
P|actors.motion|Characters|Character motion|actor:actor[nearest/last/banjo]=nearest|velocity:vec,on_ground:bool,yaw:num,state:str|(function(a) return renut.actors.velocity(a), renut.actors.on_ground(a), renut.actors.yaw(a), renut.actors.current_body_state(a) end)({actor})|Event. Its velocity, whether it stands on the floor, its facing (degrees) and its body state now.
A|actors.push|Characters|Push character|actor:actor[nearest/last/banjo]=nearest,force:vec||renut.actors.push({actor}, {force})|Event. Adds to its velocity.
A|actors.exit_vehicle|Characters|Character gets out|actor:actor[nearest/last]=nearest||renut.actors.exit_vehicle({actor})|Event. It gets out of the vehicle it's in.
# ---- Objects
P|objects.kinds|Objects|Object kinds||kinds:table|renut.objects.kinds()|Every kind learned so far ({name, class}): a kind spawns once a world that has it was visited.
P|objects.list|Objects|Objects here||list:table|renut.objects.list()|{object, kind, position} for each object in this world.
A|objects.spawn|Objects|Spawn object|kind:str=Actor_Mumbo,at:place[front/banjo]=front,yaw:num=0|object:num|renut.objects.spawn({kind}, {at}, {yaw})|Event. One of a kind (its objTag without objTag_BanjoX_).
A|objects.remove|Objects|Remove object|object:num||renut.objects.remove({object})|Event. It goes, with what it holds.
P|objects.info|Objects|Object info|object:num|kind:str,position:vec,character:bool|(function(o) return renut.objects.kind(o), renut.objects.position(o), renut.objects.is_character(o) end)({object})|Its kind, where it is and whether it's a character.
A|objects.move|Objects|Move object|object:num,at:vec||renut.objects.move({object}, {at})|Event. Puts it there.
A|objects.turn|Objects|Turn object|object:num,yaw:num=0,pitch:num=0,roll:num=0||renut.objects.turn({object}, {yaw}, {pitch}, {roll})|Event. Its rotation, in degrees.
P|objects.yaw|Objects|Object yaw|object:num|yaw:num|renut.objects.yaw({object})|Its facing in degrees (0 faces +z, 90 +x).
# ---- Trigger zones
A|zones.add|Zones|Add zone|name:str=zone1,shape:str[sphere/box]=sphere,at:place[banjo/front]=banjo,radius:num=5,size:vec=5 5 5,watch:str[banjo/vehicle/characters/all]=banjo,world:str?||renut.zones.add{ name = {name}, at = {at}, radius = {radius}, size = {shape} == "box" and {size} or nil, watch = {watch}, {&world}}|A sphere (radius) or box (size: width height depth) checked every tick; Zone entered and Zone left fire for what it watches. World: only there (a game world's name or a mod world's id), empty for any.
A|zones.move|Zones|Move zone|name:str=zone1,at:place[banjo/front]=banjo||renut.zones.move({name}, {at})|
A|zones.remove|Zones|Remove zone|name:str=zone1||renut.zones.remove({name})|
P|zones.list|Zones|Zones||names:table|renut.zones.list()|The zones' names.
P|zones.count|Zones|Zone count|name:str=zone1|count:num|renut.zones.count({name})|How many watched things are in it.
P|zones.inside|Zones|In zone|name:str=zone1,object:num?|inside:bool|renut.zones.inside({name}{,object})|Whether the object is in it; without one, whether Banjo is.
# ---- The HUD
A|hud.message|HUD|Show message|text:str=Hello!,seconds:num=3||renut.hud.message({text}, {seconds})|Event. The game's message box, no buttons; 0 seconds stays until Close message.
A|hud.close|HUD|Close message|||renut.hud.close()|Event.
F|hud.choice|HUD|Ask|in:exec,text:str=Are you sure?|confirm:exec,cancel:exec||Event. The game's box with Confirm and Cancel: the wire of the button the player picks runs.
A|hud.timer|HUD|Show timer|seconds:num=60,run:str[down/up/hold]=down||renut.hud.timer({seconds}, {run})|The panel's timer; counting down, Timer done fires at 0. Again, it sets it.
A|hud.counter|HUD|Show counter|value:num=0,target:num?||renut.hud.counter({value}{,target})|The panel's counter, as value/target with a target. Again, it sets it.
A|hud.panel|HUD|Show panel|timer:bool=true,counter:bool=true||renut.hud.panel{timer = {timer}, counter = {counter}}|Event. The panel with both; its parts are chosen as it shows (hide it first to add one).
P|hud.time|HUD|Timer seconds||seconds:num|renut.hud.time()|
A|hud.hide|HUD|Hide panel|||renut.hud.hide()|Event.
A|hud.text|HUD|Show text|key:str=status,text:str=Hello!,x:num=0.5,y:num=0.15,size:num=0.035,colour:num=0xFFFFFF,seconds:num?||renut.hud.text({key}, {text}, { x = {x}, y = {y}, size = {size}, colour = {colour}, {&seconds}})|Text on the screen (x and y 0-1 across and down) until removed, or for so many seconds; the same key again changes it.
A|hud.bar|HUD|Show bar|key:str=bar,fraction:num=0.5,label:str?,x:num=0.35,y:num=0.9,width:num=0.3,colour:num=0x3CC864||renut.hud.bar({key}, {fraction}, { {&label}x = {x}, y = {y}, width = {width}, colour = {colour} })|A bar filled to the fraction (0-1), with a label on it; the same key again changes it.
A|hud.marker|HUD|Show marker|key:str=marker,at:any,text:str?||renut.hud.marker({key}, {at}{,text})|A dot over a world position or an object (following it), with text above it, until removed or the world goes.
A|hud.remove|HUD|Remove text|key:str=status||renut.hud.remove({key})|Takes away your text, bar or marker of that key.
A|hud.clear|HUD|Clear texts|||renut.hud.clear()|Takes away all of your mod's texts, bars and markers.
A|debug.line|HUD|Debug line|from:vec,to:vec,colour:num=0x00FF80,seconds:num?||renut.debug.line({from}, {to}, { colour = {colour}, {&seconds}})|A line in the world, drawn over everything, for so many seconds (else for a moment: draw it every frame).
A|debug.sphere|HUD|Debug sphere|at:vec,radius:num=1,colour:num=0x00FF80,seconds:num?||renut.debug.sphere({at}, {radius}, { colour = {colour}, {&seconds}})|A ring the size of a sphere there, as Debug line.
A|debug.text|HUD|Debug text|at:vec,text:any="",seconds:num?||renut.debug.text({at}, {text}, { {&seconds}})|Text at a place in the world, as Debug line.
A|debug.clear|HUD|Clear debug shapes|||renut.debug.clear()|
# ---- Sound and data
A|sound.play|Sound|Play sound|file:str=sounds/boing.wav,volume:num=1,pitch:num=1,loop:bool=false,at:vec?,range:num=70|sound:table|renut.sound.play({file}, { volume = {volume}, pitch = {pitch}, loop = {loop}, {&at}range = {range} })|A .wav, .mp3, .flac or .ogg of your mod's (a path in its folder) or a full path; at makes it come from there. Nil when it won't play.
A|sound.stop|Sound|Stop sound|sound:table||{sound}:stop()|
P|sound.playing|Sound|Sound playing|sound:table|playing:bool|{sound}:playing()|
A|sound.set_volume|Sound|Set sound volume|sound:table,volume:num=1||{sound}:set_volume({volume})|
A|sound.move|Sound|Move sound|sound:table,at:vec?||{sound}:move({at})|Nothing wired: everywhere.
A|sound.music|Sound|Set music|file:str?,volume:num=1||renut.sound.music({file}, {volume})|The music of every world from now on, looped; no file gives the worlds their own back.
A|sound.set_pitch|Sound|Set sound pitch|sound:table,pitch:num=1||{sound}:set_pitch({pitch})|A file's sound: 2 is an octave up.
A|sound.cue|Sound|Play game sound|cue:num=0x1BFBF80D,volume:num=1|sound:table|renut.sound.cue({cue}, { volume = {volume} })|Event. One of the game's own sound cues, by its id; Stop sound and Sound playing work on it. Nil when it won't play.
P|data.get|Kept data|Get kept value|key:str=best_time,default:any?|value:any|renut.data.get({key}{,default})|What your mod keeps between sessions.
A|data.set|Kept data|Keep value|key:str=best_time,value:any||renut.data.set({key}, {value})|Written at once; nil removes it.
P|data.keys|Kept data|Kept keys||keys:table|renut.data.keys()|
P|data.save_get|Kept data|Get saved value|key:str=best_time,default:any?|value:any|renut.data.save_get({key}{,default})|What your mod keeps with the save being played (player 1's).
A|data.save_set|Kept data|Keep with save|key:str=best_time,value:any||renut.data.save_set({key}, {value})|Kept for this save only; nil removes it.
P|data.save_keys|Kept data|Saved keys||keys:table|renut.data.save_keys()|
)spec" R"spec(
# ---- Worlds and doors
A|worlds.add|Worlds|Add world|!id:str=myworld,!name:str?,!scene:str?,!music:str?,!music_volume:num?,!player_model:str?||renut.worlds.add{ {&id}{&name}{&scene}{&music}{&music_volume}{&player_model}}|Declares a world (NutTool writes this for its worlds). Player model: the one Banjo is made with there.
A|worlds.go|Worlds|Go to world|world:str=myworld,at:vec?,yaw:num?||renut.worlds.go({world}, { {&at}{&yaw}})|Event. Takes the player to a mod world (its id), as its door would; with a zone, a way out of a world. At and yaw (degrees): where he arrives, else the world's start.
A|worlds.stats|Worlds|Set door stats|world:str=myworld,jiggies:num?,trophies:num?,jinjos:num?,notes:num?||renut.worlds.stats({world}, { {&jiggies}{&trophies}{&jinjos}{&notes}})|Totals its door's stats panel shows.
A|worlds.set_shape|Worlds|Set world shape key|world:str=myworld,name:str,weight:num||renut.worlds.set_shape({world}, {name}, {weight})|
P|worlds.shape|Worlds|World shape key|world:str=myworld,name:str|weight:num|renut.worlds.shape({world}, {name})|
A|worlds.set_material|Worlds|Set world material|world:str=myworld,name:str,emissive:vec?,emissive_strength:num?,glow:num?,opacity:num?||renut.worlds.set_material({world}, {name}, { {&emissive}{&emissive_strength}{&glow}{&opacity}})|Emissive as a colour (x y z = r g b, 0-1).
A|worlds.reset_material|Worlds|Reset world material|world:str=myworld,name:str||renut.worlds.reset_material({world}, {name})|As NutTool made it.
P|worlds.lists|Worlds|Worlds and materials|world:str=myworld|worlds:table,materials:table|renut.worlds.list(), renut.worlds.materials({world})|
A|doors.add|Worlds|Add door|!world:str=myworld,position:vec,!yaw:num=0,!id:str?,!style:str[wall/arch/palace/hidden]?,!look:str?||renut.doors.add{ {&world}{&position}{&yaw}{&id}{&style}{&look}}|A door in Showdown Town (style: wall, arch, palace, hidden).
# ---- Mod characters
E|npc.on_spawn|Mod characters|Character appears||npc:npc|renut.npcs.on("spawn", function({_params})\n  {_body}\nend)|A world character's actor appeared.
E|npc.on_tick|Mod characters|Character tick||npc:npc,dt:num|renut.npcs.on("tick", function({_params})\n  {_body}\nend)|Every frame for every character in play.
E|npc.on_hit|Mod characters|Enemy hit||npc:npc,hits:num|renut.npcs.on("hit", function({_params})\n  {_body}\nend)|An enemy was hit.
E|npc.on_defeated|Mod characters|Enemy defeated||npc:npc|renut.npcs.on("defeated", function({_params})\n  {_body}\nend)|An enemy was beaten.
A|npcs.spawn|Mod characters|Spawn mod character|world:str=myworld,id:str=mynpc,at:place[front/banjo]=front,yaw:num=0|actor:actor|renut.npcs.spawn({world}, {id}, {at}, {yaw})|Event. A mod's character (renut.npcs.add) in whichever world is played: a plain character there (the Characters nodes act on it).
P|npc.info|Mod characters|Character info|npc:npc|world:str,id:str,name:str,in_play:bool|(function(n) return n.world, n.id, n.name, n:in_play() end)({npc})|
P|npc.place|Mod characters|Character place|npc:npc|position:vec,yaw:num|(function(n) return n:position(), n:yaw() end)({npc})|Its feet and facing (degrees).
A|npc.set_position|Mod characters|Set character position|npc:npc,position:vec||{npc}:set_position({position})|
A|npc.set_yaw|Mod characters|Set character facing|npc:npc,yaw:num||{npc}:set_yaw({yaw})|Degrees: 0 faces +z.
A|npc.play|Mod characters|Play animation|npc:npc,anim:str[stand/walk/run/idle1/hit]=walk||{npc}:play({anim})|stand, walk, run, idle1, hit...
P|npc.health|Mod characters|Enemy health|npc:npc|health:num|{npc}:health()|
A|npc.set_health|Mod characters|Set enemy health|npc:npc,health:num||{npc}:set_health({health})|0 or less beats it.
A|npc.defeat|Mod characters|Defeat enemy|npc:npc||{npc}:defeat()|
A|npc.set_shape|Mod characters|Set character shape key|npc:npc,name:str,weight:num||{npc}:set_shape({name}, {weight})|
P|npc.shape|Mod characters|Character shape key|npc:npc,name:str|weight:num|{npc}:shape({name})|
P|npcs.list|Mod characters|Mod characters||list:table|renut.npcs.list()|
A|npcs.add|Mod characters|Add character|!world:str=myworld,!id:str=mynpc,!name:str?,!tag:str?,!speaker:num?,!voice:str?,text:table?||renut.npcs.add{ {&world}{&id}{&name}{&tag}{&speaker}{&voice}{&text}}|NutTool writes these for its characters.
# ---- Shape keys and materials by model
A|shapes.set|Models|Set shape key|model:str=renutworld_myworld,name:str,weight:num||renut.shapes.set({model}, {name}, {weight})|Every copy of the model.
P|shapes.get|Models|Shape key|model:str=renutworld_myworld,name:str|weight:num,names:table|renut.shapes.get({model}, {name}), renut.shapes.list({model})|
A|materials.set|Models|Set material|model:str=renutworld_myworld,name:str,emissive:vec?,emissive_strength:num?,glow:num?,opacity:num?||renut.materials.set({model}, {name}, { {&emissive}{&emissive_strength}{&glow}{&opacity}})|
A|materials.reset|Models|Reset material|model:str=renutworld_myworld,name:str||renut.materials.reset({model}, {name})|
P|materials.list|Models|Materials|model:str=renutworld_myworld|names:table|renut.materials.list({model})|
# ---- Input, settings, log
P|input.down|Input|Key down|key:str[Space/Shift/Control/Alt/F6/Z/X]=Space|down:bool|rex.input.down({key})|Whether the key is held now.
A|input.pressed|Input|Key pressed now|key:str[F6/F7/Z/X/Space]=F6|pressed:bool|rex.input.pressed({key})|True once per press: call it every frame, from one place.
P|input.accepts|Input|Keys free||free:bool|rex.input.accepts()|No overlay or the Guide wants the keys.
P|input.mouse_button|Input|Mouse button|button:num=0|down:bool|rex.input.mouse_button({button})|
P|pad.button|Input|Pad button|button:str[a/b/x/y/lb/rb/start/back/dpad_up/dpad_down/dpad_left/dpad_right/left_stick/right_stick]=a,player:num=1|down:bool,pressed:bool,released:bool|renut.pad.down({button}, {player}), renut.pad.pressed({button}, {player}), renut.pad.released({button}, {player})|A player's pad button: held, and pressed or let go this frame (as the game reads the pad).
P|pad.sticks|Input|Pad sticks|player:num=1|left_x:num,left_y:num,right_x:num,right_y:num,left_trigger:num,right_trigger:num|(function(n) local lx, ly = renut.pad.stick("left", n) local rx, ry = renut.pad.stick("right", n) return lx, ly, rx, ry, renut.pad.trigger("left", n), renut.pad.trigger("right", n) end)({player})|A player's sticks and triggers, as the game reads the pad.
P|time.now|Input|Clock||seconds:num|rex.now()|Seconds on a steady clock.
P|cvar.get|Settings|Get setting|name:str=mymod_setting,default:any?|value:any|rex.cvar.get({name}{,default})|
A|cvar.set|Settings|Set setting|name:str=mymod_setting,value:any||rex.cvar.set({name}, {value})|
P|cvar.exists|Settings|Setting exists|name:str=mymod_setting|exists:bool|rex.cvar.exists({name})|
A|cvar.define|Settings|Define setting|!name:str=mymod_setting,!kind:str[boolean/int32/number/string]=number,default:any=1,!category:str=Nuts&Bolts/My Mod,!description:str|value:any|rex.cvar[{kind}]({name}, {default}, {category}, {description})|kind: boolean, int32, number or string.
A|settings.add|Settings|Add mod setting|!key:str=speed,!default:any=1,!description:str||renut.settings.add{ key = {key}, default = {default}, description = {description} }|A setting of your mod's in reNut's settings (F4), under Mod: and your mod's name. Its kind is the default's: true/false, a whole number, a number or text (in quotes).
P|settings.get|Settings|Get mod setting|key:str=speed|value:any|renut.settings.get({key})|
A|settings.set|Settings|Set mod setting|key:str=speed,value:any||renut.settings.set({key}, {value})|
E|settings.on_change|Settings|Mod setting changed|!key:str=speed|value:any|renut.settings.on_change({key}, function({_params})\n  {_body}\nend)|After your mod's setting changes.
P|mods.enabled|Settings|Mod enabled|name:str=othermod|enabled:bool|renut.mods.enabled({name})|Whether a mod (its folder's name) is on.
A|log.print|Log|Print|value:any=""||print({value})|To the log.
A|log.screen|Log|Print on screen|value:any="",seconds:num=2,colour:num=0x00A8FF||rex.screen_text(tostring({value}), {seconds}, {colour})|In the screen's top right corner for a while (colour 0xRRGGBB). Only shows it: log it too to keep it.
A|log.warn|Log|Warn|value:any=""||rex.warn({value})|
A|log.error|Log|Log error|value:any=""||rex.error({value})|Only logs.
# ---- Memory and the game's functions
P|mem.read|Memory|Read memory|address:num,!kind:str[u8/u16/u32/u64/s8/s16/s32/s64/f32/f64/string/wstring]=u32|value:any|rex.mem[{kind}]({address})|kind: u8 u16 u32 u64 s8 s16 s32 s64 f32 f64 string wstring.
A|mem.write|Memory|Write memory|address:num,!kind:str[set_u8/set_u16/set_u32/set_u64/set_s8/set_s16/set_s32/set_s64/set_f32/set_f64/set_string]=set_u32,value:any||rex.mem[{kind}]({address}, {value})|kind: set_u8 ... set_f64, set_string.
A|game.call|Memory|Call game function|address:num=0x82000000,a1:num?,a2:num?,a3:num?,a4:num?|result:num|rex.call({address}{,a1}{,a2}{,a3}{,a4})|Its integer arguments go in r3 up; it returns r3. Only from a game thread: a hook or a game event, not Every frame.
P|switch.get|Memory|Switch|name:str|value:num|rex.switch.get({name})|
A|switch.set|Memory|Set switch|name:str,value:any||rex.switch.set({name}, {value})|
)spec" R"spec(
# ---- Menus, online play and Lua challenges
E|menu.on_select|Menus|Menu row picked|!list:str=singlePlayerList,!text:str=My row|row:table,user:num|renut.menu.add{list = {list}, text = {text}, on_select = function({_params})\n  {_body}\nend}|Adds a row at the end of one of the game's list menus (its XUI id) and runs when it's picked.
E|net.on_message|Online|Message received|!name:str=mymsg|value:any,peer:num|renut.net.on({name}, function({_params})\n  {_body}\nend)|A message of that name from another player (Send message): its value and who sent it.
E|net.on_tick|Online|Online tick|||renut.net.on_tick(function()\n  {_body}\nend)|Every frame, after the messages that came in; what's sent here goes out this frame.
A|net.send|Online|Send message|name:str=mymsg,value:any,peer:num?||renut.net.send({name}, {value}{,peer})|To every other player in the session, or to one peer: nil, true/false, numbers, text and tables of them.
P|net.peers|Online|Players online||peers:table|renut.net.peers()|The other players in the session (their peer numbers).
P|net.state|Online|Online state||state:str,host:bool,multiplayer:bool|renut.net.state(), renut.net.is_host(), renut.net.multiplayer()|off, lobby or match; whether this game hosts; whether the game's multiplayer menus or a match are up.
A|net.open_multiplayer|Online|Open multiplayer|live:bool=true||renut.net.open_multiplayer{live = {live}}|Goes to the game's multiplayer, as its main menu's Multiplayer page does (live: its Xbox LIVE choice). Only from a Menu row picked on the main menu.
A|challenges.add|Challenges|Add Lua challenge|!id:str=mychallenge,!name:str?,!time_limit:num?,!show_time:bool?,!goal:num?,!intro:str?,!win_text:str?,!lose_text:str?||renut.challenges.add{ {&id}{&name}{&time_limit}{&show_time}{&goal}{&intro}{&win_text}{&lose_text}}|A challenge of your own: a time limit (lost at 0) or a timer counting up, and a goal (won on reaching it). Start it with Start Lua challenge.
A|challenges.start|Challenges|Start Lua challenge|id:str=mychallenge||renut.challenges.start({id})|Event. Its timer and counter show; one runs at a time.
A|challenges.stop|Challenges|End Lua challenge|won:bool=false||renut.challenges.stop({won})|Event. Ends the running one, won or lost.
A|challenges.score|Challenges|Add to score|amount:num=1||do local c = renut.challenges.running() if c then c:add({amount}) end end|Event. The running challenge's score; reaching its goal wins it.
P|challenges.running|Challenges|Lua challenge running||id:str,score:num,time:num|(function(c) if c then return c.id, c.score, c.time end end)(renut.challenges.running())|The running one's id, score and seconds so far, or nil.
E|challenges.on_finish|Challenges|Lua challenge finished||id:str,won:bool,time:num,score:num|renut.challenges.on("finish", function({_params})\n  {_body}\nend)|A Lua challenge ended, won or lost: its seconds and score.
)spec" R"spec(
# ---- Flow
F|flow.branch|Flow|Branch|in:exec,condition:bool|true:exec,false:exec||The true wire when the condition holds, else the false wire.
F|flow.sequence|Flow|Sequence|in:exec|then1:exec,then2:exec,then3:exec,then4:exec||Runs each wire in turn.
F|flow.foreach|Flow|For each|in:exec,list:table|loop:exec,item:any,index:num,done:exec||The loop wire for each item of a list, then done.
F|flow.for|Flow|For loop|in:exec,from:num=1,to:num=10,step:num=1|loop:exec,index:num,done:exec||The loop wire for each number from-to.
F|flow.while|Flow|While|in:exec,condition:bool|loop:exec,done:exec||The loop wire while the condition holds.
F|flow.delay|Flow|Delay|in:exec,seconds:num=1|then:exec||The then wire later, from the frame.
F|flow.once|Flow|Do once|in:exec|then:exec||The then wire the first time only.
F|flow.return|Flow|Stop|in:exec|||Stops this event's code here.
P|logic.select|Flow|Select|condition:bool,yes:any,no:any|value:any|(function(c, a, b) if c then return a end return b end)({condition}, {yes}, {no})|One value or the other.
# ---- Variables and fields
P|var.get|Variables|Get variable|!name:str=myvar|value:any|{@name}|A graph variable (add it under Variables).
A|var.set|Variables|Set variable|!name:str=myvar,value:any||{@name} = {value}|
P|field.get|Variables|Get field|object:any,!field:str=fuel|value:any|{object}[{field}]|A field of a table or part (your parts keep theirs).
A|field.set|Variables|Set field|object:any,!field:str=fuel,value:any||{object}[{field}] = {value}|
# ---- Values
P|value.number|Values|Number|!value:num=0|value:num|{value}|
P|value.string|Values|Text|!value:str|value:str|{value}|
P|value.bool|Values|Boolean|!value:bool=true|value:bool|{value}|
P|value.vector|Values|Make vector|x:num=0,y:num=0,z:num=0|vector:vec|renut.vec({x}, {y}, {z})|
P|value.break_vector|Values|Break vector|vector:vec|x:num,y:num,z:num|(function(v) return v.x, v.y, v.z end)({vector})|
P|value.place|Values|Place|!place:place[front/banjo]=front|place:place|{place}|front (before Banjo, facing him), banjo (where he is) or x y z.
P|value.actor|Values|Who|!who:actor[nearest/last/banjo]=last|actor:actor|{who}|A character by who: nearest (to Banjo), last (spawned), banjo, or a name.
P|value.nil|Values|Nil||value:any|nil|Nothing.
P|value.table|Values|New table||table:table|{}|An empty table.
# ---- Maths
P|math.add|Maths|Add|a:num,b:num|result:num|{a} + {b}|
P|math.subtract|Maths|Subtract|a:num,b:num|result:num|{a} - {b}|
P|math.multiply|Maths|Multiply|a:num,b:num=1|result:num|{a} * {b}|
P|math.divide|Maths|Divide|a:num,b:num=1|result:num|{a} / {b}|
P|math.modulo|Maths|Modulo|a:num,b:num=1|result:num|{a} % {b}|
P|math.power|Maths|Power|a:num,b:num=2|result:num|{a} ^ {b}|
P|math.negate|Maths|Negate|a:num|result:num|-{a}|
P|math.minmax|Maths|Min and max|a:num,b:num|min:num,max:num|math.min({a}, {b}), math.max({a}, {b})|
P|math.abs|Maths|Absolute|a:num|result:num|math.abs({a})|
P|math.round|Maths|Round|a:num|floor:num,ceil:num,round:num|(function(x) return math.floor(x), math.ceil(x), math.floor(x + 0.5) end)({a})|
P|math.sqrt|Maths|Square root|a:num|result:num|math.sqrt({a})|
P|math.trig|Maths|Sine and cosine|radians:num|sin:num,cos:num,tan:num|(function(r) return math.sin(r), math.cos(r), math.tan(r) end)({radians})|
P|math.atan2|Maths|Angle of|y:num,x:num=1|radians:num|math.atan({y}, {x})|The angle of (x, y), radians.
P|math.angles|Maths|Degrees and radians|degrees:num,radians:num|to_radians:num,to_degrees:num|math.rad({degrees}), math.deg({radians})|
P|math.pi|Maths|Pi||pi:num|math.pi|
P|math.clamp|Maths|Clamp|value:num,low:num=0,high:num=1|result:num|math.max({low}, math.min({high}, {value}))|
P|math.lerp|Maths|Lerp|a:num,b:num=1,t:num=0.5|result:num|{a} + ({b} - {a}) * {t}|
P|math.random|Maths|Random|low:num=0,high:num=1|result:num|{low} + math.random() * ({high} - {low})|
P|vector.add|Maths|Vector add|a:vec,b:vec|result:vec|{a} + {b}|
P|vector.subtract|Maths|Vector subtract|a:vec,b:vec|result:vec|{a} - {b}|
P|vector.scale|Maths|Vector scale|vector:vec,scale:num=1|result:vec|{vector} * {scale}|
P|vector.length|Maths|Vector length|vector:vec|length:num,normalized:vec|{vector}:length(), {vector}:normalized()|
P|vector.dot|Maths|Dot and cross|a:vec,b:vec|dot:num,cross:vec|{a}:dot({b}), {a}:cross({b})|
P|vector.distance|Maths|Distance|a:vec,b:vec|distance:num|({a} - {b}):length()|
# ---- Comparisons and logic
P|compare.numbers|Logic|Compare|a:num,b:num|equal:bool,less:bool,greater:bool|{a} == {b}, {a} < {b}, {a} > {b}|
P|compare.equal|Logic|Equal|a:any,b:any|equal:bool,different:bool|{a} == {b}, {a} ~= {b}|
P|logic.and|Logic|And|a:bool,b:bool|result:bool|{a} and {b}|
P|logic.or|Logic|Or|a:bool,b:bool|result:bool|{a} or {b}|
P|logic.not|Logic|Not|a:bool|result:bool|not {a}|
P|logic.is_nil|Logic|Is nil|value:any|is_nil:bool|{value} == nil|
# ---- Text and tables
P|text.join|Text and lists|Join text|a:any,b:any|text:str|tostring({a}) .. tostring({b})|
P|text.format|Text and lists|Format text|format:str=%s,a:any?,b:any?,c:any?|text:str|string.format({format}{,a}{,b}{,c})|
P|text.convert|Text and lists|To text or number|value:any|text:str,number:num|tostring({value}), tonumber({value})|
P|list.get|Text and lists|Get item|list:table,index:num=1|item:any|{list}[{index}]|
P|list.length|Text and lists|Length|list:any|length:num|#{list}|
A|list.add|Text and lists|Add item|list:table,item:any||table.insert({list}, {item})|
A|list.remove|Text and lists|Remove item|list:table,index:num=1|item:any|table.remove({list}, {index})|
# ---- Lua
P|lua.expr|Lua|Lua expression|!code:str={a} + 1,a:any?,b:any?,c:any?,d:any?|value:any||Any Lua expression; {a} to {d} are the inputs wired in.
A|lua.stmt|Lua|Lua code|!code:str=print({a}),a:any?,b:any?,c:any?,d:any?||(code)|Any Lua statements; {a} to {d} are the inputs wired in.
)spec";

} // namespace nut::scriptgraph
