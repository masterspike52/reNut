#include "common.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>

namespace nut {

void fail(const std::string& msg) { throw Error(msg); }

fs::path longPath(const fs::path& p) {
#ifdef _WIN32
    // Asset names are long; extended-length paths lift Windows' 260-character limit.
    std::wstring s = fs::absolute(p).wstring();
    if (s.size() < 240 || s.rfind(L"\\\\?\\", 0) == 0) return p;
    for (auto& c : s) if (c == L'/') c = L'\\';
    return fs::path(L"\\\\?\\" + s);
#else
    return p;  // no such limit
#endif
}

namespace {

std::mutex g_workspaceMutex;
fs::path g_game, g_work;

std::string folded(const fs::path& p) {
    std::string s = lower(fs::absolute(p).lexically_normal().generic_string());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// `p` relative to the game folder, or empty when it isn't under it (or there's no workspace).
fs::path underGame(const fs::path& p, fs::path* work) {
    std::lock_guard lock(g_workspaceMutex);
    if (g_game.empty() || g_work.empty()) return {};
    const std::string game = folded(g_game), path = folded(p);
    if (path.size() <= game.size() || path.compare(0, game.size(), game) != 0 || path[game.size()] != '/') return {};
    *work = g_work;
    return fs::absolute(p).lexically_normal().lexically_relative(fs::absolute(g_game).lexically_normal());
}

} // namespace

void setWorkspace(const fs::path& game, const fs::path& work) {
    std::lock_guard lock(g_workspaceMutex);
    g_game = game;
    g_work = work;
}

fs::path readPath(const fs::path& p) {
    fs::path work;
    const fs::path rel = underGame(p, &work);
    if (rel.empty()) return p;
    std::error_code ec;
    return fs::is_regular_file(longPath(work / rel), ec) ? work / rel : p;
}

fs::path writePath(const fs::path& p) {
    fs::path work;
    const fs::path rel = underGame(p, &work);
    return rel.empty() ? p : work / rel;
}

Bytes readFile(const fs::path& path) {
    fs::path p = longPath(readPath(path));
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot open " + p.string());
    f.seekg(0, std::ios::end);
    auto n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    Bytes b(n);
    if (n && !f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n))) fail("cannot read " + path.string());
    return b;
}

void writeFile(const fs::path& path, const void* data, size_t size) {
    fs::path p = longPath(writePath(path));
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) fail("cannot create " + path.string());
    f.write(static_cast<const char*>(data), std::streamsize(size));
    if (!f) fail("cannot write " + path.string());
}

void writeText(const fs::path& p, std::string_view text) { writeFile(p, text.data(), text.size()); }

std::string readText(const fs::path& p) {
    Bytes b = readFile(p);
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF) s.erase(0, 3);
    return s;
}

std::string hex32(uint32_t v) { char t[9]; std::snprintf(t, sizeof t, "%08x", v); return t; }
std::string hex8(uint8_t v) { char t[3]; std::snprintf(t, sizeof t, "%02x", v); return t; }

uint32_t parseHex(std::string_view s) {
    if (startsWith(s, "0x") || startsWith(s, "0X")) s.remove_prefix(2);
    if (s.empty() || s.size() > 8) fail("bad hex value '" + std::string(s) + "'");
    uint32_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else fail("bad hex value '" + std::string(s) + "'");
        v = v << 4 | uint32_t(d);
    }
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) { out.emplace_back(s.substr(b, i - b)); b = i + 1; }
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

static const uint32_t* crcTable() {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        init = true;
    }
    return t;
}

uint32_t crc32Raw(const void* data, size_t len, uint32_t seed) {
    const uint32_t* t = crcTable();
    auto p = static_cast<const uint8_t*>(data);
    uint32_t c = seed;
    for (size_t i = 0; i < len; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t crc32(const void* data, size_t len) { return ~crc32Raw(data, len); }

std::string utf16ToUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::u16string utf8ToUtf16(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (extra) c &= (0x3F >> extra);
        if (i + size_t(extra) >= s.size()) fail("truncated UTF-8 sequence");
        for (int k = 1; k <= extra; ++k) c = c << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += size_t(extra) + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}

std::string jsonEscape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char ch : s) {
        auto c = uint8_t(ch);
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char t[8]; std::snprintf(t, sizeof t, "\\u%04x", c); o += t; }
                else o += ch;
        }
    }
    return o;
}

} // namespace nut
