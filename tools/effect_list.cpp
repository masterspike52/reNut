// Lists every particle effect asset in the game's bundles: its asset id (as
// fx.play takes it), its type, its name and the bundles holding it. Its
// output made docs/modding/effects.md.
// build: clang++ -std=c++20 -O2 -I src tools/effect_list.cpp src/renut_engine/mods/xcompress.cpp -o effect_list.exe
// usage: effect_list <game folder> > effects.tsv
// (CAFF symbols are "aid_<type>_<name>,<timestamp>,<version>"; an id is
// type << 24 | CRC-32 without the final inversion of <name>, low 24 bits.)
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "renut_engine/mods/xcompress.h"

namespace fs = std::filesystem;

static uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

static uint32_t Crc32(const std::string &s) {
  static std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char ch : s)
    crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
  return crc;  // no final inversion: N&B's hash
}

struct Effect {
  uint8_t type;
  std::string name;
  std::set<std::string> bundles;
};

static const std::map<std::string, uint8_t> kTypes = {
    {"fxemitter", 0x1B},          {"fxparticle", 0x1C},       {"gpuparticleeffect", 0x45},
    {"blobsdropleteffect", 0x47}, {"explosioneffect", 0x48},  {"3dgpuparticleeffect", 0x4A},
    {"compositeeffect", 0x4B},
};

// The CAFF containers in 'data': their symbol names.
static void Symbols(const std::vector<uint8_t> &data, std::vector<std::string> &out) {
  for (size_t at = 0; at + 0x80 < data.size(); ++at) {
    if (data[at] != 'C' || data[at + 1] != 'A' || data[at + 2] != 'F' || data[at + 3] != 'F')
      continue;
    const uint8_t *h = data.data() + at;
    if (Be32(h + 0x14) != 0x78)
      continue;
    const uint32_t symbols = Be32(h + 0x1C);
    const uint32_t sections = h[0x49];
    const uint32_t section_names = Be32(h + 0x4C);
    size_t p = at + 0x78 + size_t(sections) * 0x21 + section_names;
    if (p + 4 > data.size())
      continue;
    const uint32_t buffer = Be32(data.data() + p);
    p += 4;
    const size_t offsets = p;
    const size_t strings = offsets + size_t(symbols) * 4;
    if (strings + buffer > data.size() || symbols > 200000)
      continue;
    for (uint32_t i = 0; i < symbols; ++i) {
      const uint32_t off = Be32(data.data() + offsets + i * 4);
      if (off >= buffer)
        continue;
      const char *s = reinterpret_cast<const char *>(data.data() + strings + off);
      out.emplace_back(s, strnlen(s, buffer - off));
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: effect_list <game folder>\n");
    return 1;
  }
  std::map<uint32_t, Effect> effects;
  for (const char *dir : {"Bundle/4f", "Bundle/50"}) {
    const fs::path root = fs::path(argv[1]) / dir;
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(root, ec)) {
      if (!entry.is_regular_file())
        continue;
      std::ifstream f(entry.path(), std::ios::binary);
      std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      std::string error;
      auto expanded = renut::mods::ExpandGameFile(std::move(raw), &error);
      if (!expanded) {
        std::fprintf(stderr, "%s: %s\n", entry.path().string().c_str(), error.c_str());
        continue;
      }
      std::vector<std::string> names;
      Symbols(*expanded, names);
      const std::string bundle = fs::relative(entry.path(), fs::path(argv[1]) / "Bundle", ec).generic_string();
      for (std::string name : names) {
        // Symbols are "aid_<type>_<name>,<timestamp>,<version>".
        if (const size_t comma = name.find(','); comma != std::string::npos)
          name.resize(comma);
        if (name.rfind("aid_", 0) != 0)
          continue;
        const size_t sep = name.find('_', 4);
        if (sep == std::string::npos)
          continue;
        auto type = kTypes.find(name.substr(4, sep - 4));
        if (type == kTypes.end())
          continue;
        const std::string stem = name.substr(sep + 1);
        const uint32_t aid = uint32_t(type->second) << 24 | (Crc32(stem) & 0xFFFFFFu);
        Effect &e = effects[aid];
        e.type = type->second;
        e.name = name;
        e.bundles.insert(bundle);
      }
    }
  }
  std::printf("id\ttype\tname\tbundles\n");
  for (const auto &[aid, e] : effects) {
    std::string bundles;
    for (const std::string &b : e.bundles)
      bundles += (bundles.empty() ? "" : " ") + b;
    std::printf("0x%08X\t%s\t%s\t%s\n", aid,
                e.type == 0x4B ? "composite" : e.type == 0x4A ? "3D GPU particle" : e.type == 0x45 ? "GPU particle"
                : e.type == 0x48 ? "explosion" : e.type == 0x47 ? "blobs droplet" : e.type == 0x1B ? "emitter"
                : "particle",
                e.name.c_str(), bundles.c_str());
  }
  std::fprintf(stderr, "%zu effects\n", effects.size());
  return 0;
}
