#!/usr/bin/env python3
"""Builds reNut's built-in pipeline list (gpu/render/pipeline_cache_data.inc).

The renderer builds the listed pipelines on worker threads once the game has
made their shaders, so a draw seldom compiles one on the render thread. The
list is put together from:
- pipeline captures: pipeline_capture_<time>.txt, written next to renut with
  renut_pipeline_capture on; they hold only the pipelines draws had to build
  themselves (none the built-in list has);
- pipeline_list.bin files of older builds (format 'RNPL' version 2);
- the list already in the output file, which is kept.
Inputs may be files or folders (searched for both kinds). The pipelines are
de-duplicated and sorted, and their vertex layouts shared.

usage: python pipeline_cache.py <out.inc> [capture or folder]...
"""
import os
import re
import struct
import sys

STREAMS = 16
TARGETS = 4

# PipelineKey's fields after the two shaders and the layout hash, in order;
# (name, count, hex).
KEY_FIELDS = [
    ("topology", 1, False),
    ("color_formats", TARGETS, False),
    ("depth_format", 1, False),
    ("color_count", 1, False),
    ("samples", 1, False),
    ("blend_control", TARGETS, True),
    ("color_mask", 1, True),
    ("depth_control", 1, True),
    ("stencil_ref_mask", 1, True),
    ("stencil_ref_mask_bf", 1, True),
    ("mode_control", 1, True),
    ("depth_bias", 1, False),
    ("depth_bias_clamp", 1, False),
    ("alpha_to_coverage", 1, False),
    ("per_instance_streams", 1, False),
    ("rectangle_list", 1, False),
    ("point_sprite", 1, False),
]


# A pipeline: (vs hash, ps hash, layout, key values), the key values a tuple
# in KEY_FIELDS order (arrays as tuples); a layout: (layout hash, used streams
# mask, strides tuple, elements tuple of (name, index, location, format, slot,
# offset)).


def legacy_list(path):
    data = open(path, "rb").read()
    if len(data) < 16 or data[:4] != b"RNPL":
        raise ValueError(f"{path}: not a pipeline list")
    version, key_size, streams = struct.unpack_from("<3I", data, 4)
    if version != 2 or key_size != 120 or streams != STREAMS:
        raise ValueError(f"{path}: pipeline list version {version} (key {key_size} bytes, "
                         f"{streams} streams): only version 2 (120, 16) is read")
    key = struct.Struct("<3QI4I3I4I5IifI3I4x")
    assert key.size == 120
    at = 16
    out = []
    while len(data) - at >= 4:
        (size,) = struct.unpack_from("<I", data, at)
        at += 4
        if len(data) - at < size:
            break
        record = data[at:at + size]
        at += size
        v = key.unpack_from(record, 0)
        vs, ps, layout_hash = v[0], v[1], v[2]
        values = (v[3], tuple(v[4:8]), v[8], v[9], v[10], tuple(v[11:15]), v[15], v[16], v[17],
                  v[18], v[19], v[20], fix_float(v[21]), v[22], v[23], v[24], v[25])
        r = key.size
        used, strides = 0, []
        for s in range(STREAMS):
            u, stride = struct.unpack_from("<BI", record, r)
            r += 5
            used |= (1 << s) if u else 0
            strides.append(stride)
        (count,) = struct.unpack_from("<I", record, r)
        r += 4
        elements = []
        for _ in range(count):
            length = record[r]
            name = record[r + 1:r + 1 + length].decode("ascii")
            r += 1 + length
            elements.append((name,) + struct.unpack_from("<5I", record, r))
            r += 20
        out.append((vs, ps, (layout_hash, used, tuple(strides), tuple(elements)), values))
    return out


def fix_float(value):
    # As float32 round-trips through its 9 significant digits.
    return float(f"{value:.9g}")


