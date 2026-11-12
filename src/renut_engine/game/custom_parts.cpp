#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/part_api.h"
#include "renut_engine/game/part_classes.h"
#include "renut_engine/game/restored_parts.h"
#include "renut_engine/gpu/render/shape_weights.h"
#include "renut_engine/mods/custom_parts.h"

REX_EXTERN(__imp__rex_assetManOpenParams);
REX_EXTERN(__imp__sub_8228ECA0);
REX_EXTERN(__imp__sub_82204DF0);
REX_EXTERN(__imp__sub_8228F0F0);
REX_EXTERN(__imp__sub_824FA3D8);
REX_EXTERN(__imp__sub_824FA620);
REX_EXTERN(__imp__sub_82363A50);
REX_EXTERN(__imp__sub_8224AA08);
REX_EXTERN(__imp__rex_XShowDirtyDiscErrorUI);
REX_EXTERN(__imp__sub_82241780);  
REX_EXTERN(__imp__sub_8254FAC0);  
REX_EXTERN(__imp__sub_8260EFC8);
REX_EXTERN(__imp__sub_8260AEE8);  
REX_EXTERN(__imp__sub_824FAF08);  

namespace {

using renut::mods::CustomPart;
using renut::mods::KnownCustomPart;

// System heap memory, zeroed; 0 when there's none.
uint32_t SystemAlloc(uint32_t size) {
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  return memory->Allocate(size, 0x20, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem)
      .value_or(0);
}

void SystemFree(uint32_t address) {
  if (rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY(); memory && address)
    (void)memory->Free(address);
}

constexpr uint8_t kTypeObjparams = 0x1F;
constexpr uint8_t kTypeMisc = 0x0B;
constexpr uint32_t kBlockClassGroupings = 0x82E19E40;
constexpr uint32_t kTextUpperCaseMode = 0x82D9A068;
constexpr uint32_t kRecordNameKey = 0xA0;
constexpr uint32_t kRecordNameKeySize = 64;
constexpr uint32_t kRecordAid = 0x88;
constexpr uint32_t kRecordAidNext = 0x8C;
constexpr uint32_t kRecordClassName = 0x4B;
constexpr uint32_t kRecordClassNameSize = 0x80 - kRecordClassName;
constexpr const char kScriptedClassPrefix[] = "renut_";
constexpr uint32_t kRecordCategory = 0x228;
constexpr uint32_t kRecordType = 0x268;
constexpr uint32_t kRecordSizeName = 0x2A8;
constexpr uint32_t kRecordGroupingNameSize = 64;
constexpr uint32_t kPartCollision = 0x1D0;
constexpr uint32_t kHkpBoxShapeVtable = 0x82003ADC;
constexpr float kBoxRadius = 0.05f;
constexpr float kBoxHalfExtent = 0.5f - kBoxRadius;
constexpr uint32_t kPartInstance = 896;
constexpr uint32_t kPartPaint = 912;
constexpr uint32_t kPartModel = 928;
constexpr uint32_t kInstanceDrawData = 8;
constexpr uint32_t kDrawDataC28 = 112;
constexpr uint32_t kRecordDescription = 0x120;
constexpr uint32_t kRecordSeenFlag = 0x150;
constexpr uint32_t kRecordSeenFlagSize = 64;
constexpr uint32_t kMinRecordSize = kRecordSizeName + kRecordGroupingNameSize;
constexpr uint32_t kRecordModel = 0x124;
constexpr uint32_t kRecordAttach = 0x12C;
constexpr uint8_t kTypeModel = 0x04;
constexpr uint8_t kTypeHavok = 0x05;
constexpr uint8_t kTypeAttach = 0x14;
constexpr uint32_t kAssetQuery = 0x821DDBF0;
constexpr uint32_t kAssetQueryVtable = 0x82001AF0;
constexpr uint32_t kAnyProvider = 0xFF;
constexpr uint32_t kGroupingSize = 204;
constexpr uint32_t kGroupingName = 0;
constexpr uint32_t kGroupingTextKey = 64;
constexpr uint32_t kGroupingNext = 128;         
constexpr uint32_t kGroupingDescription = 132;  
constexpr uint32_t kGroupingStringSize = 64;
constexpr uint8_t kTypeDialog = 0x43;
constexpr uint32_t kPlaceholderPartAid = 0x1F4BB5E1;
constexpr const char kSizePrefix[] = "renut_";
constexpr const char kOwnSizesPrefix[] = "banjox_renut_sizes_";
constexpr const char kPartAssetPrefix[] = "banjox_vehicleblock_renut_";
constexpr const char kBlocksetAsset[] = "banjox_blockset_renut_custom_parts";
constexpr const char kTextKeyPrefix[] = "renutpart_";
constexpr const char kSharedTypePrefix[] = "renut_type_";
constexpr const char kTypeTextKeyPrefix[] = "renuttype_";
constexpr const char kSharedSizesPrefix[] = "banjox_renut_type_sizes_";
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

std::string TypeSlug(const std::string &name) {
  std::string slug;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
    slug.push_back((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_');
  }

  return slug;
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

uint32_t LoadBig32(const std::vector<uint8_t> &bytes, uint32_t at) {
  return at + 4 <= bytes.size() ? uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3] : 0;
}

void PutString(uint8_t *at, const std::string &s, size_t field) {
  std::memset(at, 0, field);
  std::memcpy(at, s.data(), std::min(s.size(), field - 1));
}

struct GuestPart {
  const CustomPart *part = nullptr;
  uint32_t aid = 0;
  uint32_t base_aid_va = 0;
  std::string text_key;
  std::u16string name;
  uint32_t record = 0;
  uint32_t size = 0;
  bool failed = false;
};

struct KnownBase {
  uint32_t aid = 0;
  uint32_t base_aid_va = 0;
};

struct SizeTable {
  uint32_t aid = 0;
  uint32_t data = 0;
  uint32_t size = 0;
};

std::mutex g_mutex;
bool g_initialized = false;
std::vector<GuestPart> g_parts;
std::vector<uint32_t> g_own_models; 
std::vector<std::pair<std::string, std::u16string>> g_type_names;
std::vector<KnownBase> g_known_bases;
uint32_t g_blockset_aid = 0;
uint32_t g_blockset_aid_va = 0;
uint32_t g_blockset = 0; 
uint32_t g_blockset_size = 0;
uint32_t g_scratch_aid_va = 0;
uint32_t g_placeholder_aid_va = 0;
std::vector<SizeTable> g_size_tables;
std::unordered_set<uint32_t> g_existing;
std::unordered_set<uint32_t> g_missing;
std::atomic<bool> g_any_size_table{false};
std::atomic<bool> g_sizes_prepared{false};
std::atomic<int> g_checks_running{0};
std::once_flag g_box_once;
uint32_t g_box_collision = 0;

void StoreFloat(uint8_t *base, uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, va, bits);
}

uint32_t BoxCollision(uint8_t *base) {
  std::call_once(g_box_once, [base] {
    const uint32_t block = SystemAlloc(0x110);
    if (!block)
      return;
    std::memset(base + block, 0, 0x110);
    const uint32_t asset = (block + 15) & ~15u;
    const uint32_t entries = asset + 0x10, entry = asset + 0x18, mass = asset + 0x40, shape = asset + 0x90;
    Store32(base, asset, entries);
    Store32(base, asset + 4, 1);
    Store32(base, asset + 8, shape);
    Store32(base, asset + 12, 1);
    Store32(base, entries, 3);
    Store32(base, entries + 4, entry);
    Store32(base, entry + 24, mass);
    StoreFloat(base, mass, 1.0f);
    StoreFloat(base, mass + 4, 1.0f);
    for (uint32_t row = 0; row < 3; ++row)
    StoreFloat(base, mass + 32 + 16 * row + 4 * row, 1.0f / 6.0f);
    Store32(base, shape, kHkpBoxShapeVtable);
    Store16(base, shape + 6, 1);
    Store32(base, shape + 12, 5);
    StoreFloat(base, shape + 16, kBoxRadius);
    for (uint32_t i = 0; i < 4; ++i)
    StoreFloat(base, shape + 32 + 4 * i, kBoxHalfExtent);
    g_box_collision = asset;
  });

  return g_box_collision;
}

void Initialize(uint8_t *base) {
  std::lock_guard lock(g_mutex);
  if (g_initialized)
    return;
  g_initialized = true;
  const std::vector<CustomPart> &parts = renut::mods::CustomParts();
  const std::vector<renut::game::RestoredPart> restored = renut::game::RestoredParts();
  const uint32_t count = uint32_t(parts.size());
  const uint32_t known_count = uint32_t(renut::mods::KnownCustomParts().size());
  g_blockset_size = 8 * (count + uint32_t(restored.size()));
  const uint32_t block = SystemAlloc(4 * count + 12 + g_blockset_size + 4 * known_count);
  if (!block)
  return;

  g_blockset_aid_va = block + 4 * count;
  g_scratch_aid_va = g_blockset_aid_va + 4;
  g_placeholder_aid_va = g_scratch_aid_va + 4;
  g_blockset = g_placeholder_aid_va + 4;
  Store32(base, g_placeholder_aid_va, kPlaceholderPartAid);
  const uint32_t known_words = g_blockset + g_blockset_size;

  if (g_blockset_size) {
    g_blockset_aid = renut::mods::AssetId(kTypeMisc, kBlocksetAsset);
    Store32(base, g_blockset_aid_va, g_blockset_aid);
  }

  for (uint32_t i = 0; i < restored.size(); ++i) {
    Store32(base, g_blockset + 8 * (count + i), restored[i].count);
    Store32(base, g_blockset + 8 * (count + i) + 4, restored[i].aid);
  }

  for (uint32_t i = 0; i < count; ++i) {
    GuestPart gp;
    gp.part = &parts[i];
    gp.aid = renut::mods::AssetId(kTypeObjparams, kPartAssetPrefix + parts[i].id);
    gp.base_aid_va = block + 4 * i;
    Store32(base, gp.base_aid_va, parts[i].base.empty() ? 0 : renut::mods::AssetId(kTypeObjparams, parts[i].base));
    gp.text_key = kTextKeyPrefix + parts[i].id;
    gp.name = Utf16(parts[i].name);

    if (!parts[i].garage_type.empty()) {
      const std::string key = kTypeTextKeyPrefix + TypeSlug(parts[i].garage_type);
      if (std::none_of(g_type_names.begin(), g_type_names.end(), [&](const auto &t) { return t.first == key; }))
        g_type_names.push_back({key, Utf16(parts[i].garage_type)});
    }

    Store32(base, g_blockset + 8 * i, parts[i].count);
    Store32(base, g_blockset + 8 * i + 4, gp.aid);
    if (parts[i].own_record && !parts[i].model.empty())
      g_own_models.push_back(renut::mods::AssetId(kTypeModel, parts[i].model));
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

const KnownCustomPart *FindRemovedPart(uint32_t aid) {
  return FindByAid(aid) ? nullptr : renut::mods::FindKnownCustomPart(aid);
}

uint32_t KnownBaseWord(uint32_t aid) {
  for (const KnownBase &k : g_known_bases)
    if (k.aid == aid)
      return k.base_aid_va;
  return 0;
}

void MarkFailed(GuestPart &gp) {
  std::lock_guard lock(g_mutex);
  gp.failed = true;
}

bool AssetLoaded(PPCContext &ctx, uint8_t *base, uint32_t aid) {
  ::PPCFunc *loaded = renut::game::GuestFunction(kAssetQuery);
  if (!loaded)
    return false;
  const uint32_t query = SystemAlloc(8);
  if (!query)
    return false;
  Store32(base, query, kAssetQueryVtable);
  Store32(base, query + 4, aid);
  ctx.r3.u64 = query;
  ctx.r4.u64 = kAnyProvider;
  loaded(ctx, base);
  const bool there = (ctx.r3.u32 & 0xFF) != 0;
  SystemFree(query);
  return there;
}

void SetAssetField(PPCContext &ctx, uint8_t *base, std::vector<uint8_t> &bytes, uint32_t offset, uint8_t type, const std::string &name, bool own_record) {
  if (name.empty())
    return;
  const uint32_t aid = renut::mods::AssetId(type, name);
  if (!AssetLoaded(ctx, base, aid))
    return;

  if (type == kTypeModel && !own_record && !AssetLoaded(ctx, base, renut::mods::AssetId(kTypeHavok, name)))
    return;
  const uint32_t value = rex::byte_swap(aid);
  std::memcpy(bytes.data() + offset, &value, 4);
}

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
    __imp__rex_assetManOpenParams(ctx, base);
    const uint32_t source = ctx.r3.u32;

    if (!source) {
      MarkFailed(gp);
      return 0;
    }

    const uint32_t size = Load16(base, source);
    bytes.assign(base + source, base + source + size);
    ctx.r3.u64 = source;
    __imp__sub_82204DF0(ctx, base);
  }

  if (bytes.size() < kMinRecordSize) {
    MarkFailed(gp);
    return 0;
  }

  for (const renut::mods::CustomPartField &field : part.fields) {
    if (field.offset + field.bytes.size() <= bytes.size())
      std::memcpy(bytes.data() + field.offset, field.bytes.data(), field.bytes.size());
  }
  SetAssetField(ctx, base, bytes, kRecordModel, kTypeModel, part.model, part.own_record);
  SetAssetField(ctx, base, bytes, kRecordAttach, kTypeAttach, part.attach, part.own_record);

  if (part.own_record && (!LoadBig32(bytes, kRecordModel) || !LoadBig32(bytes, kRecordAttach))) {
    MarkFailed(gp);
    return 0;
  }

  if (!part.name.empty()) {
    uint8_t *key = bytes.data() + kRecordNameKey;
    const size_t old_length = strnlen(reinterpret_cast<const char *>(key), kRecordNameKeySize);
    std::memset(key, 0, std::min<size_t>(kRecordNameKeySize, std::max(old_length, gp.text_key.size()) + 1));
    std::memcpy(key, gp.text_key.data(), gp.text_key.size());
  }

  if (renut::mods::HasBehaviour(part)) {
    uint8_t *class_name = bytes.data() + kRecordClassName;
    const std::string parent(reinterpret_cast<const char *>(class_name), strnlen(reinterpret_cast<const char *>(class_name), kRecordClassNameSize));
    const std::string own = kScriptedClassPrefix + part.id;

    if (renut::game::RegisterScriptedPartClass(ctx, base, own, parent, &part)) {
      std::memset(class_name, 0, kRecordClassNameSize);
      std::memcpy(class_name, own.data(), std::min<size_t>(own.size(), kRecordClassNameSize - 1));
    }
  }

  const uint32_t record = SystemAlloc(uint32_t(bytes.size()));
  if (!record) {
    MarkFailed(gp);
    return 0;
  }

  std::memcpy(base + record, bytes.data(), bytes.size());
  Store32(base, record + kRecordAid, gp.aid);
  Store32(base, record + kRecordAidNext, 0);

  std::lock_guard lock(g_mutex);
  if (!gp.record) {  // (another thread may have built it meanwhile)
    gp.record = record;
    gp.size = uint32_t(bytes.size());
  }

  return gp.record;
}

uint32_t OwnSize(uint32_t data) {
  if (!data)
    return 0;
  if (data == g_box_collision)
    return 0x20;
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

bool Exists(PPCContext &ctx, uint8_t *base, uint32_t aid_va, uint32_t aid) {
  {
    std::lock_guard lock(g_mutex);
    if (g_existing.contains(aid))
      return true;
    if (g_missing.contains(aid))
      return false;
  }

  ctx.r3.u64 = aid_va;
  ctx.r4.u64 = 1;
  g_checks_running.fetch_add(1, std::memory_order_acq_rel);
  __imp__rex_assetManOpenParams(ctx, base);
  g_checks_running.fetch_sub(1, std::memory_order_acq_rel);
  const uint32_t data = ctx.r3.u32;

  if (data) {
    ctx.r3.u64 = data;
    __imp__sub_82204DF0(ctx, base);
  }

  std::lock_guard lock(g_mutex);

  if (data) {
    g_existing.insert(aid);
    return true;
  }

  g_missing.insert(aid);
  return false;
}

uint32_t AssetSize(PPCContext &ctx, uint8_t *base, uint32_t data) {
  ctx.r3.u64 = data;
  __imp__sub_8228F0F0(ctx, base);
  return ctx.r3.u32;
}

uint32_t FindGrouping(PPCContext &ctx, uint8_t *base, uint32_t table, const std::string &name) {
  const uint32_t count = AssetSize(ctx, base, table) / kGroupingSize;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t record = table + i * kGroupingSize;
    if (SameName(GuestString(base, record + kGroupingName, kGroupingStringSize), name))
      return record;
  }
  return 0;
}

std::vector<uint8_t> ReadGroupingTable(PPCContext &ctx, uint8_t *base, uint32_t aid) {
  Store32(base, g_scratch_aid_va, aid);
  ctx.r3.u64 = g_scratch_aid_va;
  ctx.r4.u64 = 1;
  __imp__rex_assetManOpenParams(ctx, base);
  const uint32_t data = ctx.r3.u32;
  if (!data)
    return {};
  const uint32_t size = AssetSize(ctx, base, data) / kGroupingSize * kGroupingSize;
  std::vector<uint8_t> bytes(base + data, base + data + size);
  ctx.r3.u64 = data;
  __imp__sub_82204DF0(ctx, base);
  return bytes;
}

size_t GroupingRecordOffset(const std::vector<uint8_t> &table, const std::string &name) {
  for (size_t at = 0; at + kGroupingSize <= table.size(); at += kGroupingSize) {
    const char *s = reinterpret_cast<const char *>(table.data() + at + kGroupingName);
    if (SameName(std::string(s, strnlen(s, kGroupingStringSize)), name))
      return at;
  }
  return 0;
}

std::vector<uint8_t> OwnGroupingRecord(const std::vector<uint8_t> &table, size_t from, const GuestPart &gp) {
  std::vector<uint8_t> record(table.begin() + from, table.begin() + from + kGroupingSize);
  PutString(record.data() + kGroupingName, kSizePrefix + gp.part->id, kGroupingStringSize);
  if (!gp.name.empty())
    PutString(record.data() + kGroupingTextKey, gp.text_key, kGroupingStringSize);
  return record;
}

void ServeGroupingTable(uint8_t *base, uint32_t aid, const std::vector<uint8_t> &bytes) {
  const uint32_t data = SystemAlloc(uint32_t(bytes.size()));

  if (!data)
    return;

  std::memcpy(base + data, bytes.data(), bytes.size());
  {
    std::lock_guard lock(g_mutex);
    g_size_tables.push_back({aid, data, uint32_t(bytes.size())});
  }

  g_any_size_table.store(true, std::memory_order_release);
}

struct GaragePart {
  GuestPart *gp;
  uint32_t record;
  uint32_t sizes_aid;
};

void PrepareSizes(PPCContext &ctx, uint8_t *base) {
  const uint32_t groupings = Load32(base, kBlockClassGroupings);
  if (!groupings || !g_scratch_aid_va || g_sizes_prepared.exchange(true))
    return;

  std::vector<std::pair<uint32_t, std::vector<GaragePart>>> by_size_table, by_type_table;
  auto add = [](auto &tables, uint32_t aid, GaragePart part) {
    auto it = std::find_if(tables.begin(), tables.end(), [&](const auto &e) { return e.first == aid; });

    if (it == tables.end())
      it = tables.insert(tables.end(), {aid, {}});
    it->second.push_back(part);
  };

  struct SharedType {
    uint32_t types_aid;
    std::string slug;
    std::vector<GaragePart> parts;
  };

  std::vector<SharedType> shared;
  struct OwnType {
    uint32_t types_aid;
    uint32_t category;  // its record in class_groupings
    std::string slug;
    std::vector<GaragePart> parts;
  };

  std::vector<OwnType> own_types;
  for (GuestPart &gp : g_parts) {
    const uint32_t record = BuildPart(ctx, base, gp, 1);

    if (!record)
      continue;
    const std::string category = GuestString(base, record + kRecordCategory, kRecordGroupingNameSize);
    const std::string type = GuestString(base, record + kRecordType, kRecordGroupingNameSize);
    const uint32_t category_record = FindGrouping(ctx, base, groupings, category);
    const uint32_t types_aid = category_record ? Load32(base, category_record + kGroupingNext) : 0;
    bool type_found = false;
    uint32_t sizes_aid = 0;

    if (category_record) {
      ctx.r3.u64 = category_record + kGroupingNext;
      ctx.r4.u64 = 1;
      __imp__rex_assetManOpenParams(ctx, base);

      if (const uint32_t types = ctx.r3.u32) {

        if (const uint32_t type_record = FindGrouping(ctx, base, types, type)) {
          type_found = true;
          sizes_aid = Load32(base, type_record + kGroupingNext);
        }

        ctx.r3.u64 = types;
        __imp__sub_82204DF0(ctx, base);
      }
    }

    if (gp.part->own_record) {
      if (!category_record || !types_aid)
        continue;

      const std::string slug = TypeSlug(gp.part->garage_type.empty() ? gp.part->id : gp.part->garage_type);
      auto it = std::find_if(own_types.begin(), own_types.end(), [&](const OwnType &o) { return o.types_aid == types_aid && o.slug == slug; });

      if (it == own_types.end())
        it = own_types.insert(own_types.end(), {types_aid, category_record, slug, {}});
      it->parts.push_back({&gp, record, 0});
      continue;
    }

    if (!type_found)
      continue;

    if (!gp.part->garage_type.empty() && sizes_aid) {
      const std::string slug = TypeSlug(gp.part->garage_type);
      auto it = std::find_if(shared.begin(), shared.end(), [&](const SharedType &s) { return s.types_aid == types_aid && s.slug == slug; });

      if (it == shared.end())
        it = shared.insert(shared.end(), {types_aid, slug, {}});
      it->parts.push_back({&gp, record, sizes_aid});
      continue;
    }

    if (renut::mods::HasBehaviour(*gp.part) || !sizes_aid)
      add(by_type_table, types_aid, {&gp, record, sizes_aid});
    else
      add(by_size_table, sizes_aid, {&gp, record, sizes_aid});
  }

  std::vector<std::pair<uint32_t, std::vector<std::vector<uint8_t>>>> restored_types;
  for (const renut::game::RestoredPart &rp : renut::game::RestoredParts()) {
    Store32(base, g_scratch_aid_va, rp.aid);
    ctx.r3.u64 = g_scratch_aid_va;
    ctx.r4.u64 = 1;
    __imp__rex_assetManOpenParams(ctx, base);
    const uint32_t record = ctx.r3.u32;

    if (!record)
      continue;

    const std::string category = GuestString(base, record + kRecordCategory, kRecordGroupingNameSize);
    const std::string type = GuestString(base, record + kRecordType, kRecordGroupingNameSize);
    const std::string name_key = GuestString(base, record + kRecordNameKey, kRecordNameKeySize);
    const std::string class_name = GuestString(base, record + kRecordClassName, kRecordClassNameSize);
    ctx.r3.u64 = record;
    __imp__sub_82204DF0(ctx, base);
    const uint32_t category_record = FindGrouping(ctx, base, groupings, category);
    const uint32_t types_aid = category_record ? Load32(base, category_record + kGroupingNext) : 0;
    const std::vector<uint8_t> types = types_aid ? ReadGroupingTable(ctx, base, types_aid) : std::vector<uint8_t>();

    if (types.empty() || GroupingRecordOffset(types, type) != 0 ||
        SameName(GuestString(types.data(), kGroupingName, kGroupingStringSize), type))
      continue;

    std::vector<uint8_t> own(types.begin(), types.begin() + kGroupingSize);
    PutString(own.data() + kGroupingName, type, kGroupingStringSize);
    PutString(own.data() + kGroupingTextKey, name_key, kGroupingStringSize);
    Store32(own.data(), kGroupingNext, 0);
    Store32(own.data(), kGroupingDescription, renut::mods::AssetId(kTypeDialog, "banjox_garage_description_block_" + name_key));
    constexpr const char kSeenFlag[] = "gameFlag_";
    const std::string prefix = "vehicleBlock";
    const std::string short_class = class_name.rfind(prefix, 0) == 0 ? class_name.substr(prefix.size()) : class_name;
    for (size_t at = kGroupingNext + 4; at + sizeof(kSeenFlag) - 1 < kGroupingSize; ++at) {
      if (std::memcmp(own.data() + at, kSeenFlag, sizeof(kSeenFlag) - 1) == 0) {
        PutString(own.data() + at, "gameFlag_Normal_Garage_Group_Seen_Block_" + short_class,
                  std::min<size_t>(kGroupingSize - at, kGroupingStringSize));
        break;
      }
    }

    auto it = std::find_if(restored_types.begin(), restored_types.end(), [&](const auto &e) { return e.first == types_aid; });

    if (it == restored_types.end())
      it = restored_types.insert(restored_types.end(), {types_aid, {}});
    it->second.push_back(std::move(own));
  }

  for (const SharedType &st : shared) {
    const std::vector<uint8_t> types = ReadGroupingTable(ctx, base, st.types_aid);
    if (types.empty())
      continue;

    const std::string own_type = kSharedTypePrefix + st.slug;
    const std::string lead_type = GuestString(base, st.parts.front().record + kRecordType, kRecordGroupingNameSize);
    const size_t from = GroupingRecordOffset(types, lead_type);
    std::vector<uint8_t> type_record(types.begin() + from, types.begin() + from + kGroupingSize);
    PutString(type_record.data() + kGroupingName, own_type, kGroupingStringSize);
    PutString(type_record.data() + kGroupingTextKey, kTypeTextKeyPrefix + st.slug, kGroupingStringSize);
    std::vector<uint8_t> sizes;
    for (const GaragePart &p : st.parts) {
      const std::vector<uint8_t> base_sizes = ReadGroupingTable(ctx, base, p.sizes_aid);

      if (base_sizes.empty())
        continue;

      const std::string base_size = GuestString(base, p.record + kRecordSizeName, kRecordGroupingNameSize);
      const std::vector<uint8_t> own = OwnGroupingRecord(base_sizes, GroupingRecordOffset(base_sizes, base_size), *p.gp);
      sizes.insert(sizes.end(), own.begin(), own.end());
      PutString(base + p.record + kRecordSizeName, kSizePrefix + p.gp->part->id, kRecordGroupingNameSize);
      PutString(base + p.record + kRecordType, own_type, kRecordGroupingNameSize);
    }

    if (sizes.empty())
      continue;

    const uint32_t sizes_aid = renut::mods::AssetId(kTypeMisc, kSharedSizesPrefix + st.slug);
    ServeGroupingTable(base, sizes_aid, sizes);
    Store32(type_record.data(), kGroupingNext, sizes_aid);
    auto it = std::find_if(restored_types.begin(), restored_types.end(), [&](const auto &e) { return e.first == st.types_aid; });

    if (it == restored_types.end())
      it = restored_types.insert(restored_types.end(), {st.types_aid, {}});
    it->second.push_back(std::move(type_record));
  }

  for (const OwnType &ot : own_types) {
    const uint32_t description = Load32(base, ot.category + kGroupingDescription);
    const std::string seen = GuestString(base, ot.category + kGroupingDescription + 4, kGroupingStringSize);
    const auto grouping = [&](const std::string &name, const std::string &text, uint32_t next, uint32_t desc) {
      std::vector<uint8_t> r(kGroupingSize, 0);
      PutString(r.data() + kGroupingName, name, kGroupingStringSize);
      PutString(r.data() + kGroupingTextKey, text, kGroupingStringSize);
      Store32(r.data(), kGroupingNext, next);
      Store32(r.data(), kGroupingDescription, desc);
      PutString(r.data() + kGroupingDescription + 4, seen, kGroupingStringSize);
      Store32(r.data(), kGroupingSize - 4, 0x40400000);  // 3.0, as in every grouping record
      return r;
    };

    const std::string own_type = kSharedTypePrefix + ot.slug;
    std::vector<uint8_t> sizes;
    for (const GaragePart &p : ot.parts) {

      if (!Load32(base, p.record + kRecordDescription))
        Store32(base, p.record + kRecordDescription, description);

      if (!base[p.record + kRecordSeenFlag])
        PutString(base + p.record + kRecordSeenFlag, seen, kRecordSeenFlagSize);

      const std::string size_name = kSizePrefix + p.gp->part->id;
      const std::vector<uint8_t> size = grouping(size_name, p.gp->text_key, 0, Load32(base, p.record + kRecordDescription));
      sizes.insert(sizes.end(), size.begin(), size.end());
      PutString(base + p.record + kRecordSizeName, size_name, kRecordGroupingNameSize);
      PutString(base + p.record + kRecordType, own_type, kRecordGroupingNameSize);
    }

    const uint32_t sizes_aid = renut::mods::AssetId(kTypeMisc, kSharedSizesPrefix + ot.slug);
    ServeGroupingTable(base, sizes_aid, sizes);
    const GuestPart &lead = *ot.parts.front().gp;
    const std::string type_text = lead.part->garage_type.empty() ? lead.text_key : kTypeTextKeyPrefix + ot.slug;
    auto it = std::find_if(restored_types.begin(), restored_types.end(), [&](const auto &e) { return e.first == ot.types_aid; });
    if (it == restored_types.end())
      it = restored_types.insert(restored_types.end(), {ot.types_aid, {}});
    it->second.push_back(grouping(own_type, type_text, sizes_aid, description));
  }

  auto take_restored = [&](uint32_t types_aid) {
    std::vector<std::vector<uint8_t>> records;
    auto it = std::find_if(restored_types.begin(), restored_types.end(), [&](const auto &e) { return e.first == types_aid; });

    if (it != restored_types.end()) {
      records = std::move(it->second);
      restored_types.erase(it);
    }

    return records;
  };

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
        const std::vector<uint8_t> base_sizes = ReadGroupingTable(ctx, base, p.sizes_aid);

        if (base_sizes.empty())
          continue;

        const std::string base_size = GuestString(base, p.record + kRecordSizeName, kRecordGroupingNameSize);
        const uint32_t own_sizes_aid = renut::mods::AssetId(kTypeMisc, kOwnSizesPrefix + p.gp->part->id);
        ServeGroupingTable(base, own_sizes_aid, OwnGroupingRecord(base_sizes, GroupingRecordOffset(base_sizes, base_size), *p.gp));
        Store32(own.data(), kGroupingNext, own_sizes_aid);
        PutString(base + p.record + kRecordSizeName, own_name, kRecordGroupingNameSize);
      }

