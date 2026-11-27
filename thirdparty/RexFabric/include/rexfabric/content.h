// Content: the classes a game declares and the ones mods make as assets in their content folders
// (docs/content_and_classes.md). Every class comes from Object; a class names its parent and keeps only the
// properties it adds and the defaults it changes.
//
// A game class can stand for one of the game's types (rexfabric::Namespace::Type), so the class and the type are one
// thing to graphs and Lua: what the type's objects are, and where they sit in the hierarchy. A class below it without
// a type of its own says which of the type's objects are one of it.
//
//   rexfabric::content::DeclareClass({.path = "TiP/Object", .type = "tip.Object", .help = "Any of the game's objects"});
//   rexfabric::content::DeclareClass({.path = "TiP/Avatar", .parent = "TiP/Object", .type = "tip.Avatar"});
//   rexfabric::content::DeclareClass({.path = "TiP/Pinata", .parent = "TiP/Avatar", .test = IsPinata});
//
// Properties can also hold the game's enums and structs, and arrays and maps of any of their types:
//
//   rexfabric::content::DeclareStruct({.path = "TiP/TagRequirement", .fields = {{.name = "tag", .type = {.type = PropertyType::kInt}}, {.name = "requirement", .type = {.type = PropertyType::kEnum, .of = "tip.Requirement"}}}});

#pragma once

#include <any>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <rexfabric/game_api.h>

namespace rexfabric::content {

// What a property holds. An asset property is a path to another asset ("MyMod/Thumbs/Rock"), or empty for none; an
// enum property one of an enum's values, by name; a struct property a value of each of the struct's fields.
enum class PropertyType { kBool, kInt, kFloat, kString, kText, kAsset, kEnum, kStruct };

// One value, an ordered list of them, or values each under a key.
enum class Container { kSingle, kArray, kMap };

// A property's or a struct field's type: a property type, in a container.
struct ValueType {
  PropertyType type = PropertyType::kInt;
  // What the type names. kEnum: the enum, a game's by its Lua name ("tip.Weather") or a mod's by its path; kStruct:
  // the struct; kAsset: the class the asset must be, empty for any; kString: a data table whose row names it's one of
  // ("TiP/Effects", the game's effects), which the editor offers to pick from, empty for any string.
  std::string of;
  Container container = Container::kSingle;
  // A map's keys: bool, int, string, an enum or an asset, with what that names.
  PropertyType key = PropertyType::kString;
  std::string key_of;

