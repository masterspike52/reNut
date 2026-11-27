#include "nbpatch.hpp"

#include "deflate.hpp"
#include "json.hpp"
#include "xctd.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>

namespace nut::nbpatch {

namespace {

// ---- SHA-256 (FIPS 180-4) ----------------------------------------------

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

void shaBlock(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

// ---- zip (stored / deflated entries, as .NET's ZipArchive reads them) ---

class ZipWriter {
public:
    explicit ZipWriter(const fs::path& p) : f_(longPath(p), std::ios::binary | std::ios::trunc) {
        if (!f_) fail("cannot write " + p.string());
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        time_ = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
        date_ = uint16_t(std::max(0, tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    }

    void add(const std::string& name, const Bytes& data) {
        Entry e{name, 0, crc32(data.data(), data.size()), 0, uint32_t(data.size()), offset_};
        if (data.size() > 0xFFFFFFFFull || offset_ > 0xFFFFFFFFull) fail("mod package over 4 GB");
        Bytes packed;
        // The compressor keeps a 4-byte chain link per input byte; very large files are stored.
        if (data.size() >= 64 && data.size() <= (64u << 20)) {
            packed = deflate::compress(data.data(), data.size());
            if (packed.size() < data.size()) e.method = 8;
        }
        const Bytes& body = e.method == 8 ? packed : data;
        e.csize = uint32_t(body.size());
        Writer w(false);
        w.u32(0x04034b50);
        w.u16(20);
        w.u16(0x0800); // UTF-8 name
        w.u16(e.method);
        w.u16(time_);
        w.u16(date_);
        w.u32(e.crc);
        w.u32(e.csize);
        w.u32(e.size);
        w.u16(uint16_t(name.size()));
        w.u16(0);
        w.bytes(name.data(), name.size());
        put(w.buf());
        put(body);
        entries_.push_back(e);
    }

    void finish() {
        if (entries_.size() > 0xFFFF) fail("too many files for one mod package");
        uint64_t start = offset_;
        Writer w(false);
        for (const Entry& e : entries_) {
            w.u32(0x02014b50);
            w.u16(20);
            w.u16(20);
            w.u16(0x0800);
            w.u16(e.method);
            w.u16(time_);
            w.u16(date_);
            w.u32(e.crc);
            w.u32(e.csize);
            w.u32(e.size);
            w.u16(uint16_t(e.name.size()));
            w.u16(0);
            w.u16(0);
            w.u16(0);
            w.u16(0);
            w.u32(0);
            w.u32(uint32_t(e.offset));
            w.bytes(e.name.data(), e.name.size());
        }
        uint64_t size = w.pos();
        w.u32(0x06054b50);
        w.u16(0);
        w.u16(0);
        w.u16(uint16_t(entries_.size()));
        w.u16(uint16_t(entries_.size()));
        w.u32(uint32_t(size));
        w.u32(uint32_t(start));
        w.u16(0);
        if (start > 0xFFFFFFFFull) fail("mod package over 4 GB");
        put(w.buf());
        f_.close();
        if (!f_) fail("error writing the mod package");
    }

private:
    struct Entry {
        std::string name;
        uint16_t method;
        uint32_t crc, csize, size;
        uint64_t offset;
    };
    void put(const Bytes& b) {
        f_.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        if (!f_) fail("error writing the mod package");
        offset_ += b.size();
    }

    std::ofstream f_;
    std::vector<Entry> entries_;
    uint64_t offset_ = 0;
    uint16_t time_ = 0, date_ = 0;
};

// ---- helpers ------------------------------------------------------------

constexpr uint32_t kArchiveMagic = 0x438CB47C; // Bundle/50 streaming archive

void putVarint(Bytes& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v | 0x80));
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

bool sameContents(const fs::path& a, const fs::path& b) {
    std::ifstream fa(longPath(a), std::ios::binary), fb(longPath(b), std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (fa && fb) {
        fa.read(ba.data(), std::streamsize(ba.size()));
        fb.read(bb.data(), std::streamsize(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) return false;
    }
    return fa.eof() && fb.eof();
}

std::string relPath(const fs::path& p, const fs::path& root) {
    std::u8string s = fs::relative(p, root).generic_u8string();
    return std::string(s.begin(), s.end());
}

// default.xex is recompiled ahead of time in reNut, so patching it has no effect.
bool isExecutable(const fs::path& p) {
    std::string ext = lower(p.extension().string());
    return ext == ".xex" || ext == ".xexp";
}

} // namespace

std::string sha256Hex(const uint8_t* data, size_t size) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = size & ~size_t(63);
    for (size_t i = 0; i < full; i += 64) shaBlock(h, data + i);
    uint8_t tail[128] = {};
    size_t rest = size - full;
    std::memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    size_t tailLen = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - size_t(i)] = uint8_t(bits >> (i * 8));
    shaBlock(h, tail);
    if (tailLen == 128) shaBlock(h, tail + 64);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h)
        for (int s = 28; s >= 0; s -= 4) out += digits[(v >> s) & 15];
    return out;
}

Bytes expand(const Bytes& raw) {
    if (xctd::isXctd(raw)) return xctd::decompress(raw);
    if (raw.size() < 20 || be32(raw.data()) != kArchiveMagic || be32(raw.data() + 4) != 12) return raw;

    int32_t count = int32_t(be32(raw.data() + 8));
    int32_t deps = int32_t(be32(raw.data() + 16));
    if (count < 0 || deps < 0 || 20 + (uint64_t(deps) + uint64_t(count) * 3) * 4 > raw.size()) fail("streaming archive: damaged entry table");
    std::vector<std::pair<size_t, size_t>> entries;
    size_t o = 20 + size_t(deps) * 4;
    for (int32_t i = 0; i < count; ++i, o += 12) {
        int32_t off = int32_t(be32(raw.data() + o + 4));
        int32_t size = int32_t(be32(raw.data() + o + 8));
        if (size <= 0 && off <= 0) continue;
        if (off < 0 || size < 0 || uint64_t(off) + uint64_t(size) > raw.size()) fail("streaming archive: entry out of range");
        entries.emplace_back(size_t(off), size_t(size));
    }
    Bytes out = raw;
    for (auto [off, size] : entries) {
        if (size < 16 || be32(raw.data() + off) != xctd::kMagic) continue;
        Bytes data;
        try {
            data = xctd::decompress(Bytes(raw.begin() + ptrdiff_t(off), raw.begin() + ptrdiff_t(off + size)));
        } catch (const std::exception&) {
            break; // NB Studio keeps the entries before the first one it cannot decompress
        }
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

Bytes makeDelta(const Bytes& source, const Bytes& target) {
    // Greedy matching: every block of the source is indexed by a rolling hash, the target is scanned
    // a byte at a time, and a hit is grown both ways into a COPY.
    const size_t block = source.size() > (256u << 20) ? 64 : 32;
    constexpr uint32_t kMul = 0x01000193;
    uint32_t top = 1; // kMul^(block-1)
    for (size_t i = 1; i < block; ++i) top *= kMul;
    auto hashAt = [&](const uint8_t* p) {
        uint32_t h = 0;
        for (size_t i = 0; i < block; ++i) h = h * kMul + p[i];
        return h;
    };

    size_t blocks = source.size() / block;
    int bits = 10;
    while ((size_t(1) << bits) < blocks && bits < 30) ++bits;
    std::vector<uint32_t> table(size_t(1) << bits, 0); // source offset + 1, 0 = empty
    auto slot = [&](uint32_t h) { return size_t((h * 0x9E3779B1u) >> (32 - bits)); };
    for (size_t b = 0; b < blocks; ++b) {
        uint32_t& s = table[slot(hashAt(source.data() + b * block))];
        if (!s) s = uint32_t(b * block + 1);
    }

    Bytes out = {'N', 'B', 'D', 'L', 1, 0, 0, 0};
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(uint64_t(target.size()) >> (i * 8)));
    size_t literal = 0; // start of the bytes not yet emitted
    auto emitAdd = [&](size_t end) {
        if (end <= literal) return;
        out.push_back(2);
        putVarint(out, end - literal);
        out.insert(out.end(), target.begin() + ptrdiff_t(literal), target.begin() + ptrdiff_t(end));
    };

    size_t t = 0;
    uint32_t h = target.size() >= block ? hashAt(target.data()) : 0;
    while (t + block <= target.size()) {
        uint32_t cand = table[slot(h)];
        if (cand && std::memcmp(source.data() + cand - 1, target.data() + t, block) == 0) {
            size_t s = cand - 1, start = t;
            while (start > literal && s > 0 && source[s - 1] == target[start - 1]) --s, --start;
            size_t len = t - start + block;
            while (s + len < source.size() && start + len < target.size() && source[s + len] == target[start + len]) ++len;
            emitAdd(start);
            out.push_back(1);
            putVarint(out, s);
            putVarint(out, len);
            t = literal = start + len;
            if (t + block <= target.size()) h = hashAt(target.data() + t);
            continue;
        }
        if (t + block < target.size()) h = (h - target[t] * top) * kMul + target[t + block];
        ++t;
    }
    emitAdd(target.size());
    return out;
}

Bytes applyDelta(const Bytes& source, const Bytes& delta) {
    if (delta.size() < 16 || std::memcmp(delta.data(), "NBDL", 4) != 0) fail("not an NB delta");
    uint64_t length = 0;
    for (int i = 0; i < 8; ++i) length |= uint64_t(delta[8 + size_t(i)]) << (i * 8);
    if (length > 0x7FFFFFFFull) fail("delta target too large");
    size_t p = 16;
    auto varint = [&] {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= delta.size()) fail("truncated delta");
            uint8_t b = delta[p++];
            v |= uint64_t(b & 0x7F) << shift;
            if (b < 0x80) return v;
        }
        fail("bad varint in delta");
    };
    Bytes out(length);
    uint64_t o = 0;
    while (p < delta.size()) {
        uint8_t op = delta[p++];
        if (op == 1) {
            uint64_t a = varint(), n = varint();
            if (a + n > source.size() || o + n > length) fail("delta copy out of range");
            std::memcpy(out.data() + o, source.data() + a, size_t(n));
            o += n;
        } else if (op == 2) {
            uint64_t n = varint();
            if (p + n > delta.size() || o + n > length) fail("delta literal out of range");
            std::memcpy(out.data() + o, delta.data() + p, size_t(n));
            p += size_t(n);
            o += n;
        } else {
            fail("bad delta op");
        }
    }
    if (o != length) fail("delta produced a short file");
    return out;
}

std::string slug(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        c = static_cast<unsigned char>(std::tolower(c));
        if (c < 0x80 && std::isalnum(c)) out += char(c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::vector<Change> findChanges(const fs::path& gameRoot, const fs::path& originalRoot, const ops::Progress& pr) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(gameRoot, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!originalRoot.empty() && it->is_directory() && fs::equivalent(it->path(), originalRoot, ec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file()) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::vector<Change> out;
    for (size_t i = 0; i < files.size(); ++i) {
        if (pr.cancelled()) break;
        const fs::path& f = files[i];
        pr.at(double(i) / double(files.size()), relPath(f, gameRoot));
        if (originalRoot.empty()) {
            if (lower(f.extension().string()) != ".bak") continue;
            fs::path cur = f;
            cur.replace_extension();
            if (!fs::is_regular_file(cur)) continue;
            if (isExecutable(cur)) {
                pr.info("skipped " + relPath(cur, gameRoot) + ": reNut runs recompiled code, default.xex changes have no effect");
                continue;
            }
            if (fs::file_size(cur) == fs::file_size(f) && sameContents(cur, f)) continue;
            out.push_back({relPath(cur, gameRoot), f, cur});
        } else {
            if (lower(f.extension().string()) == ".bak") continue;
            std::string rel = relPath(f, gameRoot);
            fs::path orig = originalRoot / fs::relative(f, gameRoot);
            if (!fs::is_regular_file(orig)) {
                out.push_back({rel, {}, f});
                continue;
            }
            if (fs::file_size(orig) == fs::file_size(f) &&
                (fs::last_write_time(orig) == fs::last_write_time(f) || sameContents(orig, f)))
                continue;
            if (isExecutable(f)) {
                pr.info("skipped " + rel + ": reNut runs recompiled code, default.xex changes have no effect");
                continue;
            }
            out.push_back({rel, orig, f});
        }
    }
    return out;
}

void addUpdateChanges(std::vector<Change>& out, const fs::path& gameRoot, const fs::path& updateDir, const fs::path& originalUpdateDir,
                      const ops::Progress& pr) {
    std::error_code ec;
    if (updateDir.empty() || !fs::is_directory(updateDir, ec)) return;
    fs::path rel = fs::relative(fs::absolute(updateDir, ec), fs::absolute(gameRoot, ec), ec);
    if (!ec && !rel.empty() && *rel.begin() != "..") return; // inside the game folder: already listed
    for (Change c : findChanges(updateDir, originalUpdateDir, pr)) {
        c.path = "update/" + c.path;
        out.push_back(std::move(c));
    }
}

bool write(const fs::path& out, const Info& info, const std::vector<Change>& changes, const ops::Progress& pr) {
    if (trim(info.name).empty()) {
        pr.info("the mod needs a name");
        return false;
    }
    fs::path tmp = out;
    tmp += ".tmp";
    Json files = Json::array();
    std::map<std::string, bool> seen;
    try {
        ZipWriter zip(tmp);
        for (size_t i = 0; i < changes.size(); ++i) {
            const Change& c = changes[i];
            if (pr.cancelled()) {
                zip.finish();
                fs::remove(tmp);
                return false;
            }
            pr.at(double(i) / double(changes.size()), c.path);
            if (c.path.empty() || c.path.find("..") != std::string::npos || c.path.find(':') != std::string::npos || c.path[0] == '/')
                fail("bad game path '" + c.path + "'");
            if (seen[lower(c.path)]) fail(c.path + " is listed twice");
            seen[lower(c.path)] = true;

            Bytes target = readFile(c.modified);
            Json f = Json::object();
            f["Path"] = c.path;
            if (c.original.empty()) {
                std::string entry = "files/" + c.path;
                zip.add(entry, target);
                f["Kind"] = "new";
                f["TargetSha256"] = sha256Hex(target);
                f["TargetSize"] = uint64_t(target.size());
                f["Entry"] = entry;
                pr.info("  new    " + c.path + " (" + std::to_string(target.size()) + " bytes)");
            } else {
                Bytes raw = readFile(c.original);
                if (raw == target) {
                    pr.info("  same   " + c.path + " (unchanged, left out)");
                    continue;
                }
                std::string sourceSha = sha256Hex(raw);
                Bytes source = expand(raw);
                raw = {};
                Bytes delta = makeDelta(source, target);
                if (applyDelta(source, delta) != target) fail(c.path + ": the delta does not rebuild the file");
                std::string entry = "files/" + c.path + ".nbdl";
                zip.add(entry, delta);
                f["Kind"] = "delta";
                f["SourceSha256"] = sourceSha;
                f["SourceSize"] = uint64_t(fs::file_size(c.original));
                f["TargetSha256"] = sha256Hex(target);
                f["TargetSize"] = uint64_t(target.size());
                f["Entry"] = entry;
                pr.info("  delta  " + c.path + " (" + std::to_string(delta.size()) + " byte delta for " + std::to_string(target.size()) + " bytes)");
            }
            files.push(std::move(f));
        }
        if (files.size() == 0) fail("no changed files to package");

        Json manifest = Json::object();
        manifest["Format"] = 1;
        manifest["Id"] = info.id.empty() ? slug(info.name) : info.id;
        manifest["Name"] = trim(info.name);
        manifest["Version"] = info.version.empty() ? std::string("1") : info.version;
        manifest["Author"] = info.author;
        manifest["Category"] = info.category;
        manifest["Description"] = info.description;
        manifest["Tool"] = "NutTool";
        manifest["Files"] = std::move(files);
        std::string text = manifest.dump();
        zip.add("patch.json", Bytes(text.begin(), text.end()));
        zip.finish();
    } catch (const std::exception& e) {
        pr.info(std::string("mod package not written: ") + e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) {
        pr.info("cannot write " + out.string() + ": " + ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace nut::nbpatch