      types.insert(types.end(), own.begin(), own.end());
      PutString(base + p.record + kRecordType, own_name, kRecordGroupingNameSize);
    }

    for (const std::vector<uint8_t> &own : take_restored(types_aid))
      types.insert(types.end(), own.begin(), own.end());
    ServeGroupingTable(base, types_aid, types);
  }

  while (!restored_types.empty()) {
    const uint32_t types_aid = restored_types.front().first;
    std::vector<uint8_t> types = ReadGroupingTable(ctx, base, types_aid);
    const std::vector<std::vector<uint8_t>> records = take_restored(types_aid);
    if (types.empty())
      continue;
    for (const std::vector<uint8_t> &own : records)
      types.insert(types.end(), own.begin(), own.end());
    ServeGroupingTable(base, types_aid, types);
  }
}

}

REX_HOOK_RAW(rex_assetManOpenParams) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t may_fail = ctx.r4.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  const uint8_t type = uint8_t(aid >> 24);

  if (!aid || (type != kTypeObjparams && type != kTypeMisc)) {
    __imp__rex_assetManOpenParams(ctx, base);
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
    // A removed mod's part: its base part, or nothing when it has none.
    if (const KnownCustomPart *known = FindRemovedPart(aid)) {
      const uint32_t word = KnownBaseWord(aid);
      if (!known->base_aid || !word) {
        ctx.r3.u64 = 0;
        return;
      }

      ctx.r3.u64 = word;
      ctx.r4.u64 = may_fail;
      __imp__rex_assetManOpenParams(ctx, base);
      return;
    }

    if (!Exists(ctx, base, aid_va, aid)) {
      ctx.r3.u64 = g_placeholder_aid_va;
      ctx.r4.u64 = may_fail;
      __imp__rex_assetManOpenParams(ctx, base);
      return;
    }
  }

  ctx.r3.u64 = aid_va;
  ctx.r4.u64 = may_fail;
  __imp__rex_assetManOpenParams(ctx, base);
}

