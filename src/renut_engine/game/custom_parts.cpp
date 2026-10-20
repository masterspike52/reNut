/**
 * @file    game/custom_parts.cpp
 * @brief   Serves .nbpatch mods' custom vehicle parts (mods/custom_parts.h) to the game.
 *
 * N&B (IDA): a part is an objparams record (aid_objparams_banjox_vehicleblock_*,
 * type 0x1F) naming its class (+0x42 objDefId_<class>, looked up at +0x4B)
 * with the class's fields; everything that needs a part opens it by asset id
 * through assetManOpenParams(&aid, may_fail), which returns the asset's data,
 * or asynchronously through sub_822C0CE8(&aid, ..., may_fail, ...), and lets
 * it go with sub_82235E68(data); sub_822C1148(data) is its size. The asset
 * manager's worker (sub_826EF618) shows the dirty disc error for a request
 * that must not fail and does. The garage inventory (vector 0x82FACA44) is
 * rebuilt by sub_8251CB20 at level start from the unlocked blocksets, each
 * added by sub_8251CDD8(&blockset aid, demo): a blockset asset is (u32 count,
 * u32 part aid) pairs, and each part gets an entry with its count, category
 * tab (+0x228) and paint. A part's name is the text key at +0xA0, looked up as
 * "<section>__<key>" by sub_82396E00. The garage lists the inventory in three
 * levels of 204-byte grouping records (name at + 0, text key at + 64, the next
 * level's table aid at + 128): the categories (class_groupings, matched
 * against the part's + 0x228), a category's types (sub_8251D090, + 0x268), a
 * type's sizes (sub_8251D238, + 0x2A8), one item per size: parts of the same
 * category, type and size share one item, and only one of them shows.
 *
 * Here each custom part is aid_objparams_banjox_vehicleblock_renut_<id>, a
 * record built on first open: its base part's record (opened through the game,
 * then let go), or the mod's whole record, with the mod's fields written over
 * it and the text key renutpart_<id>, kept in guest memory for the session.
 * The asset functions answer for these records (release does nothing), the
 * inventory build adds a blockset of every custom part (its count each, owned
 * without buying), and the text lookup answers renutpart_<id> with the part's
 * name. Each part gets a garage item of its own: a variant a size renut_<id>
 * in its base's type (the type's size table served with one more record, its
 * base size's renamed, text key renutpart_<id>); a new kind of part (with a
 * script), or a variant of a type without sizes (gadgets, wheels: one item per
 * type), a type renut_<id> in its base's category (the category's type table
 * served with its base type's record renamed, pointing at a size table of its
 * own, banjox_renut_sizes_<id>, holding its one size), listed beside its
 * base's type rather than under it.
 *
 * Vehicles keep their parts by aid, so a part of a removed mod would stop the
 * game: any request for an asset that isn't there fails, and the asset
 * manager's worker calls its error callback, XShowDirtyDiscErrorUI
 * (rex_XShowDirtyDiscErrorUI, which never returns), whatever may_fail says;
 * so does a synchronous open's completion (sub_82273078) unless its opener set
 * + 312, which assetManOpenParams never does, after setting byte_82FAB13A,
 * which stops the game (sub_8223C650 then only presents, forever). A
 * remembered part
 * (mods/custom_parts.h) opens its base part instead, and the block spawn's
 * copy (sub_8227CA10, + 0x88 the aid) takes the base's aid: the block becomes
 * that part (no missing-part ghost in the garage, and saving keeps the base).
 * Any other objparams aid is checked once before the game opens it (an open
 * of our own, its completion told to fail quietly: + 312 set, and the
 * worker's error dialog skipped). Callers of an open, and of the objparams
 * copy, mostly don't allow for nothing (the load list's parts check,
 * sub_8251D630, reads the copy at once), so one that isn't there opens a
 * placeholder part, the light cube; a copy keeps the missing aid at + 0x88
 * (the garage has no such part: its missing-part ghost). The block spawn
 * (sub_8256C1B8) leaves a missing part's block out.
 *
 * g_mutex guards this file's state and is never held across a call into the
 * game: the asset manager waits on its loader thread, which comes back here.
 */
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/part_classes.h"
#include "renut_engine/mods/custom_parts.h"

REX_EXTERN(__imp__assetManOpenParams);
REX_EXTERN(__imp__sub_822C0CE8);  // asynchronous asset open
REX_EXTERN(__imp__sub_82235E68);  // asset release
REX_EXTERN(__imp__sub_822C1148);  // asset size
REX_EXTERN(__imp__sub_8251CB20);  // garage inventory build
REX_EXTERN(__imp__sub_8251CDD8);  // garage inventory: add a blockset
REX_EXTERN(__imp__sub_82396E00);  // text lookup by full key
REX_EXTERN(__imp__sub_8227CA10);  // a part's objparams copy, for spawning its block
REX_EXTERN(__imp__rex_XShowDirtyDiscErrorUI);
REX_EXTERN(__imp__sub_82273078);  // a synchronous open's completion
REX_EXTERN(__imp__sub_8256C1B8);  // spawns a vehicle block from its part's aid

