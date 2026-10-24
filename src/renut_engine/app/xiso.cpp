#include "renut_engine/app/xiso.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace renut::xiso {
namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Where the game partition starts: plain xiso, XGD2, XGD3, XGD1.
constexpr uint64_t kPartitions[] = {0, 0xFD90000, 0x2080000, 0x18300000};

struct Entry {
    fs::path path;  // relative to the output folder
    uint64_t offset = 0;
    uint64_t size = 0;
    bool directory = false;
};

uint16_t Le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

class Reader {
public:
    std::ifstream file;
    uint64_t partition = 0;

    bool Read(uint64_t offset, void *dst, size_t size) {
        file.clear();
        file.seekg(std::streamoff(partition + offset));
        file.read(static_cast<char *>(dst), std::streamsize(size));
        return size_t(file.gcount()) == size;
    }

    // A directory's table is a binary tree; walk it in full.
    bool ListDirectory(uint64_t sector, uint64_t size, const fs::path &parent,
                       std::vector<Entry> &entries, int depth) {
        if (size == 0 || sector == 0)
            return true;  // empty directory
        if (depth > 64 || size > 64 * 1024 * 1024)
            return false;
        std::vector<uint8_t> table(size);
        if (!Read(sector * kSector, table.data(), table.size()))
            return false;
        std::vector<uint32_t> pending{0};
        std::vector<bool> seen(size / 4 + 1, false);
        while (!pending.empty()) {
            const uint32_t at = pending.back();
            pending.pop_back();
            if (at + 14 > size || seen[at / 4])
                continue;
            seen[at / 4] = true;
            const uint8_t *e = table.data() + at;
            const uint16_t left = Le16(e), right = Le16(e + 2);
            if (left == 0xFFFF && right == 0xFFFF && Le32(e + 4) == 0xFFFFFFFF)
                continue;  // padding
            const uint8_t name_length = e[13];
            if (at + 14 + name_length > size || name_length == 0)
                continue;
            std::string name(reinterpret_cast<const char *>(e + 14), name_length);
            if (name == "." || name == ".." || name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos)
                return false;  // a malformed (or hostile) image
            Entry entry;
            entry.path = parent / name;
            entry.offset = uint64_t(Le32(e + 4)) * kSector;
            entry.size = Le32(e + 8);
            entry.directory = (e[12] & 0x10) != 0;
            entries.push_back(entry);
            if (entry.directory &&
                !ListDirectory(Le32(e + 4), entry.size, entry.path, entries, depth + 1))
                return false;
            if (left && left != 0xFFFF)
                pending.push_back(uint32_t(left) * 4);
            if (right && right != 0xFFFF)
                pending.push_back(uint32_t(right) * 4);
        }
        return true;
    }
};

bool Fail(Progress &progress, std::string error) {
    std::lock_guard lock(progress.mutex);
    progress.error = std::move(error);
    return false;
}

} // namespace

bool Extract(const fs::path &iso, const fs::path &out, Progress &progress) {
    progress.done = 0;
    progress.total = 0;
    {
        std::lock_guard lock(progress.mutex);
        progress.error.clear();
        progress.current.clear();
    }
    Reader reader;
    reader.file.open(iso, std::ios::binary);
    if (!reader.file)
        return Fail(progress, "can't open " + iso.filename().string());

    uint8_t volume[0x800];
    bool found = false;
    for (uint64_t partition : kPartitions) {
        reader.partition = partition;
        if (reader.Read(32 * kSector, volume, sizeof(volume)) &&
            std::memcmp(volume, kMagic, 20) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return Fail(progress, "not an Xbox 360 disc image");

    std::vector<Entry> entries;
    if (!reader.ListDirectory(Le32(volume + 20), Le32(volume + 24), {}, entries, 0))
        return Fail(progress, "damaged directory table");
    uint64_t total = 0;
    for (const auto &e : entries)
        if (!e.directory)
            total += e.size;
    progress.total = total;

    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<char> buffer(4 << 20);
    for (const auto &e : entries) {
        if (progress.cancel)
            return Fail(progress, "cancelled");
        const fs::path target = out / e.path;
        if (e.directory) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        {
            std::lock_guard lock(progress.mutex);
            progress.current = e.path.generic_string();
        }
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file)
            return Fail(progress, "can't write " + target.string());
        for (uint64_t at = 0; at < e.size;) {
            if (progress.cancel)
                return Fail(progress, "cancelled");
            const size_t chunk = size_t(std::min<uint64_t>(buffer.size(), e.size - at));
            if (!reader.Read(e.offset + at, buffer.data(), chunk))
                return Fail(progress, "read error in " + e.path.generic_string());
            file.write(buffer.data(), std::streamsize(chunk));
            if (!file)
                return Fail(progress, "write error (disk full?) in " + e.path.generic_string());
            at += chunk;
            progress.done += chunk;
        }
    }
    return true;
}

} // namespace renut::xiso

#ifdef RENUT_XISO_SELFTEST
// Host check: c++ -std=c++20 -DRENUT_XISO_SELFTEST -I src src/renut_engine/app/xiso.cpp
//   ./a.out game.iso outdir   (then diff -r against extract-xiso's output)
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    renut::xiso::Progress p;
    const bool ok = renut::xiso::Extract(argv[1], argv[2], p);
    std::printf("%s %llu/%llu bytes %s\n", ok ? "ok" : "FAILED", (unsigned long long)p.done.load(),
                (unsigned long long)p.total.load(), p.error.c_str());
    return ok ? 0 : 1;
}
#endif