REX_HOOK_RAW(sub_8228ECA0) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;

  if (!aid || uint8_t(aid >> 24) != kTypeObjparams) {
    __imp__sub_8228ECA0(ctx, base);
    return;
  }

  Initialize(base);

  if (!FindByAid(aid)) {
    const KnownCustomPart *known = FindRemovedPart(aid);

    if (const uint32_t word = known ? KnownBaseWord(aid) : 0; known && known->base_aid && word) {
      ctx.r3.u64 = word;
    } else {

      const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64, r8 = ctx.r8.u64, r9 = ctx.r9.u64;
      const bool exists = !known && Exists(ctx, base, aid_va, aid);
      ctx.r3.u64 = exists ? aid_va : g_placeholder_aid_va;
      ctx.r4.u64 = r4;
      ctx.r5.u64 = r5;
      ctx.r6.u64 = r6;
      ctx.r7.u64 = r7;
      ctx.r8.u64 = r8;
      ctx.r9.u64 = r9;
    }
  }

  __imp__sub_8228ECA0(ctx, base);
}

REX_HOOK_RAW(sub_82241780) {
  const uint32_t state = ctx.r3.u32;
  const uint32_t opener = ctx.r4.u32;

  if (state && opener && Load32(base, state + 12) &&
      (g_checks_running.load(std::memory_order_acquire) > 0 ||
       renut::game::part_api::QuietFailedOpen(Load32(base, opener + 12))))
    Store32(base, opener + 312, 1);
  __imp__sub_82241780(ctx, base);
}

