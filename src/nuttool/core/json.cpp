#include "json.hpp"

#include <cmath>
#include <cstdio>

namespace nut {

static const Json kNull;

bool Json::asBool() const { if (type_ != Bool) fail("JSON: expected bool"); return bool_; }
double Json::asNumber() const { if (type_ != Number) fail("JSON: expected number"); return num_; }
uint32_t Json::asU32() const {
    if (type_ == String) return parseHex(str_); // hex strings like "0x1100216a" are accepted
    double v = asNumber();
    if (v < 0 || v > 4294967295.0) fail("JSON: number out of u32 range");
    return uint32_t(v);
}
int64_t Json::asI64() const { return int64_t(asNumber()); }
const std::string& Json::asString() const { if (type_ != String) fail("JSON: expected string"); return str_; }
const std::vector<Json>& Json::items() const { if (type_ != Array) fail("JSON: expected array"); return arr_; }
std::vector<Json>& Json::items() { if (type_ != Array) fail("JSON: expected array"); return arr_; }

bool Json::has(const std::string& key) const {
    if (type_ != Object) return false;
    for (auto& [k, v] : obj_) if (k == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Object) fail("JSON: expected object when reading '" + key + "'");
    for (auto& [k, v] : obj_) if (k == key) return v;
    return kNull;
}

Json& Json::operator[](const std::string& key) {
    if (type_ == Null) type_ = Object;
    if (type_ != Object) fail("JSON: expected object when writing '" + key + "'");
    for (auto& [k, v] : obj_) if (k == key) return v;
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

size_t Json::size() const {
    if (type_ == Array) return arr_.size();
    if (type_ == Object) return obj_.size();
    return 0;
}

void Json::push(Json v) {
    if (type_ == Null) type_ = Array;
    if (type_ != Array) fail("JSON: push on non-array");
    arr_.push_back(std::move(v));
}

const std::vector<std::pair<std::string, Json>>& Json::members() const {
    if (type_ != Object) fail("JSON: expected object");
    return obj_;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    out += '\n';
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(size_t(d * indent), ' ');
    };
    switch (type_) {
        case Null: out += "null"; break;
        case Bool: out += bool_ ? "true" : "false"; break;
        case Number: {
            char t[32];
            if (std::floor(num_) == num_ && std::fabs(num_) < 1e15) std::snprintf(t, sizeof t, "%.0f", num_);
            else std::snprintf(t, sizeof t, "%.9g", num_);
            out += t;
            break;
        }
        case String: out += '"'; out += jsonEscape(str_); out += '"'; break;
        case Array: {
            // Arrays of plain numbers are written on one line to keep large manifests compact.
            bool flat = true;
            for (auto& v : arr_) if (v.type_ == Array || v.type_ == Object) { flat = false; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += flat ? ", " : ",";
                if (!flat) nl(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            if (!flat && !arr_.empty()) nl(depth);
            out += ']';
            break;
        }
        case Object: {
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                out += '"'; out += jsonEscape(obj_[i].first); out += "\": ";
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            if (!obj_.empty()) nl(depth);
            out += '}';
            break;
        }
    }
}

namespace {

struct Parser {
    std::string_view s;
    size_t p = 0;

    [[noreturn]] void err(const char* what) { fail(std::string("JSON parse error: ") + what + " at offset " + std::to_string(p)); }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p; }
    bool lit(std::string_view w) { if (s.substr(p, w.size()) == w) { p += w.size(); return true; } return false; }

    Json value() {
        ws();
        if (p >= s.size()) err("unexpected end");
        char c = s[p];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Json(string());
        if (lit("true")) return Json(true);
        if (lit("false")) return Json(false);
        if (lit("null")) return Json();
        return number();
    }

    Json object() {
        Json o = Json::object();
        ++p;
        ws();
        if (p < s.size() && s[p] == '}') { ++p; return o; }
        for (;;) {
            ws();
            if (p >= s.size() || s[p] != '"') err("expected key");
            std::string k = string();
            ws();
            if (p >= s.size() || s[p] != ':') err("expected ':'");
            ++p;
            o[k] = value();
            ws();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return o; }
            err("expected ',' or '}'");
        }
    }

    Json array() {
        Json a = Json::array();
        ++p;
        ws();
        if (p < s.size() && s[p] == ']') { ++p; return a; }
        for (;;) {
            a.push(value());
            ws();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return a; }
            err("expected ',' or ']'");
        }
    }

    unsigned hex4() {
        if (p + 4 > s.size()) err("bad \\u escape");
        unsigned v = parseHex(s.substr(p, 4));
        p += 4;
        return v;
    }

    std::string string() {
        ++p;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) err("bad escape");
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00);
                    std::u16string tmp;
                    if (cp >= 0x10000) { tmp += char16_t(0xD800 + ((cp - 0x10000) >> 10)); tmp += char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
                    else tmp += char16_t(cp);
                    out += utf16ToUtf8(tmp);
                    break;
                }
                default: out += e;
            }
        }
        if (p >= s.size()) err("unterminated string");
        ++p;
        return out;
    }

    Json number() {
        size_t b = p;
        if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
        while (p < s.size() && ((s[p] >= '0' && s[p] <= '9') || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '-' || s[p] == '+')) ++p;
        if (b == p) err("unexpected character");
        return Json(std::stod(std::string(s.substr(b, p - b))));
    }
};

} // namespace

Json Json::parse(std::string_view text) {
    Parser ps{text};
    Json v = ps.value();
    ps.ws();
    if (ps.p != text.size()) ps.err("trailing characters");
    return v;
}

} // namespace nut