namespace {

using renut::mods::CustomPart;
using renut::mods::KnownCustomPart;

// Guest data.
constexpr uint8_t kTypeObjparams = 0x1F;
constexpr uint8_t kTypeMisc = 0x0B;
constexpr uint32_t kBlockClassGroupings = 0x82FACA38;  // the inventory's class_groupings (0: no inventory)
constexpr uint32_t kTextUpperCaseMode = 0x82E5F2F0;   // sub_82396E00 upper-cases below 2
// Objparams record: u16 size at + 0, the name's text key at + 0xA0, the
// asset id the game keeps in its copies at + 0x88 (+ 0x8C cleared).
constexpr uint32_t kRecordNameKey = 0xA0;
constexpr uint32_t kRecordNameKeySize = 64;
constexpr uint32_t kRecordAid = 0x88;
constexpr uint32_t kRecordAidNext = 0x8C;
// + 0x42 "objDefId_<class>" (char[62]): the class's name at + 0x4B.
constexpr uint32_t kRecordClassName = 0x4B;
constexpr uint32_t kRecordClassNameSize = 0x80 - kRecordClassName;
constexpr const char kScriptedClassPrefix[] = "renut_";
// The part's grouping names: category, type, size (char[64] each).
constexpr uint32_t kRecordCategory = 0x228;
constexpr uint32_t kRecordType = 0x268;
constexpr uint32_t kRecordSizeName = 0x2A8;
constexpr uint32_t kRecordGroupingNameSize = 64;
constexpr uint32_t kMinRecordSize = kRecordSizeName + kRecordGroupingNameSize;
// The part's model (aid_model_banjox_vehicleparts_*; its collision is the
// aid_havok_ asset of the same name) and footprint / attach points
// (aid_avatarhavokdata_banjox_vehicleblock_*), by asset id.
constexpr uint32_t kRecordModel = 0x124;
constexpr uint32_t kRecordAttach = 0x12C;
constexpr uint8_t kTypeModel = 0x04;
constexpr uint8_t kTypeHavok = 0x05;
constexpr uint8_t kTypeAttach = 0x14;
// sub_822C1228(const u32 *aid): whether a loaded bundle has the asset (no
// request made, so no error dialog).
constexpr uint32_t kAssetLoaded = 0x822C1228;
// Grouping record (class_groupings and the type and size tables).
constexpr uint32_t kGroupingSize = 204;
constexpr uint32_t kGroupingName = 0;
constexpr uint32_t kGroupingTextKey = 64;
constexpr uint32_t kGroupingNext = 128;  // the next level's table aid
constexpr uint32_t kGroupingStringSize = 64;
// The part opened in place of a missing one: the light cube
// (vehicleBlock, "lightcube", body / light), in every game's inventory.
constexpr uint32_t kPlaceholderPartAid = 0x1F4BB5E1;
constexpr const char kSizePrefix[] = "renut_";
constexpr const char kOwnSizesPrefix[] = "banjox_renut_sizes_";  // a new kind of part's size table
constexpr const char kPartAssetPrefix[] = "banjox_vehicleblock_renut_";
constexpr const char kBlocksetAsset[] = "banjox_blockset_renut_custom_parts";
constexpr const char kTextKeyPrefix[] = "renutpart_";
constexpr size_t kMaxTextKey = 256;

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

std::u16string Utf16(const std::string &utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0 && i + 3 < utf8.size()) {
      cp = (c & 0x07u) << 18 | (utf8[i + 1] & 0x3Fu) << 12 | (utf8[i + 2] & 0x3Fu) << 6 | (utf8[i + 3] & 0x3Fu);
      n = 4;
    } else if (c >= 0xE0 && i + 2 < utf8.size()) {
      cp = (c & 0x0Fu) << 12 | (utf8[i + 1] & 0x3Fu) << 6 | (utf8[i + 2] & 0x3Fu);
      n = 3;
    } else if (c >= 0xC0 && i + 1 < utf8.size()) {
      cp = (c & 0x1Fu) << 6 | (utf8[i + 1] & 0x3Fu);
      n = 2;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(char16_t(cp));
    }
    i += n;
  }
  return out;
}

std::string GuestString(const uint8_t *base, uint32_t va, size_t max) {
  std::string s;
  while (s.size() < max && base[va + s.size()])
    s.push_back(char(base[va + s.size()]));
  return s;
}

