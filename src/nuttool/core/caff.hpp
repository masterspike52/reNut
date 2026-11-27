// Rare "CAFF" container (version 07.08.06.0036), reversed from default.xex.
//
// File layout (big-endian):
//   header   0x78 bytes
//   info     "group 0": section table, section names, symbol names, symbols
//   fixups   "group 1": endian-swap tables, relocation tables, string pools
//   sections data of each section, back to back, in table order
//
// Header fields:
//   +0x00 "CAFF"  +0x04 "07.08.06.0036"
//   +0x14 header size (0x78)   +0x18 checksum (ELF-style hash of header with this field zeroed)
//   +0x1C symbol name count    +0x20 symbol count
//   +0x24 swap set A count     +0x28 swap set A list length
//   +0x2C reloc set A count    +0x30 reloc set A list length
//   +0x34 swap set B count     +0x38 swap set B list length
//   +0x3C reloc set B count    +0x40 reloc set B list length
//   +0x48 u8 flags (bit0: big-endian/native)  +0x49 u8 section count
//   +0x4A u8 compressed        +0x4B u8 string pool count
//   +0x4C section name bytes
//   +0x50 group 0 {size, rt, rt, rt, storedSize}  +0x64 group 1 {same}
//
// Section record (33 bytes, packed): u32 nameOffset, u8 alignLog2, u32 firstSymbol,
//   u32 size, 4 x u32 runtime, u32 storedSize. "firstSymbol" is in fact the 1-based index of the
//   section's string pool symbol, 0 for none (the game's loader, 0x82A01D30, reads it so).
// Symbol record (14 bytes): u32 nameIndex (1-based), u32 offset, u32 size, u8 section (1-based), u8 kind.
// Relocation: {u32 fromSymbol, u32 toSymbol, u32 count} + `count` offsets into fromSymbol.
//   At load, each u32 at fromSymbol+offset that is < size(toSymbol) gets address(toSymbol) added,
//   so pointers are stored relative to the start of the target symbol.
#pragma once

#include "common.hpp"
#include "json.hpp"

#include <optional>

namespace nut::caff {

struct Section {
    std::string name;
    uint8_t alignLog2 = 0;
    uint32_t firstSymbol = 0;
    Bytes data;
};

struct Symbol {
    uint32_t nameIndex = 0; // 1-based into symbolNames, 0 = unnamed
    uint32_t offset = 0;
    uint32_t size = 0;
    uint8_t section = 0; // 1-based
    uint8_t kind = 0;
};

struct Relocation {
    uint32_t from = 0, to = 0;
    std::vector<uint32_t> offsets;
};

struct SwapEntry {
    uint32_t symbol = 0, elementSize = 0, elementCount = 0, stride = 0;
    std::vector<uint32_t> offsets;
};

struct PoolString {
    uint32_t length = 0, offset = 0, hash = 0, flags = 0;
};

struct Pool {
    uint32_t symbol = 0;
    std::vector<PoolString> strings;
};

struct Caff {
    Bytes header; // raw 0x78-byte header; derived fields are rewritten on save
    std::vector<Section> sections;
    std::vector<std::string> symbolNames;
    Bytes extra; // opaque block between symbol names and symbols (normally empty)
    std::vector<Symbol> symbols;
    std::vector<SwapEntry> swaps[2];
    std::vector<Relocation> relocs[2];
    std::vector<Pool> pools;

    static Caff parse(const Bytes& file);
    // Header, symbol tables and only the sections named in `keep` (the others stay empty), from
    // the first bytes of a file. Returns nothing and sets `need` when `prefix` is too short.
    static std::optional<Caff> parsePartial(const Bytes& prefix, const std::vector<std::string>& keep, size_t& need);
    Bytes serialize() const;

    std::string symbolName(size_t symIndex) const; // 0-based symbol index
    const Section& sectionOf(const Symbol& s) const;
    Bytes symbolData(size_t symIndex) const;
    int findSection(std::string_view name) const;

    // Replace a symbol's bytes. Later symbols in the same section shift by the size
    // delta rounded up to the section alignment, so their alignment is preserved.
    void replaceSymbolData(size_t symIndex, const Bytes& data);
    // A new symbol at the end of a section (1-based `section`, aligned to it); returns its index.
    // Symbols may be listed in any order, so nothing else is renumbered.
    size_t appendSymbol(uint32_t nameIndex, uint8_t section, uint8_t kind, const Bytes& data);
    // 1-based index of `name` in symbolNames, added if missing.
    uint32_t symbolNameIndex(const std::string& name);

    Json toJson() const;              // everything except section bytes
private:
    static std::optional<Caff> parseImpl(const Bytes& file, const std::vector<std::string>* keep, size_t* need);

public:
    static Caff fromJson(const Json& j); // section bytes must be filled in by the caller
};

bool isCaff(const Bytes& data);
uint32_t headerChecksum(const uint8_t* header);

} // namespace nut::caff