REX_HOOK_RAW(rex_XShowDirtyDiscErrorUI) {
  if (g_checks_running.load(std::memory_order_acquire) > 0 || renut::game::part_api::Spawning())
    return;
  __imp__rex_XShowDirtyDiscErrorUI(ctx, base);
}

REX_HOOK_RAW(sub_8224AA08) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  uint32_t base_aid = 0;
  if (aid && uint8_t(aid >> 24) == kTypeObjparams) {
    Initialize(base);
    if (const KnownCustomPart *known = FindRemovedPart(aid))
      base_aid = known->base_aid;
  }

  __imp__sub_8224AA08(ctx, base);
  if (base_aid && ctx.r3.u32)
    Store32(base, ctx.r3.u32 + kRecordAid, base_aid);
}

REX_HOOK_RAW(sub_8254FAC0) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;

  if (aid && uint8_t(aid >> 24) == kTypeObjparams) {
    Initialize(base);

    if (!FindByAid(aid) && !FindRemovedPart(aid)) {
      const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64;
      const bool exists = Exists(ctx, base, aid_va, aid);
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

  __imp__sub_8254FAC0(ctx, base);
}

REX_HOOK_RAW(sub_824FAF08) {
  const uint32_t aid_va = ctx.r3.u32;
  const uint32_t aid = aid_va ? Load32(base, aid_va) : 0;
  if (aid && uint8_t(aid >> 24) == kTypeObjparams) {
    Initialize(base);
    const KnownCustomPart *known = FindRemovedPart(aid);

    if (known && (!known->base_aid || !KnownBaseWord(aid))) {
      ctx.r3.u64 = 0;
      return;
    }
  }

  __imp__sub_824FAF08(ctx, base);
}

REX_HOOK_RAW(sub_8260AEE8) {
  const uint32_t part = ctx.r5.u32;
  const uint32_t model = part ? Load32(base, part + kPartModel) : 0;

  if (model && std::find(g_own_models.begin(), g_own_models.end(), model) != g_own_models.end()) {
    const uint32_t instance = Load32(base, part + kPartInstance);
    const uint32_t data = instance ? Load32(base, instance + kInstanceDrawData) : 0;

    if (data) {
      std::memcpy(base + data + kDrawDataC28, base + part + kPartPaint, 16);
      const float slot = float(renut::gpu::InstanceShapeSlot(part, instance));
      uint32_t bits;
      std::memcpy(&bits, &slot, 4);
      Store32(base, data + kDrawDataC28 + 12, bits);
    }
  }

  __imp__sub_8260AEE8(ctx, base);
}

REX_HOOK_RAW(sub_8260EFC8) {
  const uint32_t part = ctx.r3.u32;
  const uint32_t record = ctx.r4.u32;
  if (part && record && !Load32(base, part + kPartCollision)) {
    const GuestPart *gp = FindByAid(Load32(base, record + kRecordAid));
    if (gp && gp->part->own_record) {
      if (const uint32_t box = BoxCollision(base))
        Store32(base, part + kPartCollision, box);
    }
  }

  __imp__sub_8260EFC8(ctx, base);
}


REX_HOOK_RAW(sub_82204DF0) {
  if (OwnSize(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }

  __imp__sub_82204DF0(ctx, base);
}

REX_HOOK_RAW(sub_8228F0F0) {
  if (const uint32_t size = OwnSize(ctx.r3.u32)) {
    ctx.r3.u64 = size;
    return;
  }
  __imp__sub_8228F0F0(ctx, base);
}

REX_HOOK_RAW(sub_824FA3D8) {
  __imp__sub_824FA3D8(ctx, base);

  if ((renut::mods::CustomParts().empty() && renut::game::RestoredParts().empty()) ||
      !Load32(base, kBlockClassGroupings))
    return;
  const uint64_t result = ctx.r3.u64;
  Initialize(base);
  PrepareSizes(ctx, base);

  if (g_blockset_aid) {
    ctx.r3.u64 = g_blockset_aid_va;
    ctx.r4.u64 = 0;
    __imp__sub_824FA620(ctx, base);
  }

  ctx.r3.u64 = result;
}

REX_HOOK_RAW(sub_82363A50) {
  const uint32_t key_va = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t out_size = ctx.r6.u32;

  if (!key_va || !out || !out_size ||
      (renut::mods::CustomParts().empty() && !renut::game::custom_npcs::HasTexts())) {
    __imp__sub_82363A50(ctx, base);
    return;
  }

  char key[kMaxTextKey];
  size_t length = 0;
  while (length + 1 < kMaxTextKey && base[key_va + length])
  key[length] = char(base[key_va + length]), ++length;
  key[length] = 0;
  std::u16string npc_text;
  const std::u16string *name = nullptr;

  if (renut::game::custom_npcs::FindText(key, &npc_text))
  name = &npc_text;
  const char *own = name || renut::mods::CustomParts().empty() ? nullptr : std::strstr(key, "__renutpart_");

  if (!own && !name && !renut::mods::CustomParts().empty())
    own = std::strstr(key, "__renuttype_");
    
  if (own) {
    own += 2;
    Initialize(base);
    for (const GuestPart &gp : g_parts)

      if (gp.text_key == own && !gp.name.empty())
        name = &gp.name;
    for (const auto &[type_key, type_name] : g_type_names)

      if (type_key == own)
        name = &type_name;
  }

  if (!name) {
    __imp__sub_82363A50(ctx, base);
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
