#!/usr/bin/env python3
"""Dump every Xbox 360 shader from Banjo-Kazooie: Nuts & Bolts bundles.

Output: <out>/{ps,vs}_<xxh3_64 of container>.bin, where each .bin is a complete
Xbox 360 shader container (virtual header + physical block, the layout
XenosRecomp consumes), plus <out>/manifest.csv describing where each came from.

Asset formats handled (all big-endian):

  assets/default.xex   XEX2, AES-128-CBC encrypted (retail/devkit key) with
      basic compression. Shaders compiled into the executable (.rdata) are
      stored whole, constant tables included.

  assets/Bundle/4f/*   CAFF bundles. Most are wrapped in Rare's LZX container:
      +0x00 u32  0x0FF512ED
      +0x04 u32  0x01000000
      +0x08 u32  checksum
      +0x0C u32  H: bits 1-5 LZX window bits, bits 6-21 partition count,
                    bit 22 set -> sizes are u32[count] at +0x10,
                    clear      -> sizes are packed 20-bit fields following H,
                                  padded to a 32-bit boundary
      then u16 size-prefixed LZX frames (0x8000 bytes out each; 0-size = skip).
      Each partition starts 2-byte aligned. A partition of size 0x1FFFE that
      starts with a 0 word is stored raw (incompressible data).
      Each partition is an independent LZX stream.

  assets/Bundle/50/*   Stream bundles (magic 0x438CB47C): a table of
      (aid, offset, size) entries, each an uncompressed CAFF (or wave bank).

  CAFF "07.08.06.0036": header, section table + symbols + file table,
      relocation groups (src file, dst file, count) + field offsets, then
      "mapped" relocation groups in the same format (pointers into the shared
      "pool" asset), then section data (.data, .texturegpu, .gpu, .gpucached,
      .stream). A pointer field holds an offset into its destination file.

Shader containers (magic 0x102A11xx, low bit 1 = vertex shader) live in an
asset's .data file with their constant tables stripped; their physical block
(literal constants + microcode) lives in the pool asset's .gpu (pixel) or
.gpucached (vertex) file. The asset's .data holds one relocated pointer table
per shader type, in the same order as that type's headers appear in .data.
Each pairing is validated by checking the physical block fits before the next
relocated pointer into the same file. Standalone aid_vertexshader_* /
aid_pixelshader_* assets store the whole container contiguously instead.
"""

import argparse
import csv
import os
import re
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

try:
    import xxhash
except ImportError:
    sys.exit("error: the 'xxhash' module is required (pip install xxhash)")


# --------------------------------------------------------------------------
# LZX decoder (libmspack lzxd semantics; 16-bit little-endian words, MSB first)
# --------------------------------------------------------------------------

EXTRA_BITS = []
_j = 0
for _i in range(0, 52, 2):
    EXTRA_BITS += [_j, _j]
    if _i != 0 and _j < 17:
        _j += 1
POSITION_BASE = []
_j = 0
for _i in range(52):
    POSITION_BASE.append(_j)
    _j += 1 << EXTRA_BITS[_i]
POSN_SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}
VERBATIM, ALIGNED, UNCOMPRESSED = 1, 2, 3


class LZXError(Exception):
    pass


class BitReader:
    __slots__ = ("d", "p", "buf", "n")

    def __init__(self, data):
        self.d = data
        self.p = 0
        self.buf = 0
        self.n = 0

    def fill(self, need):
        d = self.d
        while self.n < need:
            p = self.p
            if p + 1 < len(d):
                w = d[p] | (d[p + 1] << 8)
            elif p < len(d):
                w = d[p]
            else:
                w = 0
            self.p = p + 2
            self.buf = (self.buf << 16) | w
            self.n += 16

    def peek(self, k):
        if self.n < k:
            self.fill(k)
        return (self.buf >> (self.n - k)) & ((1 << k) - 1)

    def skip(self, k):
        self.n -= k
        self.buf &= (1 << self.n) - 1

    def read(self, k):
        if k == 0:
            return 0
        v = self.peek(k)
        self.skip(k)
        return v


