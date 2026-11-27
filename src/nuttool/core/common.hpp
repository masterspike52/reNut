// Shared helpers: byte-order aware readers/writers, file I/O, errors.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nut {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& msg);

// On Windows, returns an extended-length ("\\?\") form of long paths; otherwise `p` unchanged.
fs::path longPath(const fs::path& p);

// The workspace a mod's edits go to: files under `game` are written under `work` (the same relative paths) and read
// from there once they're there, so the game's files are never changed. Empty paths turn it off.
void setWorkspace(const fs::path& game, const fs::path& work);
// Where a file under the game folder is read from (its workspace copy when there is one) and written to.
fs::path readPath(const fs::path& p);
fs::path writePath(const fs::path& p);

Bytes readFile(const fs::path& p);
void writeFile(const fs::path& p, const void* data, size_t size);
inline void writeFile(const fs::path& p, const Bytes& b) { writeFile(p, b.data(), b.size()); }
void writeText(const fs::path& p, std::string_view text);
std::string readText(const fs::path& p);

// ---- raw endian access -------------------------------------------------
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[1] << 8 | p[0]); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
inline float bef32(const uint8_t* p) { uint32_t v = be32(p); float f; std::memcpy(&f, &v, 4); return f; }

inline void putBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void putBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void putLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void putLe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

// Bounds-checked cursor over a byte span. Endianness is chosen per reader.
class Reader {
public:
    Reader(const uint8_t* data, size_t size, bool bigEndian = true) : d_(data), n_(size), be_(bigEndian) {}
    explicit Reader(const Bytes& b, bool bigEndian = true) : Reader(b.data(), b.size(), bigEndian) {}

    size_t pos() const { return p_; }
    size_t size() const { return n_; }
    size_t remaining() const { return n_ - p_; }
    const uint8_t* data() const { return d_; }
    const uint8_t* cur() const { return d_ + p_; }
    void seek(size_t p) { if (p > n_) fail("seek past end"); p_ = p; }
    void skip(size_t k) { need(k); p_ += k; }
    void need(size_t k) const { if (k > n_ - p_) fail("unexpected end of data at offset " + std::to_string(p_)); }

    uint8_t u8() { need(1); return d_[p_++]; }
    uint16_t u16() { need(2); uint16_t v = be_ ? be16(d_ + p_) : le16(d_ + p_); p_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = be_ ? be32(d_ + p_) : le32(d_ + p_); p_ += 4; return v; }
    int32_t s32() { return int32_t(u32()); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    void bytes(void* out, size_t k) { need(k); std::memcpy(out, d_ + p_, k); p_ += k; }
    Bytes bytes(size_t k) { need(k); Bytes b(d_ + p_, d_ + p_ + k); p_ += k; return b; }
    std::string cstr() {
        size_t e = p_;
        while (e < n_ && d_[e]) ++e;
        if (e >= n_) fail("unterminated string");
        std::string s(reinterpret_cast<const char*>(d_ + p_), e - p_);
        p_ = e + 1;
        return s;
    }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
    bool be_;
};

class Writer {
public:
    explicit Writer(bool bigEndian = true) : be_(bigEndian) {}
    Bytes& buf() { return b_; }
    const Bytes& buf() const { return b_; }
    size_t pos() const { return b_.size(); }

    void u8(uint8_t v) { b_.push_back(v); }
    void u16(uint16_t v) { uint8_t t[2]; be_ ? putBe16(t, v) : putLe16(t, v); b_.insert(b_.end(), t, t + 2); }
    void u32(uint32_t v) { uint8_t t[4]; be_ ? putBe32(t, v) : putLe32(t, v); b_.insert(b_.end(), t, t + 4); }
    void f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void bytes(const void* p, size_t k) { auto c = static_cast<const uint8_t*>(p); b_.insert(b_.end(), c, c + k); }
    void bytes(const Bytes& b) { b_.insert(b_.end(), b.begin(), b.end()); }
    void cstr(std::string_view s) { bytes(s.data(), s.size()); u8(0); }
    void zeros(size_t k) { b_.insert(b_.end(), k, 0); }
    void align(size_t a) { while (b_.size() % a) b_.push_back(0); }
    void patch32(size_t at, uint32_t v) { be_ ? putBe32(&b_[at], v) : putLe32(&b_[at], v); }

private:
    Bytes b_;
    bool be_;
};

// ---- misc --------------------------------------------------------------
std::string hex32(uint32_t v);
std::string hex8(uint8_t v);
uint32_t parseHex(std::string_view s);
std::string lower(std::string s);
bool startsWith(std::string_view s, std::string_view p);
bool endsWith(std::string_view s, std::string_view p);
std::vector<std::string> split(std::string_view s, char sep);
std::string trim(std::string_view s);

// Standard reflected CRC-32 (poly 0xEDB88320) without the final inversion.
uint32_t crc32Raw(const void* data, size_t len, uint32_t seed = 0xFFFFFFFF);
uint32_t crc32(const void* data, size_t len);

// UTF-8 <-> UTF-16 conversion (UTF-16 is host-order code units).
std::string utf16ToUtf8(const std::u16string& s);
std::u16string utf8ToUtf16(std::string_view s);

// JSON string escaping for manifests.
std::string jsonEscape(std::string_view s);

} // namespace nut
