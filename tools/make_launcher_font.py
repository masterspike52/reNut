#!/usr/bin/env python3
"""Builds the launcher menu's font (gpu/overlay/launcher_font_data.inc) from the
game's own glyph cache, MS Comic Sans Euro (xuifontcachefont/euro.sbp, every
glyph), as a TrueType font written, zstd-compressed, as a C++ byte array.

The cache (as NutTool reads it): 01 01 lineHeight bitsPerPixel(4) u16 count,
then {u16 code, u32 offset, i8 metrics[4]} per glyph, each glyph one zlib
stream of a square cell at 4 bits per pixel, low nibble first. Every pixel of
coverage 8/15 or more becomes a square of the TrueType outline; 1 em is the
cache's line height, and the spacing is the game's: the pen moves by
metrics[1] before the cell is drawn and advances by metrics[1..3].

usage: python make_launcher_font.py <game folder> <out.inc> [--ttf <out.ttf>]
"""
import os
import re
import struct
import sys
import zlib

CACHE = os.path.join("xuifontcachefont", "euro.sbp")
FAMILY = "Nuts and Bolts"
UNITS = 64  # TrueType units per glyph pixel
BASELINE_LETTERS = b"HIELTZxzmnuv"  # flat-bottomed: their bottom row is the baseline

LOW = bytes(b & 15 for b in range(256))
HIGH = bytes(b >> 4 for b in range(256))
INKED = bytes(1 if v >= 8 else 0 for v in range(256))


class Glyph:
    __slots__ = ("code", "metrics", "inked")


def parse(data):
    if len(data) < 16 or data[0] != 1 or data[1] != 1:
        raise SystemExit("%s is not an XUI glyph cache" % CACHE)
    if data[3] != 4:
        raise SystemExit("%s: only 4-bit glyph caches are supported" % CACHE)
    count = struct.unpack_from(">H", data, 4)[0]
    records = [struct.unpack_from(">HI4b", data, 6 + 10 * i) for i in range(count)]
    glyphs, cell = [], 0
    for i, (code, offset, *metrics) in enumerate(records):
        end = records[i + 1][1] if i + 1 < count else len(data)
        raw = zlib.decompress(data[offset:end])
        if not cell:
            cell = round((len(raw) * 2) ** 0.5)
        if len(raw) * 2 != cell * cell:
            raise SystemExit("%s: inconsistent glyph cell size" % CACHE)
        pixels = bytearray(len(raw) * 2)
        pixels[0::2] = raw.translate(LOW)
        pixels[1::2] = raw.translate(HIGH)
        g = Glyph()
        g.code, g.metrics, g.inked = code, metrics, bytes(pixels).translate(INKED)
        glyphs.append(g)
    return data[2], cell, glyphs


def baseline_row(glyphs, cell, line_height):
    votes = {}
    for g in glyphs:
        if g.code < 128 and g.code and bytes([g.code]) in BASELINE_LETTERS:
            last = g.inked.rfind(1)
            if last >= 0:
                row = last // cell + 1
                votes[row] = votes.get(row, 0) + 1
    best, most = 0, 0
    for row in sorted(votes):
        if votes[row] > most:
            best, most = row, votes[row]
    return best if most else (line_height * 3 + 2) // 4


def advance(g):
    return max(0, g.metrics[1] + g.metrics[2] + g.metrics[3])