class Huffman:
    __slots__ = ("table", "maxbits")

    def __init__(self, lens):
        maxbits = max(lens) if lens else 0
        self.maxbits = maxbits
        if maxbits == 0:
            self.table = None
            return
        size = 1 << maxbits
        table = [0] * size
        code = 0
        filled = 0
        for L in range(1, maxbits + 1):
            span = 1 << (maxbits - L)
            for sym, l in enumerate(lens):
                if l == L:
                    start = code << (maxbits - L)
                    table[start:start + span] = [(sym << 5) | L] * span
                    filled += span
                    code += 1
            code <<= 1
        if filled > size:
            raise LZXError("oversubscribed huffman table")
        self.table = table

    def decode(self, br):
        if self.table is None:
            raise LZXError("empty huffman table used")
        e = self.table[br.peek(self.maxbits)]
        br.skip(e & 31)
        return e >> 5


class LZXDecoder:
    def __init__(self, window_bits=17):
        if window_bits not in POSN_SLOTS:
            raise LZXError("unsupported LZX window bits %d" % window_bits)
        self.window_size = 1 << window_bits
        self.posn_slots = POSN_SLOTS[window_bits]
        self.window = bytearray(self.window_size)
        self.wpos = 0
        self.R0 = self.R1 = self.R2 = 1
        self.main_lens = [0] * (256 + self.posn_slots * 8)
        self.len_lens = [0] * 249
        self.header_read = False
        self.block_type = 0
        self.block_remaining = 0
        self.block_length = 0
        self.main_tree = self.len_tree = self.aligned_tree = None

    def _read_lengths(self, br, lens, first, last):
        pre = Huffman([br.read(4) for _ in range(20)])
        x = first
        while x < last:
            z = pre.decode(br)
            if z == 17:
                y = br.read(4) + 4
                lens[x:x + y] = [0] * y
                x += y
            elif z == 18:
                y = br.read(5) + 20
                lens[x:x + y] = [0] * y
                x += y
            elif z == 19:
                y = br.read(1) + 4
                z = lens[x] - pre.decode(br)
                if z < 0:
                    z += 17
                lens[x:x + y] = [z] * y
                x += y
            else:
                z = lens[x] - z
                if z < 0:
                    z += 17
                lens[x] = z
                x += 1

    def _read_block_header(self, br):
        if self.block_type == UNCOMPRESSED and (self.block_length & 1):
            br.p += 1
        self.block_type = bt = br.read(3)
        hi = br.read(16)
        lo = br.read(8)
        self.block_length = self.block_remaining = (hi << 8) | lo
        if bt == ALIGNED:
            self.aligned_tree = Huffman([br.read(3) for _ in range(8)])
        if bt in (VERBATIM, ALIGNED):
            self._read_lengths(br, self.main_lens, 0, 256)
            self._read_lengths(br, self.main_lens, 256, 256 + self.posn_slots * 8)
            self.main_tree = Huffman(self.main_lens)
            self._read_lengths(br, self.len_lens, 0, 249)
            self.len_tree = Huffman(self.len_lens) if any(self.len_lens) else None
        elif bt == UNCOMPRESSED:
            br.fill(16)
            if br.n > 16:
                br.p -= 2
            br.n = 0
            br.buf = 0
            p = br.p
            self.R0, self.R1, self.R2 = struct.unpack_from("<III", br.d, p)
            br.p = p + 12
        else:
            raise LZXError("bad LZX block type %d" % bt)

    def decompress_frame(self, data, frame_size):
        br = BitReader(data)
        if not self.header_read:
            if br.read(1):
                raise LZXError("intel E8 translation is not supported")
            self.header_read = True
        out_start = self.wpos
        window = self.window
        wmask = self.window_size - 1
        remaining = frame_size
        while remaining > 0:
            if self.block_remaining == 0:
                self._read_block_header(br)
            this_run = min(self.block_remaining, remaining)
            remaining -= this_run
            self.block_remaining -= this_run
            wpos = self.wpos
            end = wpos + this_run
            bt = self.block_type
            if bt == UNCOMPRESSED:
                p = br.p
                window[wpos:end] = data[p:p + this_run]
                br.p = p + this_run
                wpos = end
            else:
                main = self.main_tree
                lent = self.len_tree
                alg = self.aligned_tree
                R0, R1, R2 = self.R0, self.R1, self.R2
                while wpos < end:
                    me = main.decode(br)
                    if me < 256:
                        window[wpos] = me
                        wpos += 1
                        continue
                    me -= 256
                    ml = me & 7
                    if ml == 7:
                        ml += lent.decode(br)
                    ml += 2
                    mo = me >> 3
                    if mo > 2:
                        eb = EXTRA_BITS[mo]
                        if bt == VERBATIM:
                            mo = POSITION_BASE[mo] - 2 + br.read(eb) if mo != 3 else 1
                        else:
                            mo = POSITION_BASE[mo] - 2
                            if eb > 3:
                                mo += br.read(eb - 3) << 3
                                mo += alg.decode(br)
                            elif eb == 3:
                                mo += alg.decode(br)
                            elif eb > 0:
                                mo += br.read(eb)
                            else:
                                mo = 1
                        R2 = R1
                        R1 = R0
                        R0 = mo
                    elif mo == 0:
                        mo = R0
                    elif mo == 1:
                        mo = R1
                        R1 = R0
                        R0 = mo
                    else:
                        mo = R2
                        R2 = R0
                        R0 = mo
                    src = wpos - mo
                    if src >= 0 and src + ml <= wpos:
                        window[wpos:wpos + ml] = window[src:src + ml]
                    else:
                        for k in range(ml):
                            window[wpos + k] = window[(src + k) & wmask]
                    wpos += ml
                self.R0, self.R1, self.R2 = R0, R1, R2
                if wpos != end:
                    overrun = wpos - end
                    remaining -= overrun
                    self.block_remaining -= overrun
            self.wpos = wpos
        if self.block_remaining == 0 and self.block_type == UNCOMPRESSED:
            # the realignment pad is outside the next frame's chunk
            self.block_length = 0
        out = bytes(window[out_start:out_start + frame_size])
        if self.wpos >= self.window_size:
            self.wpos &= wmask
        return out


