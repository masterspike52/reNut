"""Xenos (Xbox 360) shader container + microcode parser.

Bit layouts follow rexglue-sdk include/rex/graphics/format/ucode.h.
Bitfields there are declared LSB-first over host-order dwords; the ucode
dwords themselves are big-endian in the blob.
"""
import struct


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def sbits(v, lo, n):
    x = bits(v, lo, n)
    return x - (1 << n) if x & (1 << (n - 1)) else x


# ---------------------------------------------------------------- container

CTAB_REGSET = {0: 'bool', 1: 'int4', 2: 'float4', 3: 'sampler'}
CTAB_CLASS = {0: 'scalar', 1: 'vector', 2: 'matrix_rows', 3: 'matrix_columns', 4: 'object', 5: 'struct'}
CTAB_TYPE = {0: 'void', 1: 'bool', 2: 'int', 3: 'float', 4: 'string', 5: 'texture', 6: 'texture1D',
             7: 'texture2D', 8: 'texture3D', 9: 'textureCUBE', 10: 'sampler', 11: 'sampler1D',
             12: 'sampler2D', 13: 'sampler3D', 14: 'samplerCUBE'}

DECL_USAGE = ['POSITION', 'BLENDWEIGHT', 'BLENDINDICES', 'NORMAL', 'PSIZE', 'TEXCOORD', 'TANGENT',
              'BINORMAL', 'TESSFACTOR', 'POSITIONT', 'COLOR', 'FOG', 'DEPTH', 'SAMPLE', 'USAGE14', 'USAGE15']


class CtabType:
    def __init__(self, cls, typ, rows, cols, elements, members):
        self.cls, self.typ, self.rows, self.cols, self.elements, self.members = cls, typ, rows, cols, elements, members


class CtabConstant:
    def __init__(self, name, regset, index, count, ctype, default):
        self.name, self.regset, self.index, self.count, self.type, self.default = name, regset, index, count, ctype, default


def _cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin-1')


def parse_ctab(b, base):
    """D3DXSHADER_CONSTANTTABLE, big-endian. Offsets are relative to base."""
    u32 = lambda o: struct.unpack_from('>I', b, base + o)[0]
    u16 = lambda o: struct.unpack_from('>H', b, base + o)[0]
    size, creator, version, count, cinfo, flags, target = [u32(i * 4) for i in range(7)]
    info = {'creator': _cstr(b, base + creator) if creator else '',
            'target': _cstr(b, base + target) if target else '', 'constants': []}

    def read_type(off):
        t = CtabType(u16(off), u16(off + 2), u16(off + 4), u16(off + 6), u16(off + 8), [])
        nmem, moff = u16(off + 10), u32(off + 12)
        for m in range(nmem):
            mo = moff + m * 8
            t.members.append((_cstr(b, base + u32(mo)), read_type(u32(mo + 4))))
        return t

    for i in range(count):
        o = cinfo + i * 20
        name = _cstr(b, base + u32(o))
        regset, index, rcount = u16(o + 4), u16(o + 6), u16(o + 8)
        ctype = read_type(u32(o + 12))
        dflt = u32(o + 16)
        default = None
        if dflt and regset == 2:
            default = [struct.unpack_from('>f', b, base + dflt + 4 * k)[0] for k in range(rcount * 4)]
        info['constants'].append(CtabConstant(name, regset, index, rcount, ctype, default))
    return info


