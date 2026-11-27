// Localised string tables (asset type 0x11). A loctext asset is a CAFF with one .data
// symbol holding a small big-endian prefix followed by an "LSBL" table. LSBL itself is
// little-endian:
//   "LSBL" u32 headerSize(0x1C) u32 version(4) u32 stringsOffset u32 namesOffset
//          u32 notesOffset u32 orderOffset            (0 = block absent)
//   strings: u32 blockSize, u32 count, (count+1) x {u16 key, u32 charOffset} sorted by key
//            (last entry is a 0xFFFF sentinel), then UTF-16LE text and one extra u16 0
//   names:   u32 blockSize, u32 count, count x {u16 key, u16 nameIndex, u32 nameOffset} sorted
//            by key, then the ASCII names in nameIndex order (names are strcmp-sorted)
//   notes:   optional developer comments; kept verbatim
//   order:   optional; u32 blockSize, u32 count, count x u16 key in authoring order
// Keys are a 16-bit hash of the name: h = h * 33 + c (h starts at 0), truncated.
#pragma once

#include "caff.hpp"
#include "common.hpp"
#include "json.hpp"

namespace nut::loctext {

struct Entry {
    uint16_t key = 0;
    std::string name; // may be empty if a table has no names block
    std::string text; // UTF-8
};

struct Table {
    Bytes prefix;               // bytes in front of LSBL inside the symbol
    std::vector<Entry> entries; // sorted by name
    bool hasOrder = false;
    std::vector<uint16_t> order; // authoring order (keys), when hasOrder
    Bytes notes;                 // raw notes block, may be empty
};

uint16_t keyHash(std::string_view name);
bool isLoctext(const caff::Caff& c);
Table read(const caff::Caff& c);
// Writes the table back into the CAFF's string symbol (resizing it as needed).
void write(caff::Caff& c, const Table& t);

// Strings are listed in authoring order when the table has one, otherwise by name.
Json toJson(const Table& t);
// Updates `t` from JSON: entries are matched by name; new names are added (and appended to
// the authoring order); entries missing from the JSON are removed only when `prune` is true.
void applyJson(Table& t, const Json& j, bool prune);

} // namespace nut::loctext
