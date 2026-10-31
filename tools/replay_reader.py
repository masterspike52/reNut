#!/usr/bin/env python3
"""Reads Banjo-Kazooie: Nuts & Bolts leaderboard replays (.replay files).

A .replay is what the RexLiveless admin console downloads for a score: the
game clip the game uploaded with it (XStorageUploadFromMemory). Layout, from
N&B TU3 (IDA), everything big-endian:

  The upload (sub_823D5100):
    u32  0x0000BEEF
    u32  extra size (0 for leaderboard replays)
    u32  replay size
         the extra data, then the replay

  The replay (sub_8252B628), a 0x130-byte header:
    0x00  u8       complete (1 once the replay was written)
    0x04  float    39.0, always
    0x08  u32      format fingerprint: a hash of the replay's structure sizes
                   (sub_825273F0); builds that lay the data out differently
                   don't match
    0x0C  u32      id from the challenge's data
    0x10  float    the result: the score on a points challenge, otherwise the
                   time in seconds (the time left on one with a time limit)
    0x14  u32      a per-player value from the challenge's results
    0x18  u8       1
    0x1C  u32      id from the world
    0x20  u8       byte_82E19934 (a game mode flag)
    0x28  u64      player 1's XUID when it was recorded
    0x30  WCHAR[64] the challenge, "<LEVEL> ACT <n> - <CHALLENGE>"
    0xB0  char[64] "Head.Of.Server", always (a string in the code)
    0xF0  u32[16]
  then the recording, in zlib chunks until the end, each:
    u32  compressed size
    u32  uncompressed size
         a zlib stream

usage: python replay_reader.py <file.replay> [--extract <folder>]
         --extract writes header.bin and chunk<N>.bin (each chunk unpacked)
"""
import struct
import sys
import zlib
from pathlib import Path

UPLOAD_MAGIC = 0x0000BEEF
HEADER_SIZE = 0x130


def fail(message):
    sys.exit(f"replay_reader: {message}")


def read_replay(data):
    """The replay inside an upload (or the file itself, if it's a bare one)
    and the upload's extra data."""
    if len(data) >= 12 and struct.unpack_from(">I", data)[0] == UPLOAD_MAGIC:
        extra_size, replay_size = struct.unpack_from(">II", data, 4)
        if 12 + extra_size + replay_size != len(data):
            fail(f"the upload says {12 + extra_size + replay_size} bytes, the file has {len(data)}")
        return data[12 + extra_size:], data[12:12 + extra_size]
    return data, b""


def parse_header(replay):
    if len(replay) < HEADER_SIZE:
        fail(f"too short for a replay header ({len(replay)} bytes)")
    h = replay[:HEADER_SIZE]
    u8 = lambda off: h[off]
    u32 = lambda off: struct.unpack_from(">I", h, off)[0]
    f32 = lambda off: struct.unpack_from(">f", h, off)[0]
    return {
        "complete": u8(0x00),
        "constant": f32(0x04),
        "fingerprint": u32(0x08),
        "challenge_id": u32(0x0C),
        "result": f32(0x10),
        "player_value": u32(0x14),
        "flag_18": u8(0x18),
        "world_id": u32(0x1C),
        "mode_flag": u8(0x20),
        "xuid": struct.unpack_from(">Q", h, 0x28)[0],
        "title": h[0x30:0xB0].decode("utf-16-be", "replace").split("\0")[0],
        "tag": h[0xB0:0xF0].split(b"\0")[0].decode("latin-1"),
        "slots": struct.unpack_from(">16I", h, 0xF0),
    }


def read_chunks(replay):
    """[(offset, compressed size, uncompressed size, data or error)]."""
    chunks = []
    pos = HEADER_SIZE
    while pos < len(replay):
        if pos + 8 > len(replay):
            fail(f"{len(replay) - pos} stray bytes at the end (offset {pos:#x})")
        compressed, raw = struct.unpack_from(">II", replay, pos)
        if pos + 8 + compressed > len(replay):
            fail(f"chunk at {pos:#x} says {compressed} bytes, only {len(replay) - pos - 8} are left")
        try:
            data = zlib.decompress(replay[pos + 8:pos + 8 + compressed])
            if len(data) != raw:
                data = f"unpacks to {len(data)} bytes, not {raw}"
        except zlib.error as e:
            data = f"zlib error: {e}"
        chunks.append((pos, compressed, raw, data))
        pos += 8 + compressed
    return chunks


def main(argv):
    args = argv[1:]
    extract = None
    if "--extract" in args:
        i = args.index("--extract")
        if i + 1 >= len(args):
            fail("--extract needs a folder")
        extract = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)

    path = Path(args[0])
    replay, extra = read_replay(path.read_bytes())
    h = parse_header(replay)
    chunks = read_chunks(replay)

    print(f"{path.name}")
    print(f"  challenge     {h['title']}")
    print(f"  result        {h['result']:g}   (score, or time in seconds)")
    print(f"  player XUID   {h['xuid']:016X}")
    print(f"  complete      {'yes' if h['complete'] else 'no'}")
    print(f"  fingerprint   {h['fingerprint']:08X}   (format)")
    print(f"  challenge id  {h['challenge_id']:08X}")
    print(f"  world id      {h['world_id']:08X}")
    print(f"  player value  {h['player_value']}")
    print(f"  mode flag     {h['mode_flag']}")
    if h["constant"] != 39.0 or h["tag"] != "Head.Of.Server" or h["flag_18"] != 1:
        print(f"  (unusual: constant {h['constant']:g}, +0x18 {h['flag_18']}, tag '{h['tag']}')")
    if any(h["slots"]):
        print(f"  +0xF0         {' '.join(f'{s:08X}' for s in h['slots'])}")
    if extra:
        print(f"  extra data    {len(extra)} bytes")
    total = sum(raw for _, _, raw, _ in chunks)
    print(f"  recording     {len(chunks)} chunk(s), {total} bytes unpacked")
    for n, (offset, compressed, raw, data) in enumerate(chunks):
        state = "" if isinstance(data, bytes) else f"   BAD: {data}"
        print(f"    chunk {n}  at {offset:#07x}  {compressed:>7} -> {raw:>7} bytes{state}")

    if extract:
        extract.mkdir(parents=True, exist_ok=True)
        (extract / "header.bin").write_bytes(replay[:HEADER_SIZE])
        if extra:
            (extract / "extra.bin").write_bytes(extra)
        for n, (_, _, _, data) in enumerate(chunks):
            if isinstance(data, bytes):
                (extract / f"chunk{n}.bin").write_bytes(data)
        print(f"  extracted to  {extract}")
    return 0 if all(isinstance(d, bytes) for _, _, _, d in chunks) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