  bool operator==(const ValueType&) const = default;
};

// A property's value, as a game gives one: none (an empty asset, or a container's or struct's empty value), or a value
// of its type. An enum value is its name.
using PropertyValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

struct PropertyDecl {
  std::string name;
  ValueType type;
  PropertyValue value;
  std::string help;
};

// Whether an object of the class's type is one of the class: is this avatar a pinata. It gets the object as the type
// holds it, on the thread the mod asking runs on.
using ClassTest = std::function<bool(const Object& object)>;

struct ClassDecl {
  // Under a path of the game's own ("TiP/Pinata").
  std::string path;
  // "Object" or a class declared before it.
  std::string parent = "Object";
  std::string help;
  // The game type the class stands for, by full name ("tip.Avatar"). Empty: its objects are of its parent's type.
  std::string type;
  // For a class without a type of its own, which of its type's objects are one of it. Unset: every one of them that is
  // one of its parent.
  ClassTest test;
  std::vector<PropertyDecl> properties;
};

// Declares one of the game's classes. Before rexfabric::Start, after the types it names; false (logged) for a bad
// name, a taken path, an unknown parent or an unknown type.
bool DeclareClass(ClassDecl decl);

// A struct of the game's: a named set of typed fields, each with a default, which mods use as they use their own
// structs and can't change. The game's enums are the ones it declares through its API (Namespace::Enum), named as
// Lua names them ("tip.Weather").
struct StructDecl {
  // Under a path of the game's own ("TiP/TagRequirement").
  std::string path;
  std::string help;
  std::vector<PropertyDecl> fields;
};

// Declares one of the game's structs. Before rexfabric::Start, after the enums and structs its fields name; false
// (logged) for a bad name, a taken path or a field of a type that isn't there.
bool DeclareStruct(StructDecl decl);

// A language the game's texts come in: its code, as its files name it ("french"), and its name as the player reads it.
struct LanguageDecl {
  std::string code;
  std::string name;
};

// Declares the game's languages, the first being the one a mod writes in when it doesn't say, and how to tell which one
// the game shows now (from any thread; empty when it can't say yet). Mods' texts translate into it
// (docs/content_and_classes.md, Localisation). Before rexfabric::Start.
void DeclareLanguages(std::vector<LanguageDecl> languages, std::function<std::string()> current);

// One of the game's own texts (docs/content_and_classes.md, The game's texts).
struct GameText {
  // How the game finds it, the same in every language: what a language asset keys its text by.
  std::string id;
  // Its name, where the game's API knows one; empty where it doesn't.
  std::string label;
  // As the language the game shows has it.
  std::string text;
};

// The texts the game keeps together (in Trouble in Paradise, a loctext asset), under the group's name.
struct GameTextGroup {
  std::string name;
  std::vector<GameText> texts;
};

// How a game lets mods change its own texts, with language assets: what its texts are, and how to show the mods'.
struct GameTextsDecl {
  // Lists every text as the language the game shows has it. RexFabric calls it when the texts are first read, and
  // again after GameTextsChanged, on whatever thread reads them. False when it can't now (off the thread the game's
  // data lives on, or before its data is there): the texts stay as they were until the game calls FillGameTexts.
  std::function<bool(std::vector<GameTextGroup>& groups)> list;
  // The mods' texts in place of the game's: by language code, then by text id. Given again whenever they change (mods
  // loaded, a language asset saved), from any thread; a language the game doesn't show now waits until it does.
  std::function<void(std::map<std::string, std::map<std::string, std::string>> texts)> apply;
};

// Declares how mods change the game's texts. Before rexfabric::Start; at most one.
void DeclareGameTexts(GameTextsDecl decl);

// The game's texts changed (a language loaded): they're listed again the next time they're read. Any thread.
void GameTextsChanged();

// Lists the game's texts now, from where its `list` can run; false when it still can't. For a game whose `list`
// refused a reader on another thread.
bool FillGameTexts();

// Builds a game table's rows, one row at a time: Row starts one, and the rest fill its fields. A field left alone is
// its default.
class TableRows {
 public:
  virtual ~TableRows() = default;
  // A row under `name`, which follows the asset name rules and is the table's only row of that name.
  virtual void Row(std::string name) = 0;
  // Sets a field of the row to one value: for a struct field, its fields go in as "field.inner".
  virtual void Set(std::string_view field, PropertyValue value) = 0;
  // Adds a value to an array field.
  virtual void Add(std::string_view field, PropertyValue value) = 0;
  // Puts a value under a key in a map field; for a map of structs, `inner` names the struct's field it sets.
  virtual void Put(std::string_view field, PropertyValue key, PropertyValue value, std::string_view inner = {}) = 0;
};

// A data table of the game's (docs/content_and_classes.md, The game's tables): rows of one of its structs, made from
// the game's data rather than a file, which mods read as they read their own tables and can't change.
struct TableDecl {
  // Under a path of the game's own ("TiP/Pinatas").
  std::string path;
  std::string help;
  // The struct each row is, declared before.
  std::string row;
  // Fills the rows as the game is now. RexFabric calls it when the table is read and its rows are out of date (never
  // made, or TableChanged said so), on whatever thread reads it. False when it can't now (off the thread the game's
  // data lives on, or before the data is there): the rows stay as they were until the game calls FillTable.
  std::function<bool(TableRows& rows)> rows;
};

// Declares one of the game's tables. Before rexfabric::Start, after its row struct; false (logged) for a bad name, a
// taken path or a struct that isn't there.
bool DeclareTable(TableDecl decl);

// What a game table describes changed (a price set, a requirement moved): its rows are made again the next time it's
// read. Any thread.
void TableChanged(std::string_view path);

// Makes a game table's rows now, from where its `rows` can run; false when it still can't. For a game whose `rows`
// refused a reader on another thread.
bool FillTable(std::string_view path);

// Where something is: a position, a rotation (pitch, yaw and roll, in degrees, turned yaw first, then pitch, then
// roll) and a scale, in the game's units and axes, y up.
struct Transform {
  std::array<double, 3> position{0.0, 0.0, 0.0};
  std::array<double, 3> rotation{0.0, 0.0, 0.0};
  std::array<double, 3> scale{1.0, 1.0, 1.0};
  bool operator==(const Transform&) const = default;
};

// A transform as a 4 x 4 matrix of row vectors (ImGuizmo's layout, and the game's): the x, y and z axes scaled, then the
// position, as the editors place things.
std::array<float, 16> MatrixOf(const Transform& transform);
// A matrix of MatrixOf's layout as a transform, its axes' lengths as the scale.
Transform TransformOf(const std::array<float, 16>& matrix);

// An avatar's property values, as its backing and components see them: every property of its class, by name, as the
// avatar has it. A value no PropertyValue holds (an array, a map, a struct) is none; an enum's is its value's name.
using AvatarValues = std::map<std::string, PropertyValue>;

// What the game makes in the world for an avatar of a mod's class (docs/content_and_classes.md, Avatars): a backing
// is offered to the mod classes whose nearest game class is `for_class`, the first one declared for a class being the
// one its mod classes get when they name none. Every function runs on the thread the avatar is spawned and ticked on,
// the game's, and any may throw to refuse, with the reason as its message.
struct BackingDecl {
  // Under a path of the game's own ("TiP/Place").
  std::string path;
  std::string help;
  // The game class it's for ("TiP/Avatar").
  std::string for_class;
  // Makes it where `at` says, as the avatar's values say; its handle, never 0.
  std::function<uint64_t(const Transform& at, const AvatarValues& values)> make;
  // Where it is now, into `at`; false once it's gone from the world (its world went, the game removed it).
  std::function<bool(uint64_t handle, Transform& at)> where;
  std::function<void(uint64_t handle, const Transform& to)> move;
  std::function<void(uint64_t handle)> remove;
  // The game's object it is, for a backing that makes one; unset, or none, for one that's only a place.
  std::function<Object(uint64_t handle)> object;
  // One of the avatar's values was set on it: the backing shows them as they are now. Unset for a backing that only
  // reads them as it's made.
  std::function<void(uint64_t handle, const AvatarValues& values)> apply;
};

// Declares one of the game's backings. Before rexfabric::Start, after the class it's for; false (logged) for a bad
// path, a taken one, an unknown class or a missing function.
bool DeclareBacking(BackingDecl decl);

// What a component's functions are given: the avatar, where the component is, its values, and room for its own state.
struct ComponentContext {
  uint32_t avatar = 0;      // the avatar's id, as Lua has it
  std::string avatar_class;
  std::string name;         // the component's name in its class
  Transform world;          // where it is in the world now: the avatar's place and the components above it
  std::map<std::string, PropertyValue> properties;  // each of its type's properties, as the class sets it
  double seconds = 0.0;     // since the last tick; 0 when attached and detached
  std::any state;           // the component's own, kept from call to call for as long as the avatar is there
};

// A kind of component the game offers (docs/content_and_classes.md, Components): its properties, whether it has a
// place of its own, and what it does when its avatar is spawned, every tick while it's there, and when it goes. Each
// function runs on the game's thread and may throw: a throw is logged, and a tick that throws isn't called again.
struct ComponentDecl {
  // Under a path of the game's own ("TiP/FxEmitter").
  std::string path;
  std::string help;
  // Whether it has a transform, relative to the component above it or to the avatar.
  bool transform = true;
  std::vector<PropertyDecl> properties;
  std::function<void(ComponentContext& context)> attach;
  std::function<void(ComponentContext& context)> tick;
  std::function<void(ComponentContext& context)> detach;
};

// Declares one of the game's components. Before rexfabric::Start; false (logged) for a bad path, a taken one, or a
// property of a type that isn't there.
bool DeclareComponent(ComponentDecl decl);

// A world of the game's (docs/content_and_classes.md, Map edits): a place it loads, which mods' map edits change each
// time it's entered: avatars spawned in it, and later what a game's own kinds of edit do (a model replaced, an actor
// taken away).
struct WorldDecl {
  // Under a path of the game's own ("TiP/TitleScreen").
  std::string path;
  std::string help;
};

// Declares one of the game's worlds. Before rexfabric::Start; false (logged) for a bad path or a taken one.
bool DeclareWorld(WorldDecl decl);

// The world the game is in now, by its path, or empty for none of its declared worlds (between worlds, loading). The
// map edits for it are applied on the next tick, as the game's thread ticks RexFabric, and undone when the game leaves
// it: a new path, or the same one given again after an empty one, is a new visit. A mod's world (docs/
// content_and_classes.md, Worlds) may be given where the game is in the world it's built on: its edits are applied and
// its base's too. Any thread.
void SetWorld(std::string_view path);

// The game's world a world comes down to: itself for one of the game's, and for a mod's the game's world at the end of
// its bases; empty for no world RexFabric knows or one not built on the game's. Any thread.
std::string GameWorldOf(std::string_view path);

// The model file of a loaded mod's mesh asset ("MyMod/Town"; docs/content_and_classes.md, Meshes): the binary glTF
// beside it, for the game to read. None, with `why`, for a path that isn't one. Any thread.
std::optional<std::filesystem::path> MeshModelOf(std::string_view path, std::string* why = nullptr);

// The mods' part of a world's save (docs/content_and_classes.md, Saved with the world): the avatars spawned to be saved
// (rex.content.spawn's `saved`), each as a record of its class, where it is and the values it has of its own. The game
// writes WorldSaveData into its save as it saves a world, and gives RestoreWorldSave what it read back once that world
// has loaded, so they're spawned again where they were. A record whose class isn't loaded (its mod gone or turned off)
// isn't spawned: it's kept as it was and goes into the world's next save, so a mod turned off for a while finds its
// avatars again. Both on the thread the avatars tick on.
std::string WorldSaveData();
// Spawns the records in `data`, as WorldSaveData gave it, and keeps the ones it can't; empty for a world saved without
// any (a new one, or one saved without mods), which forgets the records kept for the last. Gives how many it spawned.
size_t RestoreWorldSave(std::string_view data);

// A part of a world's save the game keeps for itself beside the mods' avatars and data (docs/content_and_classes.md,
// Saved with the world): what it adds to its worlds for mods that isn't an avatar, such as Trouble in Paradise's mod
// packets poured on the ground. `save` gives the part as JSON text, empty for nothing to keep; `restore` gets what the
// world loaded with, empty for none, once its saved avatars are back. Both on the thread the avatars tick on. A part
// a save holds that no declaration names is kept as it came and saved again.
struct WorldSavePartDecl {
  std::string name;
  std::function<std::string()> save;
  std::function<void(std::string_view json)> restore;
};

// Declares a part. Before rexfabric::Start; false (logged) for a bad name, a taken one or a missing function.
bool DeclareWorldSavePart(WorldSavePartDecl decl);

// Packets (docs/content_and_classes.md, Packets): kinds of ground a player buys and pours, each a variant of one of
// the game's own kinds, its base, that looks and sells as it says. The game says what the bases are and what a look
// holds for each; RexFabric keeps the mods' packet assets and lists them after the game's own.
enum class PacketUnlock { kDefault, kShop };

struct Packet {
  std::string path;   // "TiP/Sand", "MyMod/RedSand"
  std::string mod;    // the mod it's from, empty for the game's own
  std::string base;   // a value of the game's base enum, by name ("Sand")
  std::string name;   // shown where it's picked and sold, in the language the game shows
  std::string help;   // the same
  std::string icon;   // a Texture asset, empty for none
  AvatarValues look;  // each field of its base's look struct, as the packet sets it or the field's default
  PacketUnlock unlock = PacketUnlock::kDefault;  // every player has it, or it's bought in the shop
  int64_t level = 0;  // kShop: the shop level it's sold from
  int64_t price = 0;  // kShop: what it costs
  double order = 0.0;  // where it sorts among its mod's packets
};

struct PacketsDecl {
  // The game's enum a packet's base is one of, by its Lua name ("tip.PacketBase").
  std::string base;
  // What a packet of each base looks like: the game's struct its look is, by the base value's name.
  std::map<std::string, std::string> looks;
  // The game's own packets, read only, listed first and in this order, under paths of the game's own.
  std::vector<Packet> built_in;
};

// Declares the game's packets. Before rexfabric::Start, after its base enum and look structs; at most once. Without
// it there's no New Packet and the mods' packet assets aren't loaded.
void DeclarePackets(PacketsDecl decl);

// Every packet: the game's own in their order, then the loaded mods', by mod load order, then by their order, then by
// path. Any thread.
std::vector<Packet> Packets();
// One packet, the game's or a loaded mod's; none for a path that isn't one. Any thread.
std::optional<Packet> FindPacket(std::string_view path);
// Goes up whenever the mods' packets are loaded again (a mod turned on or off, a packet's file changed), for a game
// keeping what it made of them, such as their looks. Any thread.
uint64_t PacketsRevision();

// A kind of map edit the game offers beside RexFabric's own Spawn: a model swapped, an actor removed. Its functions run
// on the thread RexFabric ticks on, the game's, and may throw to refuse, with the reason as their message.
struct MapEditKindDecl {
  // Under a path of the game's own ("TiP/ReplaceModel").
  std::string path;
  std::string help;
  // Whether an edit of it has a place in the world (a transform).
  bool transform = false;
  std::vector<PropertyDecl> properties;
  // Makes the edit in `world` as its values say; a handle of the game's to undo it with, 0 for nothing to undo.
  std::function<uint64_t(std::string_view world, const Transform& at, const AvatarValues& values)> apply;
  // Undoes an edit apply made, the world still there (when the world went first, the game's own leaving undid it).
  std::function<void(uint64_t handle)> undo;
  // Moves what apply made to a new place, when only its place changed (an editor's gizmo): false, or none, and it's
  // undone and made again instead.
  std::function<bool(uint64_t handle, const Transform& at)> move;
};

// Declares one of the game's kinds of map edit. Before rexfabric::Start, after the types its properties name; false
// (logged) for a bad path, a taken one, a property of a type that isn't there, or no apply.
bool DeclareMapEditKind(MapEditKindDecl decl);

// A value a material graph's pin carries: 1 to 4 floats, named as the graph's editor shows it ("Base Colour"). In the
// code a graph makes, an input of the Material node is the MaterialResult field of its name in lower case with
// underscores for spaces (base_colour).
struct MaterialPinDecl {
  std::string name;
  uint8_t width = 3;
  std::array<float, 4> initial{};
  std::string help;
};

// How a graph material's surface is lit (docs/materials.md, Shading models): the game's to say and to draw. Its
// inputs are the Material node's inputs it reads; the node greys out the rest.
struct ShadingModelDecl {
  std::string name;
  std::string help;
  std::vector<std::string> inputs;
};

// A node of the game's for its material graphs: one of its scene's values, each output an HLSL expression of the
// context `c` the game's shader gives the code (c.vertex_colour).
struct MaterialNodeDecl {
  std::string id;
  std::string title;
  // "Scene": slashes nest the add menu.
  std::string category;
  std::string help;
  std::vector<MaterialPinDecl> outputs;
  // One for each output.
  std::vector<std::string> code;
};

// A texture a graph material samples: a mod's Texture asset by its path, or one of the game's textures by its name.
struct MaterialTextureUse {
  std::string texture;
  bool game = false;
  bool cube = false;  // sampled by a direction, as a cube map
  bool operator==(const MaterialTextureUse&) const = default;
};

// A named value of a graph material (a Parameter node's), that what's drawn with the material may change without
// compiling it again: 1 to 4 floats.
struct MaterialParameterUse {
  std::string name;
  uint8_t width = 1;
  std::array<float, 4> value{};
};

// A graph material compiled (docs/materials.md, The code): HLSL defining the struct MaterialResult, with a field
// for each of its type's graph inputs, and `MaterialResult EvaluateMaterial(MaterialContext c)` filling it from the
// graph. The code samples texture n of `textures` with MaterialSample2D(c, n, uv) or MaterialSampleCube(c, n, dir) and
// reads parameter n of `parameters` with MaterialParameter(c, n), all of which the game's shader defines, as it does
// MaterialContext. A graph with problems has its problems and no code.
struct MaterialProgram {
  std::string shading_model;
  std::string code;
  std::vector<MaterialTextureUse> textures;
  std::vector<MaterialParameterUse> parameters;
  // The Material node's inputs the graph links, by name; the others are their initial values.
  std::vector<std::string> linked;
  std::vector<std::string> problems;
  // Of `code`, for the game keeping the shader it compiled from it.
  uint64_t hash = 0;
};

// A kind of material the game offers (docs/content_and_classes.md, Materials): what a material of it sets on what the
// game draws, as properties, and how the material editor previews one. A mod's material asset is of one of these
// types and gives its properties values; whatever the game draws with it (a model) reads them with FindMaterial.
struct MaterialTypeDecl {
  // Under a path of the game's own ("TiP/ModelMaterial").
  std::string path;
  std::string help;
  std::vector<PropertyDecl> properties;
  // The preview: one of the game's components shown alone in the editor's viewport, with these values, and the
  // material's path in its property `preview_material`. `preview_choice` names a property of the component the editor
  // lets its user pick (the model it's shown on), empty for none.
  std::string preview_component;
  AvatarValues preview_values;
  std::string preview_material;
  std::string preview_choice;