bool SameName(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = char(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
}

void PutString(uint8_t *at, const std::string &s, size_t field) {
  std::memset(at, 0, field);
  std::memcpy(at, s.data(), std::min(s.size(), field - 1));
}

struct GuestPart {
  const CustomPart *part = nullptr;
  uint32_t aid = 0;
  uint32_t base_aid_va = 0;  // guest word holding the base part's aid
  std::string text_key;      // renutpart_<id>
  std::u16string name;
  // Under g_mutex:
  uint32_t record = 0;  // guest address once built
  uint32_t size = 0;
  bool failed = false;  // building failed (logged once)
};

// A remembered part's base aid in a guest word, to open it by.
struct KnownBase {
  uint32_t aid = 0;
  uint32_t base_aid_va = 0;
};

// A type's size table, served with the custom parts' sizes added.
struct SizeTable {
  uint32_t aid = 0;
  uint32_t data = 0;
  uint32_t size = 0;
};

std::mutex g_mutex;
bool g_initialized = false;
// Fixed once initialized (but GuestPart's built fields).
std::vector<GuestPart> g_parts;
std::vector<KnownBase> g_known_bases;
uint32_t g_blockset_aid = 0;
uint32_t g_blockset_aid_va = 0;  // guest word holding g_blockset_aid
uint32_t g_blockset = 0;         // guest (count, aid) pairs
uint32_t g_blockset_size = 0;
uint32_t g_scratch_aid_va = 0;   // guest word for opening by aid (the inventory build only)
uint32_t g_placeholder_aid_va = 0;  // guest word holding kPlaceholderPartAid
// Under g_mutex:
std::vector<SizeTable> g_size_tables;
std::unordered_set<uint32_t> g_existing;  // objparams aids checked and found
std::unordered_set<uint32_t> g_missing;   // ... and not found
std::unordered_set<uint32_t> g_substitutes_logged;
std::atomic<bool> g_any_size_table{false};
std::atomic<bool> g_sizes_prepared{false};
// While a check of our own runs, a failed open fails quietly (its error
// dialog skipped, its completion told so).
std::atomic<int> g_checks_running{0};

// Once, on the first call: the parts' ids and guest words.
void Initialize(uint8_t *base) {
  std::lock_guard lock(g_mutex);
  if (g_initialized)
    return;
  g_initialized = true;
  const std::vector<CustomPart> &parts = renut::mods::CustomParts();
  // Base aid words, the blockset's aid word, the scratch word, the
  // placeholder's aid word, the blockset, then the remembered parts' base aid
  // words.
  const uint32_t count = uint32_t(parts.size());
  const uint32_t known_count = uint32_t(renut::mods::KnownCustomParts().size());
  g_blockset_size = 8 * count;
  const uint32_t block =
      REX_KERNEL_MEMORY()->SystemHeapAlloc(4 * count + 12 + g_blockset_size + 4 * known_count);
  if (!block) {
    RNUT_ERROR("Custom parts: no guest memory; custom parts are not in the game");
    return;
  }
  g_blockset_aid_va = block + 4 * count;
  g_scratch_aid_va = g_blockset_aid_va + 4;
  g_placeholder_aid_va = g_scratch_aid_va + 4;
  g_blockset = g_placeholder_aid_va + 4;
  Store32(base, g_placeholder_aid_va, kPlaceholderPartAid);
  const uint32_t known_words = g_blockset + g_blockset_size;
  if (count) {
    g_blockset_aid = renut::mods::AssetId(kTypeMisc, kBlocksetAsset);
    Store32(base, g_blockset_aid_va, g_blockset_aid);
  }
  for (uint32_t i = 0; i < count; ++i) {
    GuestPart gp;
    gp.part = &parts[i];
    gp.aid = renut::mods::AssetId(kTypeObjparams, kPartAssetPrefix + parts[i].id);
    gp.base_aid_va = block + 4 * i;
    Store32(base, gp.base_aid_va,
            parts[i].base.empty() ? 0 : renut::mods::AssetId(kTypeObjparams, parts[i].base));
    gp.text_key = kTextKeyPrefix + parts[i].id;
    gp.name = Utf16(parts[i].name);
    Store32(base, g_blockset + 8 * i, parts[i].count);
    Store32(base, g_blockset + 8 * i + 4, gp.aid);
    g_parts.push_back(std::move(gp));
  }
  uint32_t k = 0;
  for (const KnownCustomPart &known_part : renut::mods::KnownCustomParts()) {
    const uint32_t word = known_words + 4 * k++;
    Store32(base, word, known_part.base_aid);
    g_known_bases.push_back({known_part.aid, word});
  }
}

GuestPart *FindByAid(uint32_t aid) {
  for (GuestPart &gp : g_parts)
    if (gp.aid == aid)
      return &gp;
  return nullptr;
}

// A remembered part whose mod isn't loaded, or nullptr.
const KnownCustomPart *FindRemovedPart(uint32_t aid) {
  return FindByAid(aid) ? nullptr : renut::mods::FindKnownCustomPart(aid);
}

uint32_t KnownBaseWord(uint32_t aid) {
  for (const KnownBase &k : g_known_bases)
    if (k.aid == aid)
      return k.base_aid_va;
  return 0;
}

// Marks a part's build failed; true the first time (to log it once).
bool MarkFailed(GuestPart &gp) {
  std::lock_guard lock(g_mutex);
  const bool first = !gp.failed;
  gp.failed = true;
  return first;
}

// Whether a loaded bundle has the asset 'aid'.
bool AssetLoaded(PPCContext &ctx, uint8_t *base, uint32_t aid) {
  ::PPCFunc *loaded = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(kAssetLoaded);
  const uint32_t word = REX_KERNEL_MEMORY()->SystemHeapAlloc(4);
  if (!loaded || !word)
    return false;
  Store32(base, word, aid);
  ctx.r3.u64 = word;
  loaded(ctx, base);
  const bool there = (ctx.r3.u32 & 0xFF) != 0;
  REX_KERNEL_MEMORY()->SystemHeapFree(word);
  return there;
}

// Points the record's field at 'offset' at asset 'name' of 'type' when a
// loaded bundle has it (else the base's stays, logged).
void SetAssetField(PPCContext &ctx, uint8_t *base, const CustomPart &part, std::vector<uint8_t> &bytes,
                   uint32_t offset, uint8_t type, const char *prefix, const std::string &name) {
  if (name.empty())
    return;
  const uint32_t aid = renut::mods::AssetId(type, name);
  if (!AssetLoaded(ctx, base, aid)) {
    RNUT_ERROR("Custom part '{}' ({}): {}{} ({:08X}) is in no loaded bundle; it keeps its base part's", part.id,
               part.mod, prefix, name, aid);
    return;
  }
  // A model's collision goes with it (the game opens it by the model's name).
  const uint32_t havok = renut::mods::AssetId(kTypeHavok, name);
  if (type == kTypeModel && !AssetLoaded(ctx, base, havok)) {
    RNUT_ERROR("Custom part '{}' ({}): its model has no collision, aid_havok_{} ({:08X}); it keeps its base part's "
               "model", part.id, part.mod, name, havok);
    return;
  }
  const uint32_t value = rex::byte_swap(aid);
  std::memcpy(bytes.data() + offset, &value, 4);
}

// The part's record, built on first use: its base's (opened through the game)
// or the mod's, with the mod's fields, model, text key and aid written over it.
uint32_t BuildPart(PPCContext &ctx, uint8_t *base, GuestPart &gp, uint32_t may_fail) {
  {
    std::lock_guard lock(g_mutex);
    if (gp.record || gp.failed)
      return gp.record;
  }
  const CustomPart &part = *gp.part;
  std::vector<uint8_t> bytes;
  if (!part.objparams.empty()) {
    bytes = part.objparams;
  } else {
    ctx.r3.u64 = gp.base_aid_va;
    ctx.r4.u64 = may_fail;
    __imp__assetManOpenParams(ctx, base);
    const uint32_t source = ctx.r3.u32;
    if (!source) {
      if (MarkFailed(gp))
        RNUT_ERROR("Custom part '{}' ({}): its base part {} is not loaded or does not exist", part.id,
                   part.mod, part.base);
      return 0;
    }
    const uint32_t size = Load16(base, source);
    bytes.assign(base + source, base + source + size);
    ctx.r3.u64 = source;
    __imp__sub_82235E68(ctx, base);
  }
  if (bytes.size() < kMinRecordSize) {
    if (MarkFailed(gp))
      RNUT_ERROR("Custom part '{}' ({}): its record is {} bytes, too small for a part", part.id, part.mod,
                 bytes.size());
    return 0;
  }
  for (const renut::mods::CustomPartField &field : part.fields) {
    if (field.offset + field.bytes.size() > bytes.size()) {
      RNUT_WARN("Custom part '{}' ({}): field 0x{:X} is past the record's {} bytes; skipped", part.id,
                part.mod, field.offset, bytes.size());
      continue;
    }
    std::memcpy(bytes.data() + field.offset, field.bytes.data(), field.bytes.size());
  }
  SetAssetField(ctx, base, part, bytes, kRecordModel, kTypeModel, "aid_model_", part.model);
  SetAssetField(ctx, base, part, bytes, kRecordAttach, kTypeAttach, "aid_avatarhavokdata_", part.attach);
  if (!part.name.empty()) {
    // Clears no further than the old key's or the new key's end (the field's
    // size isn't known, only that it holds them).
    uint8_t *key = bytes.data() + kRecordNameKey;
    const size_t old_length = strnlen(reinterpret_cast<const char *>(key), kRecordNameKeySize);
    std::memset(key, 0, std::min<size_t>(kRecordNameKeySize, std::max(old_length, gp.text_key.size()) + 1));
    std::memcpy(key, gp.text_key.data(), gp.text_key.size());
  }
  if (!part.script.empty()) {
    // A new kind of part: a class of its own, deriving from its base's.
    uint8_t *class_name = bytes.data() + kRecordClassName;
    const std::string parent(reinterpret_cast<const char *>(class_name),
                             strnlen(reinterpret_cast<const char *>(class_name), kRecordClassNameSize));
    const std::string own = kScriptedClassPrefix + part.id;
    if (renut::game::RegisterScriptedPartClass(ctx, base, own, parent, &part)) {
      std::memset(class_name, 0, kRecordClassNameSize);
      std::memcpy(class_name, own.data(), std::min<size_t>(own.size(), kRecordClassNameSize - 1));
    } else {
      RNUT_ERROR("Custom part '{}' ({}): its script's class couldn't be made; it is a plain {}", part.id,
                 part.mod, parent);
    }
  }
  const uint32_t record = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(bytes.size()));
  if (!record) {
    if (MarkFailed(gp))
      RNUT_ERROR("Custom part '{}': no guest memory for its record", part.id);
    return 0;
  }
  std::memcpy(base + record, bytes.data(), bytes.size());
  Store32(base, record + kRecordAid, gp.aid);
  Store32(base, record + kRecordAidNext, 0);

  std::lock_guard lock(g_mutex);
  if (!gp.record) {  // (another thread may have built it meanwhile)
    gp.record = record;
    gp.size = uint32_t(bytes.size());
    RNUT_INFO("Custom part '{}' ({}) is in the game: asset {:08X}, {} bytes{}", part.id, part.mod, gp.aid,
              gp.size, part.base.empty() ? "" : ", from " + part.base);
  }
  return gp.record;
}

