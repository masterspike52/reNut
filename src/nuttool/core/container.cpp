#include "container.hpp"

#include "font.hpp"
#include "media.hpp"
#include "xctd.hpp"

namespace nut {

FileKind detect(const Bytes& d, const fs::path& path) {
    if (caff::isCaff(d)) return FileKind::Caff;
    if (streambundle::looksLikeStreamBundle(d)) return FileKind::StreamBundle;
    if (font::looksLikeFont(d)) return FileKind::Font;
    if (media::isAsf(d)) return FileKind::Asf;
    if (media::isWaveBank(d)) return FileKind::WaveBank;
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return FileKind::Bmp;
    std::string ext = lower(path.extension().string());
    if (ext == ".ini") return FileKind::Ini;
    if (path.filename() == "db_index.txt") return FileKind::DbIndex;
    return FileKind::Unknown;
}

const char* kindName(FileKind k) {
    switch (k) {
        case FileKind::Caff: return "CAFF";
        case FileKind::StreamBundle: return "stream bundle";
        case FileKind::Font: return "XUI font cache";
        case FileKind::Asf: return "XMV/ASF video";
        case FileKind::WaveBank: return "XACT wave bank";
        case FileKind::Ini: return "INI text";
        case FileKind::Bmp: return "BMP image";
        case FileKind::DbIndex: return "asset index";
        default: return "unknown";
    }
}

Loaded loadAsset(const fs::path& p) {
    Loaded l;
    l.raw = readFile(p);
    if (xctd::isXctd(l.raw)) {
        l.raw = xctd::decompress(l.raw);
        l.xctd = true;
    }
    l.kind = detect(l.raw, p);
    if (l.kind == FileKind::Caff) l.caff = caff::Caff::parse(l.raw);
    else if (l.kind == FileKind::StreamBundle) l.sb = streambundle::StreamBundle::parse(l.raw);
    return l;
}

void saveCaff(const fs::path& p, const caff::Caff& c, bool wrap) {
    Bytes out = c.serialize();
    writeFile(p, wrap ? xctd::wrapStored(out) : out);
}

fs::path prepareOutput(const fs::path& input, const std::string& explicitOut) {
    if (!explicitOut.empty()) return explicitOut;
    // A game file with a workspace: the edit goes there, and the game's file is its original.
    if (writePath(input) != input) return input;
    fs::path bak = input;
    bak += ".bak";
    if (!fs::exists(bak)) fs::copy_file(input, bak);
    return input;
}

} // namespace nut
