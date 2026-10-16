#pragma once
// Just enough JSON to read an .nbpatch's patch.json (System.Text.Json output:
// \uXXXX escapes for non-ASCII, numbers as plain integers).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace renut::mods {

// Object member. A struct, not std::pair: libstdc++'s pair needs JsonValue
// complete inside JsonValue's own definition.
struct JsonMember;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<JsonMember> object;

    // Object member, or nullptr.
    const JsonValue *Find(std::string_view key) const;

    std::string String(std::string_view key, std::string fallback = {}) const;
    int64_t Int(std::string_view key, int64_t fallback = 0) const;
    // The member's array, or an empty one.
    const std::vector<JsonValue> &Array(std::string_view key) const;
};

struct JsonMember {
    std::string name;
    JsonValue value;
};

// nullopt (with *error set) when the text is not valid JSON.
std::optional<JsonValue> ParseJson(std::string_view text, std::string *error);

} // namespace renut::mods