class Container:
    def __init__(self, blob):
        b = self.blob = blob
        u32 = lambda o: struct.unpack_from('>I', b, o)[0]
        self.u32 = u32
        self.flags, self.virtual_size, self.physical_size, _, ct, dt, so = [u32(i * 4) for i in range(7)]
        if self.flags & 0xFFFFFF00 != 0x102A1100:
            raise ValueError('bad magic %08x' % self.flags)
        self.is_vertex = bool(self.flags & 1)
        self.phys = b[self.virtual_size:self.virtual_size + self.physical_size]

        self.ctab = parse_ctab(b, ct + 4) if ct else None

        # Shader struct.
        s = [u32(so + 4 * i) for i in range(8)]
        self.phys_offset, self.ucode_size = s[0], s[1]
        self.shader_words = [u32(o) for o in range(so, self.virtual_size, 4)]
        ucode = self.phys[self.phys_offset:self.phys_offset + self.ucode_size]
        self.ucode = list(struct.unpack('>%dI' % (len(ucode) // 4), ucode))

        # Literal (def) constants: entries of (u16 register, u16 float count, u32 physical offset).
        self.defs_float = {}   # register -> [x, y, z, w]
        self.def_header = []
        self.def_extra = []
        self.defs_int = {}
        if dt:
            self.def_header = [u32(dt + 4 * i) for i in range(4)]
            size = u32(dt + 16)
            o = dt + 20
            end = o + size - 4
            self.def_extra = []
            while o + 8 <= end:
                reg, nfloats, poff = struct.unpack_from('>HHI', b, o)
                if nfloats == 0 or nfloats % 4 or poff + 4 * nfloats > len(self.phys):
                    # Not a float definition (the integer/loop definitions follow); keep raw.
                    self.def_extra = [u32(x) for x in range(o, end, 4)]
                    break
                o += 8
                vals = struct.unpack_from('>%df' % nfloats, self.phys, poff)
                is_ps = not self.is_vertex
                base_reg = reg - 256 if is_ps and reg >= 256 else reg
                for k in range(nfloats // 4):
                    self.defs_float[base_reg + k] = list(vals[4 * k:4 * k + 4])
            # Integer (loop) definitions: 0, then (u16 0x2320 + 4 * register, u16 count), values.
            self.defs_int = {}  # Xenos loop register -> (count, start, step)
            x = self.def_extra
            if len(x) >= 2 and x[0] == 0 and (x[1] >> 16) >= 0x2320:
                reg = ((x[1] >> 16) - 0x2320) // 4
                for k, v in enumerate(x[2:2 + (x[1] & 0xFFFF)]):
                    self.defs_int[reg + k] = (v & 0xFF, (v >> 8) & 0xFF, sbits(v, 16, 8))

        # Vertex elements / interpolators.
        self.vertex_elements = []  # (instruction address, usage, usage index, extra)
        self.interpolators = []    # (register, usage, usage index, mask)
        w = self.shader_words
        if self.is_vertex:
            # w[8] is the length of the trailing instruction address list, not interpolators.
            n_elem, n_interp = w[7], w[5] & 0x1F
            # w[6] words (normally just 0x290) precede the vertex element list.
            self.shader_prefix = w[9:9 + w[6]]
            lst = w[9 + w[6]:]
            for e in lst[:n_elem]:
                self.vertex_elements.append((e & 0xFFF, bits(e, 12, 4), bits(e, 16, 4), e >> 20))
            for e in lst[n_elem:n_elem + n_interp]:
                self.interpolators.append((bits(e, 8, 4), bits(e, 4, 4), bits(e, 0, 4), bits(e, 12, 4)))
            self.shader_tail = lst[n_elem + n_interp:]
        else:
            # Pixel parameters (VPOS / VFACE) are written to register (w[3] >> 8) when w[3] & 0xFF == 6.
            self.param_gen_reg = (w[3] >> 8) & 0xFF if (w[3] & 0xFF) == 6 else None
            n_interp = w[5] & 0x1F
            for e in w[8:8 + n_interp]:
                self.interpolators.append((bits(e, 8, 4), bits(e, 4, 4), bits(e, 0, 4), bits(e, 12, 4)))
            self.shader_tail = w[8 + n_interp:]


# ---------------------------------------------------------------- control flow

CF_NAMES = ['nop', 'exec', 'exece', 'cexec', 'cexece', 'pexec', 'pexece', 'loop', 'endloop',
            'ccall', 'ret', 'cjmp', 'alloc', 'pexec_clean', 'pexece_clean', 'mark_vs_fetch_done']


class CF:
    def __init__(self, index, d0, d1):
        self.index, self.d0, self.d1 = index, d0, d1
        self.op = bits(d1, 12, 4)
        self.address_mode = bits(d1, 11, 1)
        op = self.op
        if op in (1, 2, 3, 4, 5, 6, 13, 14):
            self.address = bits(d0, 0, 12)
            self.count = bits(d0, 12, 3)
            self.is_yield = bits(d0, 15, 1)
            self.sequence = bits(d0, 16, 12)
            self.is_end = op in (2, 4, 6, 14)
            # 13/14 (cond exec, predicate clean) test a bool constant like 3/4 (xenia translator.cpp).
            if op in (3, 4, 13, 14):
                self.kind = 'cond'
                self.bool_address = bits(d1, 2, 8)
                self.condition = bits(d1, 10, 1)
            elif op in (5, 6):
                self.kind = 'pred'
                self.condition = bits(d1, 10, 1)
            else:
                self.kind = 'exec'
        elif op == 7:
            self.address = bits(d0, 0, 13)
            self.is_repeat = bits(d0, 13, 1)
            self.loop_id = bits(d0, 16, 5)
        elif op == 8:
            self.address = bits(d0, 0, 13)
            self.loop_id = bits(d0, 16, 5)
            self.is_predicated_break = bits(d0, 21, 1)
            self.condition = bits(d1, 10, 1)
        elif op in (9, 11):
            self.address = bits(d0, 0, 13)
            self.is_unconditional = bits(d0, 13, 1)
            self.is_predicated = bits(d0, 14, 1)
            self.bool_address = bits(d1, 2, 8)
            self.condition = bits(d1, 10, 1)
        elif op == 12:
            self.size = bits(d0, 0, 3)
            self.alloc_type = bits(d1, 9, 2)

    @property
    def name(self):
        return CF_NAMES[self.op]


def parse_control_flow(ucode):
    cfs = []
    max_cf = len(ucode) // 3
    i = 0
    while i < max_cf:
        w0, w1, w2 = ucode[i * 3:i * 3 + 3]
        a = CF(len(cfs), w0, w1 & 0xFFFF)
        b = CF(len(cfs) + 1, ((w1 >> 16) | (w2 << 16)) & 0xFFFFFFFF, w2 >> 16)
        for cf in (a, b):
            if cf.op in (1, 2, 3, 4, 5, 6, 13, 14) and cf.count:
                max_cf = min(max_cf, cf.address)
        cfs += [a, b]
        i += 1
    return cfs


# ---------------------------------------------------------------- instructions

class Fetch:
    """Vertex or texture fetch instruction (3 dwords)."""

    def __init__(self, w0, w1, w2):
        self.words = (w0, w1, w2)
        self.opcode = bits(w0, 0, 5)
        self.src_reg = bits(w0, 5, 6)
        self.src_rel = bits(w0, 11, 1)
        self.dst_reg = bits(w0, 12, 6)
        self.dst_rel = bits(w0, 18, 1)
        self.dst_swiz = bits(w1, 0, 12)
        self.is_predicated = bits(w1, 31, 1)
        self.pred_condition = bits(w2, 31, 1)
        if self.opcode == 0:  # vertex fetch
            self.const_index = bits(w0, 20, 5) * 3 + bits(w0, 25, 2)
            self.prefetch_count = bits(w0, 27, 3)
            self.src_swiz = bits(w0, 30, 2)
            self.is_signed = bits(w1, 12, 1)
            self.is_integer = bits(w1, 13, 1)
            self.format = bits(w1, 16, 6)
            self.exp_adjust = sbits(w1, 24, 6)
            self.is_mini = bits(w1, 30, 1)
            self.stride = bits(w2, 0, 8)
            self.offset = sbits(w2, 8, 23)
        else:
            self.fetch_valid_only = bits(w0, 19, 1)
            self.const_index = bits(w0, 20, 5)
            self.unnormalized = bits(w0, 25, 1)
            self.src_swiz = bits(w0, 26, 6)
            self.mag_filter = bits(w1, 12, 2)
            self.min_filter = bits(w1, 14, 2)
            self.mip_filter = bits(w1, 16, 2)
            self.aniso_filter = bits(w1, 18, 3)
            self.vol_mag_filter = bits(w1, 24, 2)
            self.vol_min_filter = bits(w1, 26, 2)
            self.use_comp_lod = bits(w1, 28, 1)
            self.use_reg_lod = bits(w1, 29, 1)
            self.use_reg_gradients = bits(w2, 0, 1)
            self.sample_location = bits(w2, 1, 1)
            self.lod_bias = sbits(w2, 2, 7) / 16.0
            self.dimension = bits(w2, 14, 2)
            self.offset_xyz = (sbits(w2, 16, 5) * 0.5, sbits(w2, 21, 5) * 0.5, sbits(w2, 26, 5) * 0.5)


class Alu:
    def __init__(self, w0, w1, w2):
        self.words = (w0, w1, w2)
        self.vector_dest = bits(w0, 0, 6)
        self.vector_dest_rel = bits(w0, 6, 1)
        self.abs_constants = bits(w0, 7, 1)
        self.scalar_dest = bits(w0, 8, 6)
        self.scalar_dest_rel = bits(w0, 14, 1)
        self.export = bits(w0, 15, 1)
        self.vector_mask = bits(w0, 16, 4)
        self.scalar_mask = bits(w0, 20, 4)
        self.vector_clamp = bits(w0, 24, 1)
        self.scalar_clamp = bits(w0, 25, 1)
        self.scalar_opc = bits(w0, 26, 6)

        self.src_swiz = {3: bits(w1, 0, 8), 2: bits(w1, 8, 8), 1: bits(w1, 16, 8)}
        self.src_negate = {3: bits(w1, 24, 1), 2: bits(w1, 25, 1), 1: bits(w1, 26, 1)}
        self.pred_condition = bits(w1, 27, 1)
        self.is_predicated = bits(w1, 28, 1)
        self.const_ar_relative = bits(w1, 29, 1)
        self.const_1_rel = bits(w1, 30, 1)
        self.const_0_rel = bits(w1, 31, 1)

        self.src_reg = {3: bits(w2, 0, 8), 2: bits(w2, 8, 8), 1: bits(w2, 16, 8)}
        self.vector_opc = bits(w2, 24, 5)
        self.src_sel = {3: bits(w2, 29, 1), 2: bits(w2, 30, 1), 1: bits(w2, 31, 1)}

    def src_const_addressed(self, i):
        if i == 1:
            return self.const_0_rel
        if i == 2:
            return self.const_0_rel if self.src_sel[1] else self.const_1_rel
        return self.const_0_rel if (self.src_sel[1] and self.src_sel[2]) else self.const_1_rel

    # Write masks per ucode.h helpers.
    def vector_result_mask(self):
        m = self.vector_mask
        if self.export:
            m &= ~self.scalar_mask
        return m & 0xF

    def scalar_result_mask(self):
        m = self.scalar_mask
        if self.export:
            m &= ~self.vector_mask
        return m & 0xF

    def const0_mask(self):
        if not self.export or not self.scalar_dest_rel:
            return 0
        return 0xF & ~(self.vector_mask | self.scalar_mask)

    def const1_mask(self):
        if not self.export:
            return 0
        return self.vector_mask & self.scalar_mask


SCALAR_OPS = [
    # name, operand count, two-component
    ('adds', 1, True), ('adds_prev', 1, False), ('muls', 1, True), ('muls_prev', 1, False),
    ('muls_prev2', 1, True), ('maxs', 1, True), ('mins', 1, True), ('seqs', 1, False),
    ('sgts', 1, False), ('sges', 1, False), ('snes', 1, False), ('frcs', 1, False),
    ('truncs', 1, False), ('floors', 1, False), ('exp', 1, False), ('logc', 1, False),
    ('log', 1, False), ('rcpc', 1, False), ('rcpf', 1, False), ('rcp', 1, False),
    ('rsqc', 1, False), ('rsqf', 1, False), ('rsq', 1, False), ('maxas', 1, True),
    ('maxasf', 1, True), ('subs', 1, True), ('subs_prev', 1, False), ('setp_eq', 1, False),
    ('setp_ne', 1, False), ('setp_gt', 1, False), ('setp_ge', 1, False), ('setp_inv', 1, False),
    ('setp_pop', 1, False), ('setp_clr', 0, False), ('setp_rstr', 1, False), ('kills_eq', 1, False),
    ('kills_gt', 1, False), ('kills_ge', 1, False), ('kills_ne', 1, False), ('kills_one', 1, False),
    ('sqrt', 1, False), ('opcode_41', 0, False), ('mulsc', 2, False), ('mulsc', 2, False),
    ('addsc', 2, False), ('addsc', 2, False), ('subsc', 2, False), ('subsc', 2, False),
    ('sin', 1, False), ('cos', 1, False), ('retain_prev', 0, False),
] + [('opcode_%d' % i, 0, False) for i in range(51, 64)]

VECTOR_OPS = [
    ('add', 2), ('mul', 2), ('max', 2), ('min', 2), ('seq', 2), ('sgt', 2), ('sge', 2), ('sne', 2),
    ('frc', 1), ('trunc', 1), ('floor', 1), ('mad', 3), ('cndeq', 3), ('cndge', 3), ('cndgt', 3),
    ('dp4', 2), ('dp3', 2), ('dp2add', 3), ('cube', 2), ('max4', 1), ('setp_eq_push', 2),
    ('setp_ne_push', 2), ('setp_gt_push', 2), ('setp_ge_push', 2), ('kill_eq', 2), ('kill_gt', 2),
    ('kill_ge', 2), ('kill_ne', 2), ('dst', 2), ('maxa', 2), ('opcode_30', 0), ('opcode_31', 0),
]

VERTEX_FORMATS = {0: ('undefined', 4), 6: ('8_8_8_8', 4), 7: ('2_10_10_10', 4), 16: ('10_11_11', 3),
                  17: ('11_11_10', 3), 25: ('16_16', 2), 26: ('16_16_16_16', 4),
                  31: ('16_16_FLOAT', 2), 32: ('16_16_16_16_FLOAT', 4), 33: ('32', 1),
                  34: ('32_32', 2), 35: ('32_32_32_32', 4), 36: ('32_FLOAT', 1),
                  37: ('32_32_FLOAT', 2), 38: ('32_32_32_32_FLOAT', 4), 57: ('32_32_32_FLOAT', 3)}


def iter_exec_instructions(ucode, cf):
    seq = cf.sequence
    for addr in range(cf.address, cf.address + cf.count):
        w = ucode[addr * 3:addr * 3 + 3]
        if len(w) < 3:
            break
        if seq & 1:
            yield addr, Fetch(*w)
        else:
            yield addr, Alu(*w)
        seq >>= 2