def trace(inked, n):
    """Outlines of the inked pixels, clockwise with y up (rows from the cell
    bottom); holes come out counter-clockwise, as TrueType expects."""
    w = n + 2  # a blank border, so neighbours need no bounds checks
    grid = bytearray(w * w)
    for row in range(n):
        grid[(row + 1) * w + 1:(row + 1) * w + 1 + n] = inked[row * n:(row + 1) * n]
    stride = n + 1
    out = bytearray(stride * stride)  # bit d: an unused edge leaves the vertex in direction d
    for m in re.finditer(rb"\x01", inked):
        row, x = divmod(m.start(), n)
        p = (row + 1) * w + x + 1
        y = n - row - 1
        if not grid[p - 1]:
            out[y * stride + x] |= 2
        if not grid[p - w]:
            out[(y + 1) * stride + x] |= 1
        if not grid[p + 1]:
            out[(y + 1) * stride + x + 1] |= 8
        if not grid[p + w]:
            out[y * stride + x + 1] |= 4
    dx, dy, step = (1, 0, -1, 0), (0, 1, 0, -1), (1, stride, -1, -stride)
    contours = []
    for m in re.finditer(rb"[^\x00]", bytes(out)):
        v = m.start()
        y, x = divmod(v, stride)
        while out[v]:
            edges = out[v]
            d = (edges & -edges).bit_length() - 1
            path, cx, cy, cv = [], x, y, v
            while True:
                out[cv] &= ~(1 << d) & 0xFF
                path.append((cx, cy))
                cx, cy, cv = cx + dx[d], cy + dy[d], cv + step[d]
                if cv == v:
                    break
                # Right turns first, so pixels touching at a corner become separate contours.
                e = out[cv]
                for t in ((d + 3) & 3, d, (d + 1) & 3):
                    if e >> t & 1:
                        d = t
                        break
                else:
                    break
            corners = []
            for i, q in enumerate(path):
                p, r = path[i - 1], path[(i + 1) % len(path)]
                if (q[0] - p[0]) * (r[1] - q[1]) != (q[1] - p[1]) * (r[0] - q[0]):
                    corners.append(q)
            if len(corners) >= 3:
                contours.append(corners)
    return contours


def coordinate(delta, short_bit, same_bit, out):
    if delta == 0:
        return same_bit
    if abs(delta) < 256:
        out.append(abs(delta))
        return short_bit | (same_bit if delta > 0 else 0)
    out += struct.pack(">h", delta)
    return 0


def build_glyph(g, cell, base):
    """The glyph's glyf data, bounds, point and contour counts."""
    contours = trace(g.inked, cell)
    if not contours:
        return b"", (0, 0, 0, 0), 0, 0
    ox, oy = g.metrics[1], base - cell
    contours = [[((x + ox) * UNITS, (y + oy) * UNITS) for x, y in c] for c in contours]
    xs_all = [p[0] for c in contours for p in c]
    ys_all = [p[1] for c in contours for p in c]
    box = (min(xs_all), min(ys_all), max(xs_all), max(ys_all))
    data = bytearray(struct.pack(">h4h", len(contours), *box))
    points = 0
    for c in contours:
        points += len(c)
        data += struct.pack(">H", points - 1)
    data += struct.pack(">H", 0)  # no instructions
    flags, xs, ys = bytearray(), bytearray(), bytearray()
    px = py = 0
    for c in contours:
        for x, y in c:
            flags.append(1 | coordinate(x - px, 0x02, 0x10, xs) | coordinate(y - py, 0x04, 0x20, ys))
            px, py = x, y
    data += flags + xs + ys
    data += bytes(-len(data) & 3)
    return bytes(data), box, points, len(contours)


def checksum(data):
    data = data + bytes(-len(data) & 3)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def search_fields(count, unit):
    p, log = 1, 0
    while p * 2 <= count:
        p, log = p * 2, log + 1
    return struct.pack(">3H", p * unit, log, count * unit - p * unit)