// The size of a custom record, the blockset or a served size table at 'data',
// or 0 for other assets.
uint32_t OwnSize(uint32_t data) {
  if (!data)
    return 0;
  std::lock_guard lock(g_mutex);
  if (g_blockset && data == g_blockset)
    return g_blockset_size;
  for (const GuestPart &gp : g_parts)
    if (gp.record == data)
      return gp.size;
  for (const SizeTable &table : g_size_tables)
    if (table.data == data)
      return table.size;
  return 0;
}

// Whether the objparams asset *aid_va is there; checked once each, by opening
// it (its error dialog skipped). 'how' names the game's open for the log.
bool Exists(PPCContext &ctx, uint8_t *base, uint32_t aid_va, uint32_t aid, const char *how) {
  {
    std::lock_guard lock(g_mutex);
    if (g_existing.contains(aid))
      return true;
    if (g_missing.contains(aid))
      return false;
  }
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = aid_va;
  ctx.r4.u64 = 1;
  g_checks_running.fetch_add(1, std::memory_order_acq_rel);
  __imp__assetManOpenParams(ctx, base);
  g_checks_running.fetch_sub(1, std::memory_order_acq_rel);
  const uint32_t data = ctx.r3.u32;
  if (data) {
    ctx.r3.u64 = data;
    __imp__sub_82235E68(ctx, base);
  }
  std::lock_guard lock(g_mutex);
  if (data) {
    g_existing.insert(aid);
    return true;
  }
  if (g_missing.insert(aid).second)
    RNUT_WARN("Vehicle part {:08X} doesn't exist (a part of a removed mod?); vehicles leave it out and the "
              "garage shows it missing ({}, guest caller {:08X})",
              aid, how, uint32_t(lr));
  return false;
}

