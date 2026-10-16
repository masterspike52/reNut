#include "renut_engine/mods/json.h"

#include <cmath>
#include <cstdlib>

namespace renut::mods {

const JsonValue *JsonValue::Find(std::string_view key) const {
    if (type != Type::Object)
        return nullptr;
    for (const auto &[name, value] : object)
        if (name == key)
            return &value;
    return nullptr;
}

std::string JsonValue::String(std::string_view key, std::string fallback) const {
    const JsonValue *v = Find(key);
    return v && v->type == Type::String ? v->string : fallback;
}

int64_t JsonValue::Int(std::string_view key, int64_t fallback) const {
    const JsonValue *v = Find(key);
    return v && v->type == Type::Number ? int64_t(std::llround(v->number)) : fallback;
}

const std::vector<JsonValue> &JsonValue::Array(std::string_view key) const {
    static const std::vector<JsonValue> kEmpty;
    const JsonValue *v = Find(key);
    return v && v->type == Type::Array ? v->array : kEmpty;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {
        if (text_.substr(0, 3) == "\xEF\xBB\xBF")
            pos_ = 3;
    }

    bool Document(JsonValue &out) {
        if (!Value(out, 0))
            return false;
        Space();
        return pos_ == text_.size() || Fail("trailing characters");
    }

    std::string error;

private:
    bool Fail(const char *what) {
        error = std::string(what) + " at offset " + std::to_string(pos_);
        return false;
    }

    void Space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool Literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word)
            return Fail("unknown literal");
        pos_ += word.size();
        return true;
    }

    bool Value(JsonValue &out, int depth) {
        if (depth > 64)
            return Fail("nested too deeply");
        Space();
        if (pos_ >= text_.size())
            return Fail("unexpected end");
        char c = text_[pos_];
        if (c == '{') {
            out.type = JsonValue::Type::Object;
            ++pos_;
            Space();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                Space();
                std::string key;
                if (!Str(key))
                    return false;
                Space();
                if (pos_ >= text_.size() || text_[pos_] != ':')
                    return Fail("':' expected");
                ++pos_;
                JsonValue member;
                if (!Value(member, depth + 1))
                    return false;
                out.object.emplace_back(std::move(key), std::move(member));
                Space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return Fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            out.type = JsonValue::Type::Array;
            ++pos_;
            Space();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                JsonValue item;
                if (!Value(item, depth + 1))
                    return false;
                out.array.push_back(std::move(item));
                Space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return Fail("',' or ']' expected");
            }
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return Str(out.string);
        }
        if (c == 't') {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return Literal("true");
        }
        if (c == 'f') {
            out.type = JsonValue::Type::Bool;
            return Literal("false");
        }
        if (c == 'n') {
            out.type = JsonValue::Type::Null;
            return Literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = pos_;
            while (pos_ < text_.size() && std::string_view("+-0123456789.eE").find(text_[pos_]) != std::string_view::npos)
                ++pos_;
            std::string number(text_.substr(start, pos_ - start));
            char *end = nullptr;
            out.type = JsonValue::Type::Number;
            out.number = std::strtod(number.c_str(), &end);
            return end == number.c_str() + number.size() || Fail("bad number");
        }
        return Fail("unexpected character");
    }

    static void PutUtf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t &value) {
        if (pos_ + 4 > text_.size())
            return Fail("short \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            char h = text_[pos_++];
            value <<= 4;
            if (h >= '0' && h <= '9') value |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') value |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') value |= uint32_t(h - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        return true;
    }

    bool Str(std::string &out) {
        if (pos_ >= text_.size() || text_[pos_] != '"')
            return Fail("string expected");
        ++pos_;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size())
                break;
            char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && text_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    uint32_t low;
                    if (!Hex4(low))
                        return false;
                    if (low >= 0xDC00 && low < 0xE000)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                PutUtf8(out, cp);
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    std::string_view text_;
    size_t pos_ = 0;
};

} // namespace

std::optional<JsonValue> ParseJson(std::string_view text, std::string *error) {
    Parser parser(text);
    JsonValue root;
    if (!parser.Document(root)) {
        *error = parser.error;
        return std::nullopt;
    }
    return root;
}

} // namespace renut::mods
