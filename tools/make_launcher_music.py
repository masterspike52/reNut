#!/usr/bin/env python3
"""Makes the launcher menu's music (ui/launcher_menu.cpp), Mumbo's Mountain,
from the game's own wave bank. The build runs it (CMakeLists.txt) into the
build directory and compiles the result in: the music is the game's, so it is
never kept in the repository.

The bank MusicMumbosMountain (a big-endian XACT wave bank, "DNBW") is one entry
of the stream bundle Bundle/50/685374: one stereo XMA2 track. Its XMA is given
a RIFF XMA2 header, which ffmpeg decodes, and encoded as Ogg Vorbis (quality
4), which reNut's miniaudio plays. The bank's loop region (printed here) is
what launcher_menu.cpp loops: kMusicLoopStart / kMusicLoopFrames.

usage: python make_launcher_music.py <game folder> <out.ogg> [--ffmpeg <ffmpeg>] [--xwb <out.xwb>]
       (ffmpeg from the PATH unless given)
"""
import os
import struct
import subprocess
import sys
import tempfile

BUNDLE = os.path.join("Bundle", "50", "685374")
BANK = b"MusicMumbosMountain"
STREAM_MAGIC = 0x438CB47C


def find_bank(data):
    magic, _, count, _, nref = struct.unpack_from(">5I", data, 0)
    if magic != STREAM_MAGIC:
        raise SystemExit("%s is not a stream bundle" % BUNDLE)
    p = 0x14 + 4 * nref
    for _ in range(count):
        _, off, size = struct.unpack_from(">3I", data, p)
        p += 12
        if data[off:off + 4] == b"DNBW" and data[off + 0x3C:off + 0x3C + len(BANK) + 1] == BANK + b"\0":
            return data[off:off + size]
    raise SystemExit("no %s bank in %s" % (BANK.decode(), BUNDLE))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    game, out = sys.argv[1], sys.argv[2]
    ffmpeg = sys.argv[sys.argv.index("--ffmpeg") + 1] if "--ffmpeg" in sys.argv else "ffmpeg"
    with open(os.path.join(game, BUNDLE), "rb") as f:
        xwb = find_bank(f.read())
    if "--xwb" in sys.argv:
        with open(sys.argv[sys.argv.index("--xwb") + 1], "wb") as f:
            f.write(xwb)

    # Segments: bank data at 0x0C, entries at 0x14, wave data at 0x2C.
    entries = struct.unpack_from(">I", xwb, 0x14)[0]
    waves = struct.unpack_from(">I", xwb, 0x2C)[0]
    duration, _, offset, length, loop_start, loop_length = struct.unpack_from(">6I", xwb, entries)
    samples, rate, channels = duration >> 4, 44100, 2
    xma = xwb[waves + offset:waves + offset + length]
    print("%d samples (%.1f s); loop from %d for %d frames" % (samples, samples / rate, loop_start, loop_length))

    # XMA2WAVEFORMATEX: WAVEFORMATEX (tag 0x166) and the XMA2 fields.
    fmt = struct.pack("<HHIIHHH", 0x166, channels, rate, rate * channels * 2, 2048, 16, 34)
    fmt += struct.pack("<HIIIIIIIBBH", 1, 3, samples, 0x10000, 0, samples, 0, 0, 0, 3, len(xma) // 0x10000 + 1)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(xma)) + xma
    with tempfile.TemporaryDirectory() as tmp:
        riff = os.path.join(tmp, "music.xma")
        with open(riff, "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", riff,
                        "-c:a", "libvorbis", "-q:a", "4", out], check=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