// A remembered part whose mod is gone: logged once.
void LogRemoved(const KnownCustomPart &known) {
  std::lock_guard lock(g_mutex);
  if (!g_substitutes_logged.insert(known.aid).second)
    return;
  if (known.base_aid)
    RNUT_WARN("Custom part '{}' (mod '{}') is not loaded; a vehicle using it gets its base part instead",
              known.id, known.mod);
  else
    RNUT_WARN("Custom part '{}' (mod '{}') is not loaded and has no base part; vehicles using it go without it",
              known.id, known.mod);
}

// An opened asset's size, through the game.
uint32_t AssetSize(PPCContext &ctx, uint8_t *base, uint32_t data) {
  ctx.r3.u64 = data;
  __imp__sub_822C1148(ctx, base);
  return ctx.r3.u32;
}

// The grouping record named 'name' in an opened table, or 0.
uint32_t FindGrouping(PPCContext &ctx, uint8_t *base, uint32_t table, const std::string &name) {
  const uint32_t count = AssetSize(ctx, base, table) / kGroupingSize;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t record = table + i * kGroupingSize;
    if (SameName(GuestString(base, record + kGroupingName, kGroupingStringSize), name))
      return record;
  }
  return 0;
}

// A grouping table's records, opened through the game by aid (empty if none).
std::vector<uint8_t> ReadGroupingTable(PPCContext &ctx, uint8_t *base, uint32_t aid) {
  Store32(base, g_scratch_aid_va, aid);
  ctx.r3.u64 = g_scratch_aid_va;
  ctx.r4.u64 = 1;
  __imp__assetManOpenParams(ctx, base);
  const uint32_t data = ctx.r3.u32;
  if (!data)
    return {};
  const uint32_t size = AssetSize(ctx, base, data) / kGroupingSize * kGroupingSize;
  std::vector<uint8_t> bytes(base + data, base + data + size);
  ctx.r3.u64 = data;
  __imp__sub_82235E68(ctx, base);
  return bytes;
}

// The record named 'name' in a table's bytes (its offset), or the first.
size_t GroupingRecordOffset(const std::vector<uint8_t> &table, const std::string &name) {
  for (size_t at = 0; at + kGroupingSize <= table.size(); at += kGroupingSize) {
    const char *s = reinterpret_cast<const char *>(table.data() + at + kGroupingName);
    if (SameName(std::string(s, strnlen(s, kGroupingStringSize)), name))
      return at;
  }
  return 0;
}

// A copy of 'table''s record 'from', renamed to the part's own name and text.
std::vector<uint8_t> OwnGroupingRecord(const std::vector<uint8_t> &table, size_t from, const GuestPart &gp) {
  std::vector<uint8_t> record(table.begin() + from, table.begin() + from + kGroupingSize);
  PutString(record.data() + kGroupingName, kSizePrefix + gp.part->id, kGroupingStringSize);
  if (!gp.name.empty())
    PutString(record.data() + kGroupingTextKey, gp.text_key, kGroupingStringSize);
  return record;
}