def capture(path):
    out = []
    for line in open(path, encoding="ascii"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = dict(item.split("=", 1) for item in line.split())
        def number(text):
            return float(text) if any(c in text for c in ".en") and not text.startswith("0x") \
                else int(text, 0)
        def get(name, count):
            text = fields.get(name, "0" if count == 1 else ",".join(["0"] * count))
            values = [number(t) for t in text.split(",")]
            if len(values) != count:
                raise ValueError(f"{path}: {name} has {len(values)} values, not {count}")
            return values[0] if count == 1 else tuple(values)
        values = tuple(get(name, count) for name, count, _ in KEY_FIELDS)
        index = [n for n, _, _ in KEY_FIELDS].index("depth_bias_clamp")
        values = values[:index] + (fix_float(float(values[index])),) + values[index + 1:]
        strides = tuple(int(t) for t in fields["strides"].split(","))
        elements = []
        if fields.get("elements"):
            for item in fields["elements"].split(","):
                name, *numbers = item.split(":")
                elements.append((name,) + tuple(int(t) for t in numbers))
        layout = (int(fields["layout"], 0), int(fields["used"], 0), strides, tuple(elements))
        out.append((int(fields["vs"], 0), int(fields["ps"], 0), layout, values))
    return out


TOKEN = re.compile(r'\s*(?:("[^"]*")|([{},])|(0x[0-9A-Fa-f]+|[-+]?[0-9.]+(?:e[-+]?\d+)?)f?)')


def parse_entry(text):
    """One generated initializer ({...}) as nested lists."""
    tokens = []
    at = 0
    text = text.strip().rstrip(",")
    while at < len(text):
        m = TOKEN.match(text, at)
        if not m or m.end() == at:
            raise ValueError(f"cannot parse: {text[at:at + 40]}")
        at = m.end()
        if m.group(1):
            tokens.append(m.group(1)[1:-1])
        elif m.group(2):
            tokens.append(m.group(2))
        else:
            t = m.group(3)
            tokens.append(int(t, 0) if t.startswith("0x") or re.fullmatch(r"-?\d+", t) else float(t))
    pos = 0
    def value():
        nonlocal pos
        t = tokens[pos]
        pos += 1
        if t != "{":
            return t
        items = []
        while tokens[pos] != "}":
            items.append(value())
            if tokens[pos] == ",":
                pos += 1
        pos += 1
        return items
    return value()


def built_in(path):
    """The list already in a generated file."""
    sections = {}
    current = None
    for line in open(path, encoding="ascii"):
        m = re.match(r"const BuiltIn(\w+) kBuiltIn\w+\[\] = \{", line)
        if m:
            current = sections.setdefault(m.group(1), [])
        elif line.startswith("};"):
            current = None
        elif current is not None and line.strip().startswith("{") and line.strip() != "{},":
            current.append(parse_entry(line))
    elements = [tuple(e) for e in sections.get("Element", [])]
    layouts = []
    for first, count, layout_hash, used, strides in sections.get("Layout", []):
        strides = tuple(strides) + (0,) * (STREAMS - len(strides))
        layouts.append((layout_hash, used, strides, tuple(elements[first:first + count])))
    out = []
    for entry in sections.get("Pipeline", []):
        vs, ps, layout = entry[0], entry[1], layouts[entry[2]]
        values = []
        for (name, count, _), v in zip(KEY_FIELDS, entry[3:]):
            if count > 1:
                v = tuple(v) + (0,) * (count - len(v))
            elif name == "depth_bias_clamp":
                v = fix_float(float(v))
            values.append(v)
        out.append((vs, ps, layout, tuple(values)))
    return out


def trimmed(values):
    values = list(values)
    while len(values) > 1 and values[-1] == 0:
        values.pop()
    return values


def float_text(v):
    if v == 0:
        return "0"
    text = f"{v:.9g}"
    if not any(c in text for c in ".e"):
        text += ".0"
    return text + "f"


def write(path, pipelines):
    layouts = sorted({p[2] for p in pipelines}, key=lambda l: (l[0], l[1], l[2], l[3]))
    index = {l: i for i, l in enumerate(layouts)}
    pipelines = sorted(set(pipelines), key=lambda p: (p[0], p[1], index[p[2]], p[3]))
    lines = [
        "// Generated by tools/pipeline_cache.py from pipeline captures: do not edit.",
        f"// {len(pipelines)} pipelines, {len(layouts)} vertex layouts.",
        "",
        "// Elements: semantic name, semantic index, location, format, slot, offset.",
        "const BuiltInElement kBuiltInElements[] = {",
    ]
    first = []
    at = 0
    for layout in layouts:
        first.append(at)
        for name, sem, location, fmt, slot, offset in layout[3]:
            lines.append(f'    {{"{name}", {sem}, {location}, {fmt}, {slot}, {offset}}},')
            at += 1
    lines += [
        "    {},",
        "};",
        "",
        "// Layouts: first element, element count, layout hash, used streams (a bit",
        "// each), the streams' host strides.",
        "const BuiltInLayout kBuiltInLayouts[] = {",
    ]
    for i, (layout_hash, used, strides, elements) in enumerate(layouts):
        stride_text = ", ".join(str(s) for s in trimmed(strides))
        lines.append(f"    {{{first[i]}, {len(elements)}, 0x{layout_hash:016X}, 0x{used:04X}, "
                     f"{{{stride_text}}}}},")
    lines += [
        "    {},",
        "};",
        "",
        "// Pipelines: vertex and pixel shader cache hashes, layout, then the rest of",
        "// the PipelineKey in order.",
        "const BuiltInPipeline kBuiltInPipelines[] = {",
    ]
    for vs, ps, layout, values in pipelines:
        parts = [f"0x{vs:016X}", f"0x{ps:016X}", str(index[layout])]
        for (name, count, hex_), v in zip(KEY_FIELDS, values):
            fmt = (lambda x: f"0x{x:X}") if hex_ else str
            if name == "depth_bias_clamp":
                parts.append(float_text(v))
            elif count > 1:
                parts.append("{" + ", ".join(fmt(x) for x in trimmed(v)) + "}")
            else:
                parts.append(fmt(v))
        lines.append("    {" + ", ".join(parts) + "},")
    lines += ["    {},", "};", ""]
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))
    return len(pipelines), len(layouts)


def inputs(paths):
    for path in paths:
        if os.path.isdir(path):
            for root, _, names in os.walk(path):
                for name in sorted(names):
                    if (name.startswith("pipeline_capture_") and name.endswith(".txt")) or \
                            name == "pipeline_list.bin":
                        yield os.path.join(root, name)
        else:
            yield path


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    out = argv[0]
    pipelines = built_in(out) if os.path.exists(out) else []
    kept = len(pipelines)
    for path in inputs(argv[1:]):
        read = legacy_list(path) if path.endswith(".bin") else capture(path)
        print(f"{path}: {len(read)} pipelines")
        pipelines += read
    count, layouts = write(out, pipelines)
    print(f"{out}: {count} pipelines ({kept} kept), {layouts} vertex layouts")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