# --------------------------------------------------------------------------
# Byte sources: plain buffers and lazily-decoded Rare LZX containers
# --------------------------------------------------------------------------

RARE_LZX_MAGIC = 0x0FF512ED
RAW_PARTITION_SIZE = 0x20000 - 2


class PlainSource:
    def __init__(self, data):
        self.data = data
        self.size = len(data)

    def read(self, off, n):
        return bytes(self.data[off:off + n])


class RareLZXSource:
    """Decodes only the partitions that are actually read."""

    def __init__(self, data):
        self.data = data
        h = struct.unpack_from(">I", data, 0x0C)[0]
        self.window_bits = (h >> 1) & 0x1F
        count = (h >> 6) & 0xFFFF
        if h & 0x400000:
            sizes = list(struct.unpack_from(">%dI" % count, data, 0x10))
            pos = 0x10 + 4 * count
        else:
            nbytes = ((32 + 20 * count + 31) // 32) * 4
            bits = int.from_bytes(data[0x0C:0x0C + nbytes], "big")
            total = nbytes * 8
            sizes = [(bits >> (total - 32 - 20 * (i + 1))) & 0xFFFFF for i in range(count)]
            pos = 0x0C + nbytes
        # Walk frames (without decoding) to find where each partition starts.
        self.parts = []  # (out_offset, out_size, [(frame_size, chunk_off, chunk_len), ...])
        out = 0
        for size in sizes:
            pos += pos & 1  # partitions start 2-byte aligned
            if size == RAW_PARTITION_SIZE and data[pos:pos + 2] == b"\0\0":
                # incompressible data: a 0 marker then the raw bytes (one 128K block)
                self.parts.append((out, size, [(size, pos + 2, -1)]))
                pos += 2 + size
                out += size
                continue
            frames = []
            got = 0
            while got < size:
                if pos + 2 > len(data):
                    raise LZXError("truncated LZX container")
                if data[pos] == 0xFF:
                    fsz = struct.unpack_from(">H", data, pos + 1)[0]
                    csz = struct.unpack_from(">H", data, pos + 3)[0]
                    pos += 5
                else:
                    fsz = 0x8000
                    csz = struct.unpack_from(">H", data, pos)[0]
                    pos += 2
                if csz == 0:
                    continue
                n = min(fsz, size - got)
                frames.append((n, pos, csz))
                pos += csz
                got += n
            self.parts.append((out, size, frames))
            out += size
        self.size = out
        self.cache = {}

    def _part(self, i):
        buf = self.cache.get(i)
        if buf is None:
            _, _, frames = self.parts[i]
            if frames[0][2] < 0:
                n, o, _ = frames[0]
                buf = bytes(self.data[o:o + n])
            else:
                dec = LZXDecoder(self.window_bits)
                buf = b"".join(dec.decompress_frame(self.data[o:o + c], n) for n, o, c in frames)
            self.cache[i] = buf
        return buf

    def read(self, off, n):
        end = min(off + n, self.size)
        out = []
        for i, (po, ps, _) in enumerate(self.parts):
            if po + ps <= off or po >= end:
                continue
            buf = self._part(i)
            out.append(buf[max(off, po) - po:min(end, po + ps) - po])
        return b"".join(out)


# --------------------------------------------------------------------------
# XEX2 (default.xex): AES-128 decryption + basic compression
# --------------------------------------------------------------------------

def _aes_tables():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1 | q >> 7) & 0xFF) ^ ((q << 2 | q >> 6) & 0xFF) \
            ^ ((q << 3 | q >> 5) & 0xFF) ^ ((q << 4 | q >> 4) & 0xFF)
        sbox[p] = x ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i

    def mul(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
            b >>= 1
        return r

    td0 = [(mul(inv[x], 14) << 24) | (mul(inv[x], 9) << 16) | (mul(inv[x], 13) << 8) | mul(inv[x], 11)
           for x in range(256)]
    ror = lambda t, n: [((v >> n) | (v << (32 - n))) & 0xFFFFFFFF for v in t]
    return sbox, inv, td0, ror(td0, 8), ror(td0, 16), ror(td0, 24)


class AES128Decryptor:
    """Minimal AES-128 decryption (ECB block + CBC)."""
    _T = None

    def __init__(self, key):
        self._fast = None
        try:
            from Crypto.Cipher import AES
            self._fast = lambda data, iv: AES.new(key, AES.MODE_CBC, iv).decrypt(data)
            return
        except ImportError:
            pass
        try:
            from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
            self._fast = lambda data, iv: Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(data)
            return
        except ImportError:
            pass
        if AES128Decryptor._T is None:
            AES128Decryptor._T = _aes_tables()
        sbox, _, td0, td1, td2, td3 = self._T
        w = list(struct.unpack(">4I", key))
        rcon = 1
        for i in range(4, 44):
            t = w[i - 1]
            if i % 4 == 0:
                t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
                t = (sbox[t >> 24] << 24) | (sbox[(t >> 16) & 255] << 16) | (sbox[(t >> 8) & 255] << 8) | sbox[t & 255]
                t ^= rcon << 24
                rcon = ((rcon << 1) ^ 0x1B) & 0xFF if rcon & 0x80 else rcon << 1
            w.append(w[i - 4] ^ t)
        dk = []
        for r in range(10, -1, -1):
            for c in range(4):
                v = w[4 * r + c]
                if 0 < r < 10:
                    v = td0[sbox[v >> 24]] ^ td1[sbox[(v >> 16) & 255]] ^ td2[sbox[(v >> 8) & 255]] ^ td3[sbox[v & 255]]
                dk.append(v)
        self.dk = dk

    def decrypt_cbc(self, data, iv=bytes(16)):
        if self._fast:
            return self._fast(bytes(data), iv)
        _, inv, td0, td1, td2, td3 = self._T
        dk = self.dk
        out = bytearray(len(data))
        p0, p1, p2, p3 = struct.unpack(">4I", iv)
        unpack = struct.Struct(">4I").unpack_from
        pack = struct.Struct(">4I").pack_into
        for off in range(0, len(data) - 15, 16):
            c0, c1, c2, c3 = unpack(data, off)
            s0, s1, s2, s3 = c0 ^ dk[0], c1 ^ dk[1], c2 ^ dk[2], c3 ^ dk[3]
            for k in range(4, 40, 4):
                s0, s1, s2, s3 = (
                    td0[s0 >> 24] ^ td1[(s3 >> 16) & 255] ^ td2[(s2 >> 8) & 255] ^ td3[s1 & 255] ^ dk[k],
                    td0[s1 >> 24] ^ td1[(s0 >> 16) & 255] ^ td2[(s3 >> 8) & 255] ^ td3[s2 & 255] ^ dk[k + 1],
                    td0[s2 >> 24] ^ td1[(s1 >> 16) & 255] ^ td2[(s0 >> 8) & 255] ^ td3[s3 & 255] ^ dk[k + 2],
                    td0[s3 >> 24] ^ td1[(s2 >> 16) & 255] ^ td2[(s1 >> 8) & 255] ^ td3[s0 & 255] ^ dk[k + 3])
            pack(out, off,
                 ((inv[s0 >> 24] << 24) | (inv[(s3 >> 16) & 255] << 16) | (inv[(s2 >> 8) & 255] << 8) | inv[s1 & 255]) ^ dk[40] ^ p0,
                 ((inv[s1 >> 24] << 24) | (inv[(s0 >> 16) & 255] << 16) | (inv[(s3 >> 8) & 255] << 8) | inv[s2 & 255]) ^ dk[41] ^ p1,
                 ((inv[s2 >> 24] << 24) | (inv[(s1 >> 16) & 255] << 16) | (inv[(s0 >> 8) & 255] << 8) | inv[s3 & 255]) ^ dk[42] ^ p2,
                 ((inv[s3 >> 24] << 24) | (inv[(s2 >> 16) & 255] << 16) | (inv[(s1 >> 8) & 255] << 8) | inv[s0 & 255]) ^ dk[43] ^ p3)
            p0, p1, p2, p3 = c0, c1, c2, c3
        return bytes(out)


XEX_KEYS = (bytes.fromhex("20B185A59D28FDC340583FBB0896BF91"),  # retail
            bytes(16))                                           # devkit


def load_xex_image(data):
    """Return (image bytes, base address) of an XEX2 file."""
    if data[:4] != b"XEX2":
        raise ValueError("not an XEX2 file")
    pe_off, _, sec_off, nopt = struct.unpack_from(">IIII", data, 8)
    opts = dict(struct.unpack_from(">II", data, 0x18 + 8 * i) for i in range(nopt))
    base = opts.get(0x10201, 0x82000000)
    ff = opts[0x3FF]
    ff_size, enc, comp = struct.unpack_from(">IHH", data, ff)
    payload = data[pe_off:]
    if enc:
        file_key = data[sec_off + 0x150:sec_off + 0x160]
        for key in XEX_KEYS:
            session = AES128Decryptor(key).decrypt_cbc(file_key)
            head = AES128Decryptor(session).decrypt_cbc(payload[:16])
            if head[:2] == b"MZ" or comp == 2:
                payload = AES128Decryptor(session).decrypt_cbc(payload[:len(payload) & ~15])
                break
        else:
            raise ValueError("could not decrypt XEX (unknown key)")
    if comp == 1:  # basic: (data size, zero-fill size) blocks
        out = bytearray()
        p = 0
        for i in range((ff_size - 8) // 8):
            dsz, zsz = struct.unpack_from(">II", data, ff + 8 + 8 * i)
            out += payload[p:p + dsz]
            out += bytes(zsz)
            p += dsz
        image = bytes(out)
    elif comp == 0:
        image = payload
    else:
        raise ValueError("XEX compression type %d is not supported" % comp)
    if image[:2] != b"MZ":
        raise ValueError("XEX image did not decode to a PE")
    return image, base


# --------------------------------------------------------------------------
# CAFF 07.08.06.0036
# --------------------------------------------------------------------------

def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


class Caff:
    def __init__(self, src):
        self.src = src
        head = src.read(0, 0x78)
        if head[:4] != b"CAFF":
            raise ValueError("not a CAFF")
        self.version = head[4:0x14].split(b"\0")[0].decode()
        if self.version != "07.08.06.0036":
            raise ValueError("unsupported CAFF version %s" % self.version)
        hs = u32(head, 0x14)
        num_symbols = u32(head, 0x1C)
        num_files = u32(head, 0x20)
        num_bsw_groups, _, num_groups, _ = struct.unpack_from(">4I", head, 0x24)
        num_mbsw_groups, _, num_mgroups, _ = struct.unpack_from(">4I", head, 0x34)
        if num_bsw_groups or num_mbsw_groups:
            raise ValueError("CAFF byteswap tables are not supported")
        num_sec = head[0x49]
        if head[0x4A]:
            raise ValueError("internally compressed CAFF is not supported")
        names_len = u32(head, 0x4C)
        sect_size = u32(head, 0x50)
        reloc_size = u32(head, 0x64)
        meta = src.read(0, hs + sect_size + reloc_size)

        self.sections = []
        p = hs
        for _ in range(num_sec):
            name_off, align, pool, size = struct.unpack_from(">IBII", meta, p)
            self.sections.append({"name_off": name_off, "size": size})
            p += 0x21
        for s in self.sections:
            s["name"] = meta[p + s["name_off"]:].split(b"\0", 1)[0].decode("latin1")
        p += names_len
        label_size = u32(meta, p)
        offs = struct.unpack_from(">%dI" % num_symbols, meta, p + 4)
        lb = p + 4 + 4 * num_symbols
        self.symbols = [meta[lb + o:].split(b"\0", 1)[0].decode("latin1") for o in offs]
        p = lb + label_size
        p += 4 + u32(meta, p)  # adb string
        self.files = []
        for _ in range(num_files):
            aid, start, size = struct.unpack_from(">III", meta, p)
            self.files.append({"asset": aid, "start": start, "size": size, "sec": meta[p + 12]})
            p += 14

        base = hs + sect_size + reloc_size
        for s in self.sections:
            s["offset"] = base
            base += s["size"]

        # relocations: {src_file: [(field_offset, dst_file), ...]} (1-based files).
        # Regular groups come first, then "mapped" groups (pointers into pool
        # assets); both are (src file, dst file, count) + count field offsets.
        self.relocs = {}
        p = hs + sect_size
        for count in (num_groups, num_mgroups):
            groups = [struct.unpack_from(">III", meta, p + 12 * i) for i in range(count)]
            p += 12 * count
            for s, t, n in groups:
                lst = self.relocs.setdefault(s, [])
                lst.extend((o, t) for o in struct.unpack_from(">%dI" % n, meta, p))
                p += 4 * n

    def section_name(self, file_idx):
        return self.sections[self.files[file_idx - 1]["sec"] - 1]["name"]

    def file_offset(self, file_idx):
        f = self.files[file_idx - 1]
        return self.sections[f["sec"] - 1]["offset"] + f["start"]

    def read_file(self, file_idx):
        return self.src.read(self.file_offset(file_idx), self.files[file_idx - 1]["size"])

    def asset_name(self, file_idx):
        aid = self.files[file_idx - 1]["asset"]
        return self.symbols[aid] if 0 <= aid < len(self.symbols) else ""


# --------------------------------------------------------------------------
# Shader extraction
# --------------------------------------------------------------------------

SHADER_MAGIC = re.compile(rb"\x10\x2a\x11", re.S)
GPU_SECTIONS = (".gpu", ".gpucached")
# Section each shader type's physical blocks normally live in (tie-breaker only).
PREFERRED_SECTION = {"ps": ".gpu", "vs": ".gpucached"}


def shader_kind(flags):
    return "vs" if flags & 1 else "ps"


def find_shader_headers(data):
    hdrs = []
    for m in SHADER_MAGIC.finditer(data):
        o = m.start()
        if o % 4 or o + 0x24 > len(data):
            continue
        virt, phys = struct.unpack_from(">II", data, o + 4)
        ct, dt, so = struct.unpack_from(">III", data, o + 0x10)
        if not (0x24 <= virt < 0x10000 and 0 < phys < 0x100000 and o + virt <= len(data)):
            continue
        if ct >= virt or dt >= virt or not (0x24 <= so < virt - 8):
            continue
        poff, psize = struct.unpack_from(">II", data, o + so)
        if poff + psize != phys:
            continue
        hdrs.append(o)
    return hdrs


def extract_from_xex(path):
    """Shaders compiled into the executable are stored whole (with constant tables)."""
    with open(path, "rb") as fh:
        image, base = load_xex_image(fh.read())
    results = []
    for h in find_shader_headers(image):
        virt, phys = struct.unpack_from(">II", image, h + 4)
        blob = image[h:h + virt + phys]
        if len(blob) != virt + phys:
            continue
        results.append({
            "kind": shader_kind(u32(image, h)),
            "hash": xxhash.xxh3_64_hexdigest(blob),
            "blob": blob,
            "virt": virt,
            "phys": phys,
            "asset": "0x%08x" % (base + h),
            "origin": path,
        })
    return results


def pointer_runs(caff, file_idx, data):
    """Runs of consecutive relocated u32 fields that all point into one GPU file."""
    fields = sorted((o, t) for o, t in caff.relocs.get(file_idx, ())
                    if caff.section_name(t) in GPU_SECTIONS)
    runs = []
    for o, t in fields:
        if runs and runs[-1][-1][0] + 4 == o and runs[-1][-1][1] == t:
            runs[-1].append((o, t))
        else:
            runs.append([(o, t)])
    return [(r[0][1], [u32(data, o) for o, _ in r]) for r in runs]


def fits(caff, targets, hdr_sizes, gfile, ptrs):
    size = caff.files[gfile - 1]["size"]
    tg = targets[gfile]
    for phys, g in zip(hdr_sizes, ptrs):
        nxt = next((v for v in tg if v > g), size)
        if g + phys > nxt:
            return False
    return True


def pair_shaders(caff, file_idx, data, hdrs, targets):
    """Return [(hdr_offset, gpu_file_idx, gpu_offset)] or raise ValueError.

    Each shader type has its own table of physical-block pointers, in the same
    order as that type's headers appear in the .data file.
    """
    runs = pointer_runs(caff, file_idx, data)
    if not runs and hdrs == [0] and sum(struct.unpack_from(">II", data, 4)) <= len(data):
        # standalone aid_vertexshader_*/aid_pixelshader_* asset: the whole
        # container (virtual + physical) is stored contiguously in .data
        return [(0, file_idx, u32(data, 4))]
    pairs = []
    used = set()
    for kind in ("ps", "vs"):
        hs = [h for h in hdrs if shader_kind(u32(data, h)) == kind]
        if not hs:
            continue
        sizes = [u32(data, h + 8) for h in hs]
        cands = [i for i, (gfile, ptrs) in enumerate(runs)
                 if i not in used and len(ptrs) == len(hs) and fits(caff, targets, sizes, gfile, ptrs)]
        if len(cands) > 1:
            pref = [i for i in cands if caff.section_name(runs[i][0]) == PREFERRED_SECTION[kind]]
            if pref:
                cands = pref
        if len(cands) != 1:
            raise ValueError("%d candidate pointer tables for %d %s shaders (run lengths: %s)"
                             % (len(cands), len(hs), kind, [len(p) for _, p in runs]))
        used.add(cands[0])
        gfile, ptrs = runs[cands[0]]
        pairs += [(h, gfile, g) for h, g in zip(hs, ptrs)]
    return sorted(pairs)


def gpu_targets(caff):
    """All relocated pointer values per destination .gpu file (sorted)."""
    targets = {}
    data_cache = {}
    for s, lst in caff.relocs.items():
        for o, t in lst:
            if caff.section_name(t) not in GPU_SECTIONS:
                continue
            if s not in data_cache:
                data_cache[s] = caff.read_file(s)
            targets.setdefault(t, set()).add(u32(data_cache[s], o))
    return {t: sorted(v) for t, v in targets.items()}


def extract_from_caff(caff, origin):
    """Yield dicts describing each shader found in the CAFF."""
    results, problems = [], []
    targets = None
    gpu_cache = {}
    for fi, f in enumerate(caff.files, 1):
        if caff.sections[f["sec"] - 1]["name"] != ".data":
            continue
        data = caff.read_file(fi)
        hdrs = find_shader_headers(data)
        if not hdrs:
            continue
        name = caff.asset_name(fi)
        if targets is None:
            targets = gpu_targets(caff)
        try:
            pairs = pair_shaders(caff, fi, data, hdrs, targets)
        except ValueError as e:
            problems.append("%s file %d (%s): %s" % (origin, fi, name, e))
            continue
        for h, gfile, goff in pairs:
            virt, phys = struct.unpack_from(">II", data, h + 4)
            if gfile not in gpu_cache:
                gpu_cache[gfile] = caff.read_file(gfile)
            blob = data[h:h + virt] + gpu_cache[gfile][goff:goff + phys]
            results.append({
                "kind": shader_kind(u32(data, h)),
                "hash": xxhash.xxh3_64_hexdigest(blob),
                "blob": blob,
                "virt": virt,
                "phys": phys,
                "asset": name,
                "origin": origin,
            })
    return results, problems


STREAM_MAGIC = 0x438CB47C


def iter_caffs(path):
    """Yield (origin, Caff) for every CAFF inside an asset file."""
    with open(path, "rb") as fh:
        data = fh.read()
    magic = u32(data, 0) if len(data) >= 4 else 0
    if magic == RARE_LZX_MAGIC:
        src = RareLZXSource(data)
        yield path, Caff(src)
    elif data[:4] == b"CAFF":
        yield path, Caff(PlainSource(data))
    elif magic == STREAM_MAGIC:
        _, count, _, nref = struct.unpack_from(">4I", data, 4)
        p = 0x14 + 4 * nref
        mv = memoryview(data)
        for _ in range(count):
            aid, off, size = struct.unpack_from(">3I", data, p)
            p += 12
            if data[off:off + 4] == b"CAFF":
                yield "%s#%08x" % (path, aid), Caff(PlainSource(mv[off:off + size]))


def process_file(path):
    results, problems = [], []
    try:
        with open(path, "rb") as fh:
            if fh.read(4) == b"XEX2":
                return path, extract_from_xex(path), problems
        for origin, caff in iter_caffs(path):
            r, p = extract_from_caff(caff, origin)
            results += r
            problems += p
    except Exception as e:  # keep going with other files
        problems.append("%s: %s: %s" % (path, type(e).__name__, e))
    return path, results, problems


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--assets", default=os.path.join(root, "assets"),
                    help="game assets directory (default: %(default)s)")
    ap.add_argument("--out", default=os.path.join(root, "shaders"),
                    help="output directory (default: %(default)s)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1,
                    help="worker processes (default: %(default)s)")
    ap.add_argument("--ucode", action="store_true",
                    help="also write <name>.ucode (physical block only)")
    ap.add_argument("--allow-problems", action="store_true",
                    help="exit 0 even when some assets could not be read (the build step: "
                         "the problems are printed, the shaders found are kept)")
    ap.add_argument("files", nargs="*",
                    help="specific asset files to scan (default: <assets>/default.xex "
                         "and everything under <assets>/Bundle)")
    args = ap.parse_args()

    paths = args.files
    if not paths:
        xex = os.path.join(args.assets, "default.xex")
        if os.path.isfile(xex):
            paths.append(xex)
        bundle_dir = os.path.join(args.assets, "Bundle")
        for dp, _, fns in os.walk(bundle_dir):
            paths += [os.path.join(dp, fn) for fn in sorted(fns)]
    if not paths:
        sys.exit("no asset files found")
    # biggest first so the pool stays busy
    paths.sort(key=lambda p: -os.path.getsize(p))

    os.makedirs(args.out, exist_ok=True)
    seen = {}
    manifest = []
    problems = []
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [ex.submit(process_file, p) for p in paths]
        for i, fut in enumerate(as_completed(futs), 1):
            path, results, probs = fut.result()
            problems += probs
            new = 0
            for r in results:
                name = "%s_%s" % (r["kind"], r["hash"])
                manifest.append((name, r["kind"], r["virt"], r["phys"],
                                 os.path.relpath(r["origin"].split("#")[0], args.assets)
                                 + ("#" + r["origin"].split("#")[1] if "#" in r["origin"] else ""),
                                 r["asset"]))
                if name in seen:
                    continue
                seen[name] = True
                new += 1
                with open(os.path.join(args.out, name + ".bin"), "wb") as fh:
                    fh.write(r["blob"])
                if args.ucode:
                    with open(os.path.join(args.out, name + ".ucode"), "wb") as fh:
                        fh.write(r["blob"][r["virt"]:])
            print("[%3d/%d] %-40s %5d shaders, %4d new  (%.0fs)"
                  % (i, len(paths), os.path.relpath(path, args.assets), len(results), new,
                     time.time() - t0), flush=True)

    manifest.sort()
    with open(os.path.join(args.out, "manifest.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["name", "type", "virtual_size", "physical_size", "source", "asset"])
        w.writerows(manifest)

    n_ps = sum(1 for k in seen if k.startswith("ps_"))
    print("\n%d unique shaders (%d ps, %d vs) from %d occurrences -> %s"
          % (len(seen), n_ps, len(seen) - n_ps, len(manifest), args.out))
    if problems:
        print("\n%d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
    return 1 if problems and not args.allow_problems else 0


if __name__ == "__main__":
    sys.exit(main())
