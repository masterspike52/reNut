// Loading/saving of the game's on-disk asset files regardless of wrapping.
#pragma once

#include "caff.hpp"
#include "common.hpp"
#include "streambundle.hpp"

#include <optional>

namespace nut {

enum class FileKind { Caff, StreamBundle, Font, Asf, WaveBank, Ini, Bmp, DbIndex, Unknown };

struct Loaded {
    FileKind kind = FileKind::Unknown;
    bool xctd = false; // file was XCTD-compressed on disk
    Bytes raw;         // decompressed bytes
    std::optional<caff::Caff> caff;
    std::optional<streambundle::StreamBundle> sb;
};

FileKind detect(const Bytes& data, const fs::path& path);
const char* kindName(FileKind k);
Loaded loadAsset(const fs::path& p);
// Writes a CAFF. By default the file is plain: the game's XCTD layer only decompresses files
// that start with the 0x0FF512ED magic and reads everything else directly (loose loctext files
// already rely on this). `xctd` wraps the output in an uncompressed XCTD container instead.
void saveCaff(const fs::path& p, const caff::Caff& c, bool xctd = false);

// Output path handling shared by commands that modify a file: an explicit output wins,
// otherwise the input is overwritten after a one-time "<input>.bak" backup.
fs::path prepareOutput(const fs::path& input, const std::string& explicitOut);

} // namespace nut