// Serves 'bytes' as the grouping table 'aid' from now on.
void ServeGroupingTable(uint8_t *base, uint32_t aid, const std::vector<uint8_t> &bytes) {
  const uint32_t data = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(bytes.size()));
  if (!data) {
    RNUT_ERROR("Custom parts: no guest memory for a garage table");
    return;
  }
  std::memcpy(base + data, bytes.data(), bytes.size());
  {
    std::lock_guard lock(g_mutex);
    g_size_tables.push_back({aid, data, uint32_t(bytes.size())});
  }
  g_any_size_table.store(true, std::memory_order_release);
}

// A part waiting for its garage item: its record, its base's type and size
// tables.
struct GaragePart {
  GuestPart *gp;
  uint32_t record;
  uint32_t sizes_aid;  // its base type's size table
};

// Once, from the inventory build (the game thread) with class_groupings open:
// each part's own garage item (see the file comment). A variant gets a size of
// its own in its base's type; a new kind of part (scripted), or a variant of a
// type without sizes, a type of its own in its base's category (with a size
// table holding its one size when its base type has sizes).
void PrepareSizes(PPCContext &ctx, uint8_t *base) {
  const uint32_t groupings = Load32(base, kBlockClassGroupings);
  if (!groupings || !g_scratch_aid_va || g_sizes_prepared.exchange(true))
    return;

  // The parts by the table they join: variants by size table, new kinds by
  // type table.
  std::vector<std::pair<uint32_t, std::vector<GaragePart>>> by_size_table, by_type_table;
  auto add = [](auto &tables, uint32_t aid, GaragePart part) {
    auto it = std::find_if(tables.begin(), tables.end(), [&](const auto &e) { return e.first == aid; });
    if (it == tables.end())
      it = tables.insert(tables.end(), {aid, {}});
    it->second.push_back(part);
  };
  for (GuestPart &gp : g_parts) {
    const uint32_t record = BuildPart(ctx, base, gp, 1);
    if (!record)
      continue;
    const std::string category = GuestString(base, record + kRecordCategory, kRecordGroupingNameSize);
    const std::string type = GuestString(base, record + kRecordType, kRecordGroupingNameSize);
    const uint32_t category_record = FindGrouping(ctx, base, groupings, category);
    const uint32_t types_aid = category_record ? Load32(base, category_record + kGroupingNext) : 0;
    bool type_found = false;
    uint32_t sizes_aid = 0;  // 0: a type without sizes (its parts listed one by one, as gadgets)
    if (category_record) {
      ctx.r3.u64 = category_record + kGroupingNext;
      ctx.r4.u64 = 1;
      __imp__assetManOpenParams(ctx, base);
      if (const uint32_t types = ctx.r3.u32) {
        if (const uint32_t type_record = FindGrouping(ctx, base, types, type)) {
          type_found = true;
          sizes_aid = Load32(base, type_record + kGroupingNext);
        }
        ctx.r3.u64 = types;
        __imp__sub_82235E68(ctx, base);
      }
    }
    if (!type_found) {
      RNUT_WARN("Custom part '{}': the garage has no category '{}' / type '{}' for it; it may not show",
                gp.part->id, category, type);
      continue;
    }
    // A new kind of part, or a variant of a type without sizes (gadgets,
    // wheels: one item per type, which its base already has), gets a type of
    // its own; a variant of a sized type, a size of its own.
    if (!gp.part->script.empty() || !sizes_aid)
      add(by_type_table, types_aid, {&gp, record, sizes_aid});
    else
      add(by_size_table, sizes_aid, {&gp, record, sizes_aid});
  }

  for (auto &[sizes_aid, parts] : by_size_table) {
    std::vector<uint8_t> sizes = ReadGroupingTable(ctx, base, sizes_aid);
    if (sizes.empty())
      continue;
    const std::vector<uint8_t> original = sizes;
    for (const GaragePart &p : parts) {
      const std::string base_size = GuestString(base, p.record + kRecordSizeName, kRecordGroupingNameSize);
      const std::vector<uint8_t> own = OwnGroupingRecord(original, GroupingRecordOffset(original, base_size), *p.gp);
      sizes.insert(sizes.end(), own.begin(), own.end());
      PutString(base + p.record + kRecordSizeName, kSizePrefix + p.gp->part->id, kRecordGroupingNameSize);
    }
    ServeGroupingTable(base, sizes_aid, sizes);
    RNUT_INFO("Custom parts: {} size(s) added to the garage's size table {:08X}", parts.size(), sizes_aid);
  }

  for (auto &[types_aid, parts] : by_type_table) {
    std::vector<uint8_t> types = ReadGroupingTable(ctx, base, types_aid);
    if (types.empty())
      continue;
    const std::vector<uint8_t> original = types;
    for (const GaragePart &p : parts) {
      const std::string own_name = kSizePrefix + p.gp->part->id;
      // Its type: its base type's record, renamed.
      const std::string base_type = GuestString(base, p.record + kRecordType, kRecordGroupingNameSize);
      std::vector<uint8_t> own = OwnGroupingRecord(original, GroupingRecordOffset(original, base_type), *p.gp);
      if (p.sizes_aid) {
        // Its one size: its base size's record, renamed, in a table of its
        // own that its type points at.
        const std::vector<uint8_t> base_sizes = ReadGroupingTable(ctx, base, p.sizes_aid);
        if (base_sizes.empty())
          continue;
        const std::string base_size = GuestString(base, p.record + kRecordSizeName, kRecordGroupingNameSize);
        const uint32_t own_sizes_aid = renut::mods::AssetId(kTypeMisc, kOwnSizesPrefix + p.gp->part->id);
        ServeGroupingTable(base, own_sizes_aid,
                           OwnGroupingRecord(base_sizes, GroupingRecordOffset(base_sizes, base_size), *p.gp));
        Store32(own.data(), kGroupingNext, own_sizes_aid);
        PutString(base + p.record + kRecordSizeName, own_name, kRecordGroupingNameSize);
      }
      types.insert(types.end(), own.begin(), own.end());
      PutString(base + p.record + kRecordType, own_name, kRecordGroupingNameSize);
    }
    ServeGroupingTable(base, types_aid, types);
    RNUT_INFO("Custom parts: {} type(s) added to the garage's type table {:08X}", parts.size(), types_aid);
  }
}

} // namespace