def to_ttf(line_height, cell, glyphs):
    U = UNITS
    lh = line_height or cell
    base = baseline_row(glyphs, cell, lh)
    ascent, descent, em = base * U, (lh - base) * U, lh * U

    # Glyph 0 (.notdef) is the cache's U+0001, which the game draws for missing characters.
    notdef = next((g for g in glyphs if g.code == 1), None)
    order = [notdef] + sorted((g for g in glyphs if g.code not in (1, 0xFFFF)), key=lambda g: g.code)
    if len(order) > 0xFFFF:
        raise SystemExit("too many glyphs for TrueType")

    glyf, loca, hmtx = bytearray(), bytearray(), bytearray()
    x_min = y_min = min_lsb = min_rsb = 1 << 30
    x_max = y_max = max_extent = -(1 << 30)
    adv_max = max_points = max_contours = x_height = cap_height = 0
    adv_sum = adv_count = 0
    for g in order:
        data, box, points, contours = build_glyph(g, cell, base) if g else (b"", (0, 0, 0, 0), 0, 0)
        adv = (advance(g) if g else lh // 2) * U
        loca += struct.pack(">I", len(glyf))
        glyf += data
        hmtx += struct.pack(">Hh", adv, box[0] if contours else 0)
        adv_max = max(adv_max, adv)
        if adv:
            adv_sum, adv_count = adv_sum + adv, adv_count + 1
        if not contours:
            continue
        x_min, y_min = min(x_min, box[0]), min(y_min, box[1])
        x_max, y_max = max(x_max, box[2]), max(y_max, box[3])
        min_lsb, min_rsb = min(min_lsb, box[0]), min(min_rsb, adv - box[2])
        max_extent = max(max_extent, box[2])
        max_points, max_contours = max(max_points, points), max(max_contours, contours)
        if g.code == ord("x"):
            x_height = box[3]
        if g.code == ord("H"):
            cap_height = box[3]
    loca += struct.pack(">I", len(glyf))
    if x_min == 1 << 30:
        x_min = y_min = x_max = y_max = min_lsb = min_rsb = max_extent = 0

    # cmap: runs of consecutive codes map to consecutive glyph ids.
    runs = []
    for gid in range(1, len(order)):
        code = order[gid].code
        if runs and runs[-1][1] + 1 == code:
            runs[-1][1] = code
        else:
            runs.append([code, code, gid])
    present = {g.code for g in order[1:]}
    segs = len(runs) + 1
    fmt4 = 16 + segs * 8 <= 0xFFFF
    cmap = bytearray(struct.pack(">HH", 0, 2))
    for enc in ((3, 1) if fmt4 else (4, 10)):
        cmap += struct.pack(">HHI", 3 if enc in (1, 10) else 0, enc, 20)
    if fmt4:
        cmap += struct.pack(">4H", 4, 16 + segs * 8, 0, segs * 2) + search_fields(segs, 2)
        cmap += b"".join(struct.pack(">H", r[1]) for r in runs) + struct.pack(">HH", 0xFFFF, 0)
        cmap += b"".join(struct.pack(">H", r[0]) for r in runs) + struct.pack(">H", 0xFFFF)
        cmap += b"".join(struct.pack(">H", (r[2] - r[0]) & 0xFFFF) for r in runs) + struct.pack(">H", 1)
        cmap += bytes(segs * 2)
    else:
        cmap += struct.pack(">HHIII", 12, 0, 16 + len(runs) * 12, 0, len(runs))
        cmap += b"".join(struct.pack(">3I", *r) for r in runs)

    # OS/2 script coverage: (first, last, unicode range bit, code page bit or -1).
    scripts = [(0x0020, 0x007E, 0, -1), (0x00A0, 0x00FF, 1, 0), (0x0100, 0x017F, 2, 1),
               (0x0370, 0x03FF, 7, 3), (0x0400, 0x04FF, 9, 2), (0x1100, 0x11FF, 28, -1),
               (0x2000, 0x206F, 31, -1), (0x3000, 0x303F, 48, -1), (0x3040, 0x309F, 49, 17),
               (0x30A0, 0x30FF, 50, 17), (0x4E00, 0x9FFF, 59, 18), (0xAC00, 0xD7AF, 56, 19),
               (0xFF00, 0xFFEF, 68, -1)]
    uni_range, cp_range = [0, 0, 0, 0], 0
    for lo, hi, uni, cp in scripts:
        if not any(c in present for c in range(lo, hi + 1)):
            continue
        uni_range[uni // 32] |= 1 << (uni % 32)
        if cp >= 0:
            cp_range |= 1 << cp
        if cp == 18:
            cp_range |= 1 << 20  # CJK ideographs serve both Chinese code pages
    win_ascent, win_descent = max(ascent, y_max), max(descent, -y_min)
    os2 = struct.pack(">HhHHH", 4, adv_sum // adv_count if adv_count else 0, 400, 5, 0)
    os2 += struct.pack(">8h", em * 65 // 100, em * 60 // 100, 0, em * 7 // 100,
                       em * 65 // 100, em * 60 // 100, 0, em * 35 // 100)
    os2 += struct.pack(">hhh", U, (x_height if x_height else ascent // 2) // 2, 0)
    os2 += bytes(10)  # PANOSE
    os2 += struct.pack(">4I", *uni_range) + b"NUTT"
    os2 += struct.pack(">HHH", 0xC0, runs[0][0] if runs else 0, runs[-1][1] if runs else 0)  # REGULAR | USE_TYPO_METRICS
    os2 += struct.pack(">hhhHH", ascent, -descent, 0, win_ascent, win_descent)
    os2 += struct.pack(">II", cp_range, 0)
    os2 += struct.pack(">hhHHH", x_height, cap_height, 0, ord(" "), 0)

    # Created and modified at 0 (1904), so every build makes the same file.
    head = struct.pack(">IIIIHHQQ4hHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x0009, em, 0, 0,
                       x_min, y_min, x_max, y_max, 0, lh, 2, 1, 0)
    hhea = struct.pack(">Ihhh", 0x00010000, ascent, -descent, 0) + struct.pack(">H", adv_max)
    hhea += struct.pack(">hhhh", min_lsb, min_rsb, max_extent, 1) + bytes(14) + struct.pack(">H", len(order))
    maxp = struct.pack(">IHHH", 0x00010000, len(order), max_points, max_contours) + bytes(4)
    maxp += struct.pack(">H", 2) + bytes(16)

    ps = "".join(c for c in FAMILY if c.isascii() and c.isalnum())[:55] + "-Regular"
    names = [(1, FAMILY), (2, "Regular"), (3, "reNut:" + FAMILY), (4, FAMILY), (5, "Version 1.000"), (6, ps)]
    name, strings = bytearray(struct.pack(">HHH", 0, len(names), 6 + 12 * len(names))), bytearray()
    for nid, text in names:
        s = text.encode("utf-16-be")
        name += struct.pack(">6H", 3, 1, 0x409, nid, len(s), len(strings))
        strings += s
    name += strings
    post = struct.pack(">IIhh", 0x00030000, 0, -2 * U, U) + bytes(20)

    tables = [(b"OS/2", os2), (b"cmap", bytes(cmap)), (b"glyf", bytes(glyf)), (b"head", head),
              (b"hhea", hhea), (b"hmtx", bytes(hmtx)), (b"loca", bytes(loca)), (b"maxp", maxp),
              (b"name", bytes(name)), (b"post", post)]
    out = bytearray(struct.pack(">IH", 0x00010000, len(tables)) + search_fields(len(tables), 16))
    offset, head_offset = 12 + 16 * len(tables), 0
    for tag, data in tables:
        out += tag + struct.pack(">III", checksum(data), offset, len(data))
        if tag == b"head":
            head_offset = offset
        offset += (len(data) + 3) & ~3
    for _, data in tables:
        out += data + bytes(-len(data) & 3)
    struct.pack_into(">I", out, head_offset + 8, (0xB1B0AFBA - checksum(bytes(out))) & 0xFFFFFFFF)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    game, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(game, CACHE), "rb") as f:
        line_height, cell, glyphs = parse(f.read())
    ttf = to_ttf(line_height, cell, glyphs)
    if "--ttf" in sys.argv:
        with open(sys.argv[sys.argv.index("--ttf") + 1], "wb") as f:
            f.write(ttf)
    import zstandard
    packed = zstandard.ZstdCompressor(level=19).compress(ttf)
    with open(out, "w", newline="\n") as f:
        f.write("// Generated by tools/make_launcher_font.py from xuifontcachefont/euro.sbp: the\n"
                "// launcher's font, a TrueType font of %d glyphs, zstd-compressed.\n" % len(glyphs))
        f.write("constexpr size_t kLauncherFontBytes = %d;\n" % len(ttf))
        f.write("constexpr unsigned char kLauncherFontZstd[] = {\n")
        for i in range(0, len(packed), 20):
            f.write("    " + ", ".join("0x%02X" % b for b in packed[i:i + 20]) + ",\n")
        f.write("};\n")
    print("%d glyphs, %d bytes, %d compressed" % (len(glyphs), len(ttf), len(packed)))


if __name__ == "__main__":
    main()
