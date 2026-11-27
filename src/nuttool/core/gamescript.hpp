// Game scripts: scene scripts (aid_script_), actor goal lists (aid_actorgoals_) and the tables that
// point at them (aid_scripttable_, aid_statetable_). All are flat big-endian records without pointers.
//
//   script       { u32 size, u32 opcode, args }...  ends with opcode 0
//   actorgoals   { u16 size, u16 flags, char goal[64], args }...
//   scripttable  { char name[64], u32 asset id }...
//   statetable   { char state[64], u32 asset id, u32 asset id, char name[64] }...
//
// The argument layouts of the opcodes and goals were read off every script in the game.
#pragma once

#include "common.hpp"

#include <functional>
#include <optional>

namespace nut::gamescript {

enum class Kind { Script, ActorGoals, ScriptTable, StateTable };

// From an asset name's aid_<type>_ prefix, or from an asset id's type byte.
std::optional<Kind> kindOf(std::string_view assetName);
std::optional<Kind> kindOfId(uint32_t id);
const char* kindLabel(Kind k);

// Name of an asset id, "" when unknown.
using NameLookup = std::function<std::string(uint32_t)>;

// Editable text, one record per line: `op_60 "Title" @aid_challenge_x 1`. Throws on malformed data.
std::string toText(Kind k, const Bytes& data, const NameLookup& names = {});
// Throws Error("line N: ...") on mistakes. Opcodes and goals with a known layout must keep their argument size.
Bytes fromText(Kind k, std::string_view text);

// Ids of the named assets that `data` points at (aligned words with a known name), in order, without repeats.
std::vector<uint32_t> referencedIds(const Bytes& data, const NameLookup& names, uint32_t self = 0);

// The value syntax of the text, shared with other editable text (objparams).
namespace text {

struct Token {
    enum Type { Word, String, Raw, Head, Flags } type = Word;
    uint32_t word = 0;
    uint32_t size = 0; // explicit string size; 0 takes it from the layout
    std::string text;
    Bytes raw;
};

// Tokens of one line, up to a # comment. Throws Error("line N: ...").
std::vector<Token> parseLine(std::string_view line, int lineNo);
// A word as 'i' int, 'f' float, 'a' asset id or 'x' hex (asset ids with a known name print as @name).
std::string formatWord(char type, uint32_t v, const NameLookup& names);
// A fixed-size string field; one with bytes after its terminator prints as <raw bytes>.
std::string formatString(const uint8_t* p, uint32_t n, bool withSize);
std::string formatRaw(const uint8_t* p, size_t n);

} // namespace text

} // namespace nut::gamescript