// void *assetManOpenParams(const u32 *aid, int may_fail): the asset's data.
REX_HOOK_RAW(assetManOpenParams) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t may_fail = ctx.r4.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  const uint8_t type = uint8_t(aid >> 24);
  if (!aid || (type != kTypeObjparams && type != kTypeMisc)) {
    __imp__assetManOpenParams(ctx, base);
    return;
  }
  if (g_any_size_table.load(std::memory_order_acquire)) {
    std::lock_guard lock(g_mutex);
    for (const SizeTable &table : g_size_tables) {
      if (table.aid == aid) {
        ctx.r3.u64 = table.data;
        return;
      }
    }
  }
  Initialize(base);
  if (g_blockset_aid && aid == g_blockset_aid) {
    ctx.r3.u64 = g_blockset;
    return;
  }
  if (GuestPart *gp = FindByAid(aid)) {
    ctx.r3.u64 = BuildPart(ctx, base, *gp, may_fail);
    return;
  }
  if (type == kTypeObjparams) {
    if (const KnownCustomPart *known = FindRemovedPart(aid)) {
      LogRemoved(*known);
      const uint32_t word = KnownBaseWord(aid);
      if (!known->base_aid || !word) {
        ctx.r3.u64 = 0;
        return;
      }
      ctx.r3.u64 = word;
      ctx.r4.u64 = may_fail;
      __imp__assetManOpenParams(ctx, base);
      return;
    }
    if (!Exists(ctx, base, aid_va, aid, "open")) {
      ctx.r3.u64 = g_placeholder_aid_va;
      ctx.r4.u64 = may_fail;
      __imp__assetManOpenParams(ctx, base);
      return;
    }
  }
  ctx.r3.u64 = aid_va;
  ctx.r4.u64 = may_fail;
  __imp__assetManOpenParams(ctx, base);
}

// sub_822C0CE8(const u32 *aid, callback, ..., int may_fail (r7), ...): opens
// an asset asynchronously (the request takes the aid at once).
REX_HOOK_RAW(sub_822C0CE8) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  if (!aid || uint8_t(aid >> 24) != kTypeObjparams) {
    __imp__sub_822C0CE8(ctx, base);
    return;
  }
  Initialize(base);
  if (!FindByAid(aid)) {
    const KnownCustomPart *known = FindRemovedPart(aid);
    if (known)
      LogRemoved(*known);
    if (const uint32_t word = known ? KnownBaseWord(aid) : 0; known && known->base_aid && word) {
      ctx.r3.u64 = word;
    } else {
      // Exists() uses r3-r4 (and what the game call clobbers): the async
      // open's arguments are put back.
      const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64, r8 = ctx.r8.u64,
                     r9 = ctx.r9.u64;
      const bool exists = !known && Exists(ctx, base, aid_va, aid, "asynchronous open");
      ctx.r3.u64 = exists ? aid_va : g_placeholder_aid_va;
      ctx.r4.u64 = r4;
      ctx.r5.u64 = r5;
      ctx.r6.u64 = r6;
      ctx.r7.u64 = r7;
      ctx.r8.u64 = r8;
      ctx.r9.u64 = r9;
    }
  }
  __imp__sub_822C0CE8(ctx, base);
}

// sub_82273078(state, opener): a synchronous open's completion; a failed one
// (state + 12) stops the game with the dirty disc error unless opener + 312.
// Our own checks fail quietly.
REX_HOOK_RAW(sub_82273078) {
  const uint32_t state = ctx.r3.u32;
  const uint32_t opener = ctx.r4.u32;
  if (g_checks_running.load(std::memory_order_acquire) > 0 && state && opener && Load32(base, state + 12))
    Store32(base, opener + 312, 1);
  __imp__sub_82273078(ctx, base);
}