  // A graph material type (docs/materials.md): its materials are node graphs ending in a Material node, whose inputs
  // are these, lit by one of these shading models. The type's properties are the Material node's details (a blend
  // mode, two sided). The game compiles the code a graph makes (MaterialProgram) into its own shader. Empty for a type
  // whose materials only give its properties values.
  std::vector<MaterialPinDecl> graph_inputs;
  std::vector<ShadingModelDecl> shading_models;
  // The game's own nodes for its graphs: values of its scene (the vertex colour, the sun).
  std::vector<MaterialNodeDecl> graph_nodes;
  // The data table whose rows name the game's textures, which a graph's texture nodes may sample as well as the mods'
  // Texture assets ("TiP/Textures"); empty for the mods' alone.
  std::string game_textures;
  // What the game finds wrong with a graph's code once it has it (its shader compiler's errors), which the editor shows
  // with its problems; empty while it's fine or still being worked on. Any thread, and often.
  std::function<std::vector<std::string>(const MaterialProgram& program)> check_program;
};

// Declares one of the game's material types. Before rexfabric::Start, after the types its properties name; false
// (logged) for a bad path, a taken one, or a property of a type that isn't there.
bool DeclareMaterialType(MaterialTypeDecl decl);

// A material as it is now: its type, each single property's value (the material's, or its type's default), each map
// property's entries by key, and a version that goes up with any change (a save, an editor's unsaved change), for a
// game keeping what it made of it. A graph material has its graph compiled.
struct MaterialView {
  std::string type;
  AvatarValues values;
  std::map<std::string, std::map<std::string, PropertyValue>> maps;
  uint64_t version = 0;
  std::shared_ptr<const MaterialProgram> program;
};

// A loaded mod's material ("MyMod/Materials/Gold"), as its editor has it while one is open; none for a path that isn't
// one. Any thread.
std::optional<MaterialView> FindMaterial(std::string_view path);

// The game's picture of the world it's in, for editing it in place (docs/content_and_classes.md, Editing in the
// world): the camera it's drawn with, so the map edit editor draws its markers and gizmos over the picture, and hit
// tests of rays into the world, so it can place things on what's under the pointer.
struct WorldCamera {
  std::array<float, 16> view{};        // row vectors, as ViewportImage's, only turning and moving
  std::array<float, 16> projection{};  // onto the whole of the game's picture
  std::array<double, 3> eye{};
};

// A ray into the world, from one point toward another, and what it hit first: none where it hit nothing on its way.
struct WorldRay {
  std::array<double, 3> from{};
  std::array<double, 3> to{};
  bool operator==(const WorldRay&) const = default;
};
struct WorldHit {
  WorldRay ray;
  std::optional<std::array<double, 3>> point;
};

struct WorldViewDecl {
  // The camera of the picture shown now; false while no world is shown. Any thread.
  std::function<bool(WorldCamera& camera)> camera;
  // The rays the editor wants hit tested, replacing the ones before; the game tests them on its own time. Any thread.
  std::function<void(std::vector<WorldRay> rays)> aim;
  // What the rays last tested hit, each with its ray. Any thread.
  std::function<std::vector<WorldHit>()> hits;
};

// Registers the game's view of the world. Before rexfabric::Start; without one, map edits are edited by their values.
void SetWorldView(WorldViewDecl decl);

// A mod's texture asset's picture (docs/content_and_classes.md, Textures): RGBA8, rows top first, tightly packed.
struct TextureImage {
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  // Changes when the picture's file does, for a game keeping what it made of it.
  uint64_t version = 0;
};

// The picture of a loaded mod's texture asset ("MyMod/Pictures/Logo"), read from its file; none, with `why`, for a
// path that isn't one or a picture that can't be read. Any thread.
std::optional<TextureImage> LoadTextureImage(std::string_view path, std::string* why = nullptr);

// One of the class's components as the viewport shows it, for a game that shows what it does (an emitter playing):
// where the avatar puts it and its values.
struct ViewportPart {
  std::string name;
  std::string type;     // its component type ("TiP/FxEmitter")
  Transform world;      // in the avatar's space
  AvatarValues values;  // each of its type's properties, as the class sets it
  bool own = false;     // the class's own, not inherited
};

// The Avatar editor's viewport (docs/content_and_classes.md, The viewport): one avatar of the class being edited,
// alone, drawn by the game and shown by the editor, which draws the components' gizmos over it. What's asked for: the
// class as the editor has it now, saved or not, its components, and how the editor's camera looks at it.
struct ViewportView {
  std::string class_path;
  std::string game_class;  // its nearest game class ("TiP/Signpost")
  std::string backing;     // the backing it has, its own or inherited
  AvatarValues values;     // every property's value, as the editor has them
  double yaw = 0.0;        // degrees the camera has gone round the avatar's up axis
  double pitch = 0.0;      // degrees the camera is above it
  double zoom = 1.0;       // 1 for the game's framing of the avatar and its components, more to come nearer
  uint32_t width = 0;      // the viewport's size, in pixels
  uint32_t height = 0;
  std::vector<ViewportPart> parts;  // every component with its place: the inherited first, each tree in order
  bool playing = true;     // its time runs (an emitter plays); paused, everything holds still
  uint32_t replays = 0;    // goes up each time the editor asks for everything to start again
  bool hold_framing = false;  // a component is being dragged: the framing stays as it is, so the camera keeps still
};

// What the game drew last for a viewport.
struct ViewportImage {
  uint64_t texture = 0;  // the UI's texture of it (an ImTextureID); 0 while there's none
  std::array<float, 2> uv_max{1.0f, 1.0f};  // the part of the texture it fills, from the top left
  // How a point in the avatar's space lands on it, for the gizmos: clip = point * view * projection with row vectors
  // (the layout ImGuizmo takes), x and y from -1 to 1 across the image, y up. The view only turns and moves (any scale
  // goes in the projection), as the gizmos are sized by it. `camera` false when the game can't say.
  bool camera = false;
  bool orthographic = false;
  std::array<float, 16> view{};
  std::array<float, 16> projection{};
  std::string message;  // what to show in its place while there's no image, as why
};

// What draws the viewports, each known by a number of its own. `show` comes from the UI thread every frame a viewport
// is seen and gives what was drawn last, the game drawing on its own time; `close` when the viewport goes. Either may
// be called on any thread, and neither should wait for the game.
struct ViewportRenderer {
  std::function<ViewportImage(uint64_t viewport, const ViewportView& view)> show;
  std::function<void(uint64_t viewport)> close;
};

// Registers the game's viewport renderer, at any time, a later one replacing it. Until one is, the viewport says so and
// shows the components over a plain background.
void SetViewportRenderer(ViewportRenderer renderer);

// Whether a name works as an asset or folder name: letters, digits and _, starting with a letter, at most 64. With
// `why`, the reason it doesn't.
bool ValidName(std::string_view name, std::string* why = nullptr);

}  // namespace rexfabric::content