// XShowDirtyDiscErrorUI(user): the dirty disc dialog, then a reboot (never
// returns). Skipped for the failures of our own checks.
REX_HOOK_RAW(rex_XShowDirtyDiscErrorUI) {
  if (g_checks_running.load(std::memory_order_acquire) > 0) {
    RNUT_INFO("Custom parts: the dirty disc error for a missing vehicle part was skipped");
    return;
  }
  __imp__rex_XShowDirtyDiscErrorUI(ctx, base);
}

// u32 *sub_8227CA10(const u32 *aid): a malloc'd copy of a part's objparams
// (+ 0x88 the aid), or null; the block spawn leaves a null one out. A removed
// mod's part becomes its base part.
REX_HOOK_RAW(sub_8227CA10) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  uint32_t base_aid = 0;
  if (aid && uint8_t(aid >> 24) == kTypeObjparams) {
    Initialize(base);
    if (const KnownCustomPart *known = FindRemovedPart(aid))
      base_aid = known->base_aid;
  }
  __imp__sub_8227CA10(ctx, base);
  if (base_aid && ctx.r3.u32)
    Store32(base, ctx.r3.u32 + kRecordAid, base_aid);
}

// sub_8256C1B8(const u32 *aid, ...): spawns a vehicle block, or returns null
// (the vehicle spawn goes on without it). A missing part's block is left out.
REX_HOOK_RAW(sub_8256C1B8) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  if (aid && uint8_t(aid >> 24) == kTypeObjparams) {
    Initialize(base);
    if (!FindByAid(aid) && !FindRemovedPart(aid)) {
      const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64;
      const bool exists = Exists(ctx, base, aid_va, aid, "block spawn");
      if (!exists) {
        ctx.r3.u64 = 0;
        return;
      }
      ctx.r3.u64 = aid_va;
      ctx.r4.u64 = r4;
      ctx.r5.u64 = r5;
      ctx.r6.u64 = r6;
      ctx.r7.u64 = r7;
    }
  }
  __imp__sub_8256C1B8(ctx, base);
}

// sub_82235E68(data): lets an opened asset go. Custom records stay.
REX_HOOK_RAW(sub_82235E68) {
  if (OwnSize(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82235E68(ctx, base);
}

// u32 sub_822C1148(data): an opened asset's size.
REX_HOOK_RAW(sub_822C1148) {
  if (const uint32_t size = OwnSize(ctx.r3.u32)) {
    ctx.r3.u64 = size;
    return;
  }
  __imp__sub_822C1148(ctx, base);
}

// sub_8251CB20(): rebuilds the garage inventory; the custom parts join it.
REX_HOOK_RAW(sub_8251CB20) {
  __imp__sub_8251CB20(ctx, base);
  if (renut::mods::CustomParts().empty() || !Load32(base, kBlockClassGroupings))
    return;
  const uint64_t result = ctx.r3.u64;
  Initialize(base);
  PrepareSizes(ctx, base);
  if (g_blockset_aid) {
    ctx.r3.u64 = g_blockset_aid_va;
    ctx.r4.u64 = 0;
    __imp__sub_8251CDD8(ctx, base);
    static bool logged = false;
    if (!logged) {
      logged = true;
      RNUT_INFO("Custom parts: {} part(s) added to the garage inventory", g_parts.size());
    }
  }
  ctx.r3.u64 = result;
}

// int sub_82396E00(text, const char *key, wchar_t *out, size_t out_size, int upper):
// the text for "<section>__<key>"; renutpart_<id> is a custom part's name.
REX_HOOK_RAW(sub_82396E00) {
  const uint32_t key_va = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t out_size = ctx.r6.u32;
  if (!key_va || !out || !out_size || renut::mods::CustomParts().empty()) {
    __imp__sub_82396E00(ctx, base);
    return;
  }
  char key[kMaxTextKey];
  size_t length = 0;
  while (length + 1 < kMaxTextKey && base[key_va + length])
    key[length] = char(base[key_va + length]), ++length;
  key[length] = 0;
  const char *own = std::strstr(key, "__renutpart_");
  if (!own) {
    __imp__sub_82396E00(ctx, base);
    return;
  }
  own += 2;
  Initialize(base);
  const std::u16string *name = nullptr;
  for (const GuestPart &gp : g_parts)
    if (gp.text_key == own && !gp.name.empty())
      name = &gp.name;
  if (!name) {
    __imp__sub_82396E00(ctx, base);
    return;
  }
  const bool upper = ctx.r7.u32 && Load32(base, kTextUpperCaseMode) < 2;
  const uint32_t n = uint32_t(std::min<size_t>(name->size(), out_size - 1));
  for (uint32_t i = 0; i < n; ++i) {
    char16_t c = (*name)[i];
    if (upper && c >= u'a' && c <= u'z')
      c = char16_t(c - u'a' + u'A');
    Store16(base, out + 2 * i, uint16_t(c));
  }
  Store16(base, out + 2 * n, 0);
  ctx.r3.u64 = 1;
}
