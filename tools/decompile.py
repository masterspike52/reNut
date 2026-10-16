"""Decompile Xenos shader containers (.bin dumped by shader_dump.cpp) to D3D9/SM3 HLSL.

usage: python decompile.py <input dir> <output dir>

Writes <vs|ps>_<hash>.hlsl for every .bin; the shaders include xenos_common.hlsli.
Validate with: python compile_check.py <output dir>
"""
import glob
import re
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xenos import *  # noqa

XYZW = 'xyzw'
DIM_NAMES = ['1D', '2D', '3D', 'Cube']


def fmt_float(v):
    if v != v:
        return '(0.0 / 0.0)'
    if v in (float('inf'), float('-inf')):
        return ('-' if v < 0 else '') + 'XE_FLT_MAX'
    packed = struct.pack('>f', v)
    for prec in range(1, 10):
        s = '%.*g' % (prec, v)
        if struct.pack('>f', float(s)) == packed:
            break
    if 'e' not in s and '.' not in s:
        s += '.0'
    return s


def swizzled(swiz, i):
    return ((swiz >> (2 * i)) + i) & 3


def mask_str(mask):
    return ''.join(XYZW[i] for i in range(4) if mask & (1 << i))


def mask_picks(mask):
    return [i for i in range(4) if mask & (1 << i)]


class Operand:
    def __init__(self, kind, reg, rel=None, absolute=False, negate=False, comps=(0, 1, 2, 3), values=None):
        self.kind, self.reg, self.rel, self.abs, self.neg = kind, reg, rel, absolute, negate
        self.comps = list(comps)  # component index read for positions 0..3
        self.values = values


class Decompiler:
    def __init__(self, name, container):
        self.name = name
        self.c = container
        self.vs = container.is_vertex
        self.lines = []
        self.indent = 1
        self.temps = set()
        self.temp_array = False
        self.const_refs = set()      # (reg, rel)
        self.samplers = {}           # index -> dimension
        self.bools = set()
        self.ints = set()
        self.outputs = set()
        self.uses_ps = False
        self.uses_p0 = False
        self.uses_a0 = False
        self.uses_aL = False
        self.uses_lod = False
        self.texel_size = set()
        self.vfetch_attrs = {}       # addr -> (field, semantic or None if shared, instruction)
        self.vbuffers = set()        # fetch constants read as indexed vertex buffers
        self.sampler_extra = set()   # (index, dimension) fetched with a second dimension
        self.notes = []

        ctab = container.ctab
        self.ctab_float = []
        self.ctab_names = {'sampler': {}, 'bool': {}, 'int4': {}}
        if ctab:
            for k in ctab['constants']:
                rs = CTAB_REGSET.get(k.regset)
                if rs == 'float4':
                    self.ctab_float.append(k)
                elif rs in self.ctab_names:
                    for n in range(k.count):
                        self.ctab_names[rs][k.index + n] = (k.name if k.count == 1 else '%s[%d]' % (k.name, n), k)

    # ------------------------------------------------------------ helpers
    def emit(self, s=''):
        s = s.replace(' + (-', ' - (').replace(' + -', ' - ')
        s = re.sub(r' - \(([0-9][0-9.e+-]*)\)', r' - \1', s)
        self.lines.append('    ' * self.indent + s if s else '')

    def ctab_float_for(self, reg):
        for k in self.ctab_float:
            if k.index <= reg < k.index + k.count:
                return k
        return None

    def temp_name(self, reg, rel):
        if rel or self.temp_array:
            self.temp_array = True
            if rel:
                self.uses_aL = True
                return 'r[aL + %d]' % reg
            return 'r[%d]' % reg
        self.temps.add(reg)
        return 'r%d' % reg

    def const_name(self, reg, rel):
        if rel:
            k = self.ctab_float_for(reg)
            if k and k.count > 1 and not self.const_array:
                return '%s[%s + %d]' % (k.name, rel, reg - k.index) if reg != k.index else '%s[%s]' % (k.name, rel)
            return 'c[%s + %d]' % (rel, reg)
        if self.const_array:
            return 'c[%d]' % reg
        k = self.ctab_float_for(reg)
        if k:
            return k.name if k.count == 1 else '%s[%d]' % (k.name, reg - k.index)
        return 'c%d' % reg

    def render(self, op, picks, scalar_ok=True):
        comps = [op.comps[p] for p in picks]
        if op.kind == 'literal':
            vals = [op.values[ci] for ci in comps]
            if op.abs:
                vals = [abs(v) for v in vals]
            if op.neg:
                vals = [-v for v in vals]
            if len(vals) == 1 or (scalar_ok and all(v == vals[0] for v in vals)):
                s = fmt_float(vals[0])
                return '(%s)' % s if s.startswith('-') else s
            return 'float%d(%s)' % (len(vals), ', '.join(fmt_float(v) for v in vals))
        if op.kind == 'temp':
            base = self.temp_name(op.reg, op.rel)
        else:
            base = self.const_name(op.reg, op.rel)
            if op.rel == 'a0':
                self.uses_a0 = True
            elif op.rel == 'aL':
                self.uses_aL = True
        if not (len(comps) == 4 and comps == [0, 1, 2, 3]):
            base += '.' + ''.join(XYZW[ci] for ci in comps)
        if op.abs:
            base = 'abs(%s)' % base
        if op.neg:
            base = '-' + base
        return base

    # ------------------------------------------------------------ operands
    def alu_operand(self, ins, i):
        reg = ins.src_reg[i]
        swiz = ins.src_swiz[i]
        comps = [swizzled(swiz, j) for j in range(4)]
        neg = bool(ins.src_negate[i])
        if ins.src_sel[i]:
            return Operand('temp', reg & 0x3F, 'aL' if reg & 0x40 else None, bool(reg & 0x80), neg, comps)
        rel = None
        if ins.src_const_addressed(i):
            rel = 'a0' if ins.const_ar_relative else 'aL'
        return self.make_const(reg, rel, bool(ins.abs_constants), neg, comps)

    def make_const(self, reg, rel, absolute, neg, comps):
        if rel is None and reg in self.c.defs_float:
            return Operand('literal', reg, None, absolute, neg, comps, self.c.defs_float[reg])
        self.const_refs.add((reg, rel))
        return Operand('const', reg, rel, absolute, neg, comps)

    # ------------------------------------------------------------ pre-pass
    def prepass(self, cfs):
        self.const_array = False
        rel_refs = []
        for cf in cfs:
            if cf.op in (3, 4, 13, 14):
                self.bools.add(cf.bool_address)
            if cf.op not in (1, 2, 3, 4, 5, 6, 13, 14) or not cf.count:
                continue
            for addr, ins in iter_exec_instructions(self.c.ucode, cf):
                if isinstance(ins, Alu):
                    so = SCALAR_OPS[ins.scalar_opc][0]
                    if so in ('adds_prev', 'muls_prev', 'muls_prev2', 'subs_prev'):
                        self.uses_ps = True
                    if so == 'retain_prev' and (ins.scalar_result_mask() or ins.scalar_clamp):
                        self.uses_ps = True
                    for i in (1, 2, 3):
                        if not ins.src_sel[i] and ins.src_const_addressed(i):
                            rel_refs.append(ins.src_reg[i])
        for reg in rel_refs:
            k = self.ctab_float_for(reg)
            if not (k and k.count > 1):
                self.const_array = True

    # ------------------------------------------------------------ export / dest names
    def export_name(self, reg):
        if reg == 32:
            n = 'eA'
        elif 33 <= reg <= 37:
            n = 'eM%d' % (reg - 33)
        elif self.vs:
            n = 'oPos' if reg == 62 else 'oPts' if reg == 63 else 'o%d' % reg
        else:
            n = 'oDepth' if reg == 61 else 'oC%d' % reg
        self.outputs.add(n)
        return n

    def dest(self, name, mask):
        return name if mask == 0xF else '%s.%s' % (name, mask_str(mask))

    # ------------------------------------------------------------ ALU
    def emit_alu(self, ins):
        vname, vn = VECTOR_OPS[ins.vector_opc]
        sname, sn, s2 = SCALAR_OPS[ins.scalar_opc]
        stmts = []

        if ins.export:
            vdest = sdest = self.export_name(ins.vector_dest)
        else:
            vdest = self.temp_name(ins.vector_dest, ins.vector_dest_rel)
            sdest = self.temp_name(ins.scalar_dest, ins.scalar_dest_rel)
        vmask = ins.vector_result_mask()
        smask = ins.scalar_result_mask()

        # ---------------- scalar value
        s_expr = None
        s_reads = []
        if sname != 'retain_prev' and sn:
            if sn == 2:
                swiz = ins.src_swiz[3]
                neg = bool(ins.src_negate[3])
                absolute = bool(ins.abs_constants)
                rel = None
                if ins.src_const_addressed(3):
                    rel = 'a0' if ins.const_ar_relative else 'aL'
                cop = self.make_const(ins.src_reg[3], rel, absolute, neg, [swizzled(swiz, 3)] * 4)
                treg = (ins.scalar_opc & 1) | (ins.src_sel[3] << 1) | (swiz & 0x3C)
                top = Operand('temp', treg, None, absolute, neg, [swizzled(swiz, 0)] * 4)
                a, b = self.render(cop, [0]), self.render(top, [0])
                s_reads = [top]
            else:
                op3 = self.alu_operand(ins, 3)
                a = self.render(op3, [3])
                b = self.render(op3, [0])
                s_reads = [op3]
            s_expr, s_pre = self.scalar_expr(sname, a, b)
            stmts += s_pre
        elif sname == 'setp_clr':
            s_expr, s_pre = self.scalar_expr(sname, None, None)
            stmts += s_pre
        elif sname == 'retain_prev':
            s_expr = 'ps' if self.uses_ps else None

        # ---------------- vector value
        v_stmt = []
        const0, const1 = ins.const0_mask(), ins.const1_mask()
        vec_side = vname.startswith(('setp_', 'kill')) or vname == 'maxa'
        if vmask or vec_side:
            v_ops = [self.alu_operand(ins, i) for i in range(1, vn + 1)]
            v_stmt = self.vector_stmts(vname, v_ops, vdest, vmask, bool(ins.vector_clamp))

        # ---------------- order: scalar pre, (ps temp), vector write, scalar write
        s_needed = sname != 'retain_prev' or smask
        vector_writes_reg = vmask and not ins.export
        clobber = vector_writes_reg and any(
            o.kind == 'temp' and self.temp_name(o.reg, o.rel) == vdest for o in s_reads)
        if s_expr is not None and sname != 'retain_prev':
            if self.uses_ps or clobber:
                self.uses_ps = True
                stmts.append('ps = %s;' % s_expr)
                s_expr = 'ps'
            elif not smask:
                s_expr = None
        stmts += v_stmt
        if smask and s_expr is not None:
            val = 'saturate(%s)' % s_expr if ins.scalar_clamp else s_expr
            stmts.append('%s = %s;' % (self.dest(sdest, smask), val))
        if const0:
            stmts.append('%s = 0.0;' % self.dest(vdest, const0))
        if const1:
            stmts.append('%s = 1.0;' % self.dest(vdest, const1))

        if not stmts:
            return
        self.emit_predicated(ins, stmts)

    def emit_predicated(self, ins, stmts):
        if ins.is_predicated:
            self.uses_p0 = True
            header = 'if (%sp0)' % ('' if ins.pred_condition else '!')
            close = '    ' * self.indent + '}'
            if (getattr(self, 'last_pred', None) == (header, self.indent, len(self.lines))
                    and self.lines and self.lines[-1] == close):
                self.lines.pop()  # continue the previous block with the same predicate
            else:
                self.emit(header)
                self.emit('{')
            self.indent += 1
        for s in stmts:
            for line in s.split('\n'):
                self.emit(line)
        if ins.is_predicated:
            self.indent -= 1
            self.emit('}')
            sets_p0 = any(x.startswith('p0 =') for x in stmts)
            self.last_pred = None if sets_p0 else (header, self.indent, len(self.lines))

    def scalar_expr(self, name, a, b):
        pre = []
        e = {
            'adds': '%s + %s' % (a, b), 'adds_prev': '%s + ps' % a,
            'muls': '%s * %s' % (a, b), 'muls_prev': '%s * ps' % a,
            'muls_prev2': 'xe_muls_prev2(%s, %s, ps)' % (a, b),
            'maxs': a if a == b else 'max(%s, %s)' % (a, b), 'mins': a if a == b else 'min(%s, %s)' % (a, b),
            'seqs': '(%s == 0.0) ? 1.0 : 0.0' % a, 'sgts': '(%s > 0.0) ? 1.0 : 0.0' % a,
            'sges': '(%s >= 0.0) ? 1.0 : 0.0' % a, 'snes': '(%s != 0.0) ? 1.0 : 0.0' % a,
            'frcs': 'frac(%s)' % a, 'truncs': 'trunc(%s)' % a, 'floors': 'floor(%s)' % a,
            'exp': 'pow(2.0, %s)' % a, 'logc': 'xe_logc(%s)' % a, 'log': 'log2(%s)' % a,
            'rcpc': 'xe_rcpc(%s)' % a, 'rcpf': 'xe_rcpf(%s)' % a, 'rcp': '1.0 / %s' % a,
            'rsqc': 'xe_rsqc(%s)' % a, 'rsqf': 'xe_rsqf(%s)' % a, 'rsq': 'rsqrt(%s)' % a,
            'subs': '%s - %s' % (a, b), 'subs_prev': '%s - ps' % a,
            'sqrt': 'sqrt(%s)' % a, 'sin': 'sin(%s)' % a, 'cos': 'cos(%s)' % a,
            'mulsc': '%s * %s' % (a, b), 'addsc': '%s + %s' % (a, b), 'subsc': '%s - %s' % (a, b),
        }.get(name)
        if e is not None:
            return e, pre
        if name in ('maxas', 'maxasf'):
            self.uses_a0 = True
            pre.append('a0 = %s(%s);' % ('xe_mova' if name == 'maxas' else 'xe_mova_floor', a))
            return ('%s' % a if a == b else 'max(%s, %s)' % (a, b)), pre
        if name.startswith('setp_'):
            self.uses_p0 = True
            cond = {'setp_eq': '%s == 0.0', 'setp_ne': '%s != 0.0', 'setp_gt': '%s > 0.0',
                    'setp_ge': '%s >= 0.0', 'setp_inv': '%s == 1.0', 'setp_pop': '%s - 1.0 <= 0.0',
                    'setp_rstr': '%s == 0.0', 'setp_clr': None}[name]
            if cond is None:
                pre.append('p0 = false;')
                return 'XE_FLT_MAX', pre
            pre.append('p0 = (%s);' % (cond % a))
            other = {'setp_inv': '(%s == 0.0) ? 1.0 : %s' % (a, a), 'setp_pop': '%s - 1.0' % a,
                     'setp_rstr': a}.get(name, '1.0')
            return 'p0 ? 0.0 : %s' % ('(%s)' % other if ' ' in other else other), pre
        if name.startswith('kills_'):
            cond = {'kills_eq': '%s == 0.0', 'kills_gt': '%s > 0.0', 'kills_ge': '%s >= 0.0',
                    'kills_ne': '%s != 0.0', 'kills_one': '%s == 1.0'}[name] % a
            pre.append('if (%s) clip(-1.0);' % cond)
            return '(%s) ? 1.0 : 0.0' % cond, pre
        self.notes.append('unhandled scalar opcode %s' % name)
        return '0.0 /* %s */' % name, pre

    def vector_stmts(self, name, ops, dest, mask, clamp):
        picks = mask_picks(mask)
        sat = (lambda s: 'saturate(%s)' % s) if clamp else (lambda s: s)
        d = self.dest(dest, mask) if mask else None
        R = lambda i, p=None, scalar_ok=True: self.render(ops[i], picks if p is None else p, scalar_ok)
        out = []
        binop = {'add': '%s + %s', 'mul': '%s * %s', 'max': 'max(%s, %s)', 'min': 'min(%s, %s)',
                 'seq': '(%s == %s)', 'sgt': '(%s > %s)', 'sge': '(%s >= %s)', 'sne': '(%s != %s)'}
        n = len(picks)
        ftype = 'float' if n == 1 else 'float%d' % n
        if name in binop:
            a, b = R(0), R(1)
            if name == 'max' and a == b:
                e = a
            elif name in ('seq', 'sgt', 'sge', 'sne'):
                e = '%s(%s)' % (ftype, binop[name] % (a, b))
            else:
                e = binop[name] % (a, b)
            out.append('%s = %s;' % (d, sat(e)))
        elif name in ('frc', 'trunc', 'floor'):
            out.append('%s = %s;' % (d, sat('%s(%s)' % ({'frc': 'frac'}.get(name, name), R(0)))))
        elif name == 'mad':
            out.append('%s = %s;' % (d, sat('%s * %s + %s' % (R(0), R(1), R(2)))))
        elif name in ('cndeq', 'cndge', 'cndgt'):
            cmp = {'cndeq': '==', 'cndge': '>=', 'cndgt': '>'}[name]
            a = R(0, scalar_ok=False) if n > 1 else R(0)
            out.append('%s = %s;' % (d, sat('(%s %s 0.0) ? %s : %s' % (a, cmp, R(1), R(2)))))
        elif name == 'dp4':
            out.append('%s = %s;' % (d, sat('dot(%s, %s)' % (R(0, [0, 1, 2, 3], False), R(1, [0, 1, 2, 3], False)))))
        elif name == 'dp3':
            out.append('%s = %s;' % (d, sat('dot(%s, %s)' % (R(0, [0, 1, 2], False), R(1, [0, 1, 2], False)))))
        elif name == 'dp2add':
            out.append('%s = %s;' % (d, sat('dot(%s, %s) + %s' % (R(0, [0, 1], False), R(1, [0, 1], False), R(2, [0])))))
        elif name == 'cube':
            e = 'xe_cube(%s)' % R(0, [2, 3, 0], False)
            out.append('%s = %s;' % (d, sat(e if mask == 0xF else '%s.%s' % (e, mask_str(mask)))))
        elif name == 'max4':
            out.append('%s = %s;' % (d, sat('xe_max4(%s)' % R(0, [0, 1, 2, 3], False))))
        elif name == 'dst':
            e = 'float4(1.0, %s * %s, %s, %s)' % (R(0, [1]), R(1, [1]), R(0, [2]), R(1, [3]))
            out.append('%s = %s;' % (d, sat(e if mask == 0xF else '%s.%s' % (e, mask_str(mask)))))
        elif name == 'maxa':
            self.uses_a0 = True
            out.append('a0 = xe_mova(%s);' % R(0, [3]))
            if mask:
                a, b = R(0), R(1)
                out.append('%s = %s;' % (d, sat(a if a == b else 'max(%s, %s)' % (a, b))))
        elif name.startswith('setp_'):
            self.uses_p0 = True
            cmp = {'setp_eq_push': '==', 'setp_ne_push': '!=', 'setp_gt_push': '>', 'setp_ge_push': '>='}[name]
            out.append('p0 = (%s == 0.0 && %s %s 0.0);' % (R(0, [3]), R(1, [3]), cmp))
            if mask:
                ax = R(0, [0])
                out.append('%s = %s;' % (d, sat('(%s == 0.0 && %s %s 0.0) ? 0.0 : %s + 1.0' % (ax, R(1, [0]), cmp, ax))))
        elif name.startswith('kill_'):
            cmp = {'kill_eq': '==', 'kill_gt': '>', 'kill_ge': '>=', 'kill_ne': '!='}[name]
            cond = 'any(%s %s %s)' % (R(0, [0, 1, 2, 3], False), cmp, R(1, [0, 1, 2, 3], False))
            out.append('if (%s) clip(-1.0);' % cond)
            if mask:
                out.append('%s = %s ? 1.0 : 0.0;' % (d, cond))
        else:
            self.notes.append('unhandled vector opcode %s' % name)
            out.append('// unhandled vector opcode %s' % name)
        return out

    # ------------------------------------------------------------ fetch
    def fetch_dest_stmts(self, dest, swiz, value_expr):
        """Map a fetch result through the 3-bits-per-component destination swizzle."""
        src_mask, src_comps, zero_mask, one_mask = 0, [], 0, 0
        for i in range(4):
            s = (swiz >> (3 * i)) & 7
            if s <= 3:
                src_mask |= 1 << i
                src_comps.append(XYZW[s])
            elif s == 5:
                one_mask |= 1 << i
            elif s in (4, 6):
                zero_mask |= 1 << i
        out = []
        if src_mask:
            sw = ''.join(src_comps)
            rhs = value_expr if (src_mask == 0xF and sw == 'xyzw') else '%s.%s' % (value_expr, sw)
            out.append('%s = %s;' % (self.dest(dest, src_mask), rhs))
        if zero_mask:
            out.append('%s = 0.0;' % self.dest(dest, zero_mask))
        if one_mask:
            out.append('%s = 1.0;' % self.dest(dest, one_mask))
        return out

    def emit_fetch(self, addr, ins):
        if ins.opcode == 0:
            dest = self.temp_name(ins.dst_reg, ins.dst_rel)
            has_element = any(e[0] == addr for e in self.c.vertex_elements)
            if not has_element and (ins.src_reg != 0 or ins.src_rel):
                # Indexed read from a vertex buffer (e.g. bone / instance matrices), not a
                # vertex stream attribute: the index is a computed register.
                idx = self.render(Operand('temp', ins.src_reg, 'aL' if ins.src_rel else None,
                                          comps=[ins.src_swiz & 3] * 4), [0])
                self.vbuffers.add(ins.const_index)
                fmt = VERTEX_FORMATS.get(ins.format, ('format %d' % ins.format, 4))[0]
                call = 'xe_vfetch(xe_vfetch_heap, xe_vfetch_heap_Size, vfetch_c%d_Base, %s, %d.0, %d.0)' % (
                    ins.const_index, idx, ins.offset, ins.stride)
                stmts = self.fetch_dest_stmts(dest, ins.dst_swiz, call)
                if fmt != '32_32_32_32_FLOAT':
                    stmts[0] += ' // source format %s%s' % (fmt, ', signed' if ins.is_signed else '')
                self.emit_predicated(ins, stmts)
                return
            field = self.vertex_attr(addr, ins)
            self.emit_predicated(ins, self.fetch_dest_stmts(dest, ins.dst_swiz, 'In.%s' % field))
            return
        if ins.opcode == 17:  # getCompTexLOD: the LOD the hardware would pick for this fetch
            dim = ins.dimension
            ncomp = [1, 2, 3, 3][dim]
            comps = [(ins.src_swiz >> (2 * j)) & 3 for j in range(3)]
            src = Operand('temp', ins.src_reg, 'aL' if ins.src_rel else None, comps=comps + [0])
            coord = self.render(src, list(range(ncomp)), False)
            s = ins.const_index
            self.samplers.setdefault(s, dim)
            self.texel_size.add(s)
            ts = '%s_TexelSize.%s' % (self.sampler_ident(s), 'xyz'[:ncomp])
            call = 'xe_comp_lod(%s, %s)' % (coord, ts)
            dest = self.temp_name(ins.dst_reg, ins.dst_rel)
            self.emit_predicated(ins, self.fetch_dest_stmts(dest, ins.dst_swiz, 'float4(%s, 0.0, 0.0, 0.0)' % call))
            return
        if ins.opcode == 24:
            self.uses_lod = True
            src = Operand('temp', ins.src_reg, 'aL' if ins.src_rel else None, comps=[ins.src_swiz & 3] * 4)
            self.emit_predicated(ins, ['xe_lod = %s;' % self.render(src, [0])])
            return
        if ins.opcode != 1:
            self.notes.append('unhandled fetch opcode %d at %d' % (ins.opcode, addr))
            self.emit('// unhandled fetch opcode %d' % ins.opcode)
            return
        dim = ins.dimension
        ncomp = [1, 2, 3, 3][dim]
        comps = [(ins.src_swiz >> (2 * j)) & 3 for j in range(3)]
        src = Operand('temp', ins.src_reg, 'aL' if ins.src_rel else None, comps=comps + [0])
        coord = self.render(src, list(range(ncomp)), False)
        s = ins.const_index
        prev = self.samplers.setdefault(s, dim)
        sname = self.sampler_name(s)
        if prev != dim:
            # Same fetch constant sampled with another dimension: SM3 needs a sampler per type.
            if (s, dim) not in self.sampler_extra:
                self.notes.append('fetch constant %d sampled as both %s and %s' % (s, DIM_NAMES[prev], DIM_NAMES[dim]))
            self.sampler_extra.add((s, dim))
            sname = '%s_%s' % (sname.replace('[', '_').replace(']', ''), DIM_NAMES[dim])
        fn = ['tex1D', 'tex2D', 'tex3D', 'texCUBE'][dim]
        if any(ins.offset_xyz[:ncomp]) and dim != 3:
            self.texel_size.add(s)
            off = ins.offset_xyz[:ncomp]
            offs = fmt_float(off[0]) if ncomp == 1 else 'float%d(%s)' % (ncomp, ', '.join(fmt_float(o) for o in off))
            ts = '%s_TexelSize.%s' % (self.sampler_ident(s), 'xyz'[:ncomp])
            coord = '%s + %s * %s' % (coord, offs, ts)
        if dim == 3:
            coord = 'xe_cube_dir(%s)' % coord
        if ins.use_reg_lod:
            self.uses_lod = True
            if dim == 1:
                call = 'tex2Dlod(%s, float4(%s, 0.0, xe_lod))' % (sname, coord)
            elif dim == 0:
                call = 'tex1Dlod(%s, float4(%s, 0.0, 0.0, xe_lod))' % (sname, coord)
            else:
                call = '%slod(%s, float4(%s, xe_lod))' % (fn, sname, coord)
        elif ins.lod_bias:
            b = fmt_float(ins.lod_bias)
            if dim == 1:
                call = 'tex2Dbias(%s, float4(%s, 0.0, %s))' % (sname, coord, b)
            elif dim == 0:
                call = 'tex1Dbias(%s, float4(%s, 0.0, 0.0, %s))' % (sname, coord, b)
            else:
                call = '%sbias(%s, float4(%s, %s))' % (fn, sname, coord, b)
        elif self.vs:
            # Vertex texture fetch has no derivatives; Xenos samples the base level.
            if dim == 1:
                call = 'tex2Dlod(%s, float4(%s, 0.0, 0.0))' % (sname, coord)
            elif dim == 0:
                call = 'tex1Dlod(%s, float4(%s, 0.0, 0.0, 0.0))' % (sname, coord)
            else:
                call = '%slod(%s, float4(%s, 0.0))' % (fn, sname, coord)
        else:
            call = '%s(%s, %s)' % (fn, sname, coord)
        dest = self.temp_name(ins.dst_reg, ins.dst_rel)
        self.emit_predicated(ins, self.fetch_dest_stmts(dest, ins.dst_swiz, call))

    # Xenos puts pixel shader bool constants at 128+ and loop constants at 16+.
    def bool_reg(self, addr):
        return addr - 128 if not self.vs and addr >= 128 else addr

    def int_reg(self, loop_id):
        return loop_id - 16 if not self.vs and loop_id >= 16 else loop_id

    def bool_name(self, addr):
        n = self.ctab_names['bool'].get(self.bool_reg(addr))
        return n[0] if n else 'b%d' % self.bool_reg(addr)

    def int_name(self, loop_id):
        n = self.ctab_names['int4'].get(self.int_reg(loop_id))
        return n[0] if n else 'i%d' % self.int_reg(loop_id)

    def sampler_ident(self, s):
        n = self.ctab_names['sampler'].get(s)
        return n[0].replace('[', '_').replace(']', '') if n else 's%d' % s

    def sampler_name(self, s):
        n = self.ctab_names['sampler'].get(s)
        return n[0] if n else 's%d' % s

    def vertex_attr(self, addr, ins):
        if addr in self.vfetch_attrs:
            return self.vfetch_attrs[addr][0]
        el = next((e for e in self.c.vertex_elements if e[0] == addr), None)
        if el:
            usage = DECL_USAGE[el[1]]
            field, sem = '%s%d' % (usage.lower(), el[2]), '%s%d' % (usage, el[2])
            if any(v[1] == sem for v in self.vfetch_attrs.values()):
                # Same attribute fetched again (e.g. inside a branch): share the input field.
                self.vfetch_attrs[addr] = (field, None, ins)
                return field
        else:
            field, sem = 'vfetch%d' % addr, None
            self.notes.append('vfetch at instruction %d has no vertex element entry' % addr)
        self.vfetch_attrs[addr] = (field, sem, ins)
        return field

    # ------------------------------------------------------------ control flow
    def emit_body(self, cfs):
        self.cfs = cfs
        self.early_return = False
        self.emit_range(0, len(cfs), 0)

    def cf_condition(self, cf, taken=True):
        """HLSL condition under which a conditional exec runs / a cjmp is taken."""
        want = bool(cf.condition) == taken
        if cf.op == 11 and cf.is_predicated or cf.op in (5, 6):
            self.uses_p0 = True
            return 'p0' if want else '!p0'
        b = self.bool_name(cf.bool_address)
        self.bools.add(cf.bool_address)
        return b if want else '!' + b

    def open_block(self, cond):
        self.emit('if (%s)' % cond)
        self.emit('{')
        self.indent += 1

    def close_block(self):
        self.indent -= 1
        self.emit('}')

    def emit_exec(self, cf, top_level):
        """Emit one exec clause. Returns True if it unconditionally ends the shader."""
        cond = self.cf_condition(cf) if cf.kind in ('cond', 'pred') else None
        ends = cf.is_end
        if cond is not None and (cf.count or ends):
            self.open_block(cond)
        for addr, ins in iter_exec_instructions(self.c.ucode, cf):
            if isinstance(ins, Fetch):
                self.emit_fetch(addr, ins)
            else:
                self.emit_alu(ins)
        if ends and (cond is not None or not top_level):
            self.early_return = True
            self.emit('XE_RETURN')
        if cond is not None and (cf.count or ends):
            self.close_block()
        return ends and cond is None

    def matching_endloop(self, i, stop):
        depth = 0
        for j in range(i + 1, stop):
            if self.cfs[j].op == 7:
                depth += 1
            elif self.cfs[j].op == 8:
                if depth == 0:
                    return j
                depth -= 1
        return None

    def emit_range(self, i, stop, loop_depth, top_level=True):
        """Emit CF instructions [i, stop). Returns True if every path ends the shader."""
        cfs = self.cfs
        while i < stop:
            cf = cfs[i]
            if cf.op in (1, 2, 3, 4, 5, 6, 13, 14):
                if self.emit_exec(cf, top_level and not loop_depth):
                    return True
                i += 1
            elif cf.op == 7:
                j = self.matching_endloop(i, stop)
                if j is None:
                    self.notes.append('loop at cf %d has no matching endloop' % i)
                    i += 1
                    continue
                self.uses_aL = True
                lit = self.c.defs_int.get(cf.loop_id)
                if lit:
                    count, start, step = lit
                    self.emit('for (int xe_iter%d = 0, aL = %d; xe_iter%d < %d; xe_iter%d++, aL += %d)'
                              % (loop_depth, start, loop_depth, count, loop_depth, step))
                else:
                    self.ints.add(cf.loop_id)
                    n = self.int_name(cf.loop_id)
                    self.emit('// loop: count = %s.x, aL starts at %s.y and steps by %s.z' % (n, n, n))
                    self.emit('for (int xe_iter%d = 0, aL = %s.y; xe_iter%d < %s.x; xe_iter%d++, aL += %s.z)'
                              % (loop_depth, n, loop_depth, n, loop_depth, n))
                self.emit('{')
                self.indent += 1
                self.emit_range(i + 1, j, loop_depth + 1, False)
                end = cfs[j]
                if end.is_predicated_break:
                    self.uses_p0 = True
                    self.emit('if (%sp0) break;' % ('' if end.condition else '!'))
                self.indent -= 1
                self.emit('}')
                i = j + 1
            elif cf.op == 11:  # cjmp
                target = cf.address
                if target <= i or target > stop:
                    self.notes.append('unstructured jump at cf %d -> %d' % (i, target))
                    self.emit('// UNHANDLED jump to cf %d' % target)
                    i += 1
                    continue
                if cf.is_unconditional:
                    i = target  # code up to the target is unreachable
                    continue
                skip = self.cf_condition(cf, taken=False)
                last = cfs[target - 1]
                if (target - 1 > i and last.op == 11 and last.is_unconditional
                        and target < last.address <= stop):
                    # if (!taken) { then } else { else } ; join at the else-jump's target
                    self.open_block(skip)
                    a = self.emit_range(i + 1, target - 1, loop_depth, False)
                    self.close_block()
                    self.emit('else')
                    self.emit('{')
                    self.indent += 1
                    b = self.emit_range(target, last.address, loop_depth, False)
                    self.close_block()
                    if a and b:
                        return True
                    i = last.address
                else:
                    self.open_block(skip)
                    self.emit_range(i + 1, target, loop_depth, False)
                    self.close_block()
                    i = target
            elif cf.op == 8:
                self.notes.append('endloop without loop at cf %d' % i)
                i += 1
            elif cf.op in (9, 10):
                self.notes.append('unhandled control flow %s at cf %d' % (cf.name, cf.index))
                self.emit('// UNHANDLED control flow: %s' % cf.name)
                i += 1
            else:  # nop, alloc, mark_vs_fetch_done
                i += 1
        return False

    # ------------------------------------------------------------ output
    def run(self):
        c = self.c
        cfs = parse_control_flow(c.ucode)
        self.prepass(cfs)
        self.indent = 1
        self.emit_body(cfs)
        body = self.lines
        self.lines = []
        return self.assemble(cfs, body)

    def semantic_for_interp(self, reg):
        it = next((i for i in self.c.interpolators if i[0] == reg), None)
        if it:
            usage = DECL_USAGE[it[1]]
            return '%s%d' % (usage.lower(), it[2]), '%s%d' % (usage, it[2]), 4
        return 'texcoord%d' % reg, 'TEXCOORD%d' % reg, 4

    def assemble(self, cfs, body):
        c = self.c
        L = []
        kind = 'vertex' if self.vs else 'pixel'
        L.append('// %s' % self.name)
        L.append('// Xenos %s shader decompiled to HLSL (D3D9 / SM3).' % kind)
        L.append('// Container flags 0x%08X, %d ucode dwords, %d literal constant(s).'
                 % (c.flags, len(c.ucode), len(c.defs_float)))
        if c.ctab:
            L.append('// Constant table creator: %s (%s)' % (c.ctab['creator'], c.ctab['target']))
        L.append('// Shader header: %s' % ' '.join('%08X' % w for w in c.shader_words))
        if self.vs:
            for e in c.vertex_elements:
                L.append('//   vertex element: instruction %d -> %s%d' % (e[0], DECL_USAGE[e[1]], e[2]))
        for reg, usage, idx, extra in c.interpolators:
            L.append('//   interpolator: r%d <-> %s%d (flags 0x%X)' % (reg, DECL_USAGE[usage], idx, extra))
        if c.def_header and any(c.def_header):
            L.append('// Definition header: %s' % ' '.join('%08X' % w for w in c.def_header))
        if c.def_extra:
            L.append('// Non-float definitions (raw): %s' % ' '.join('%08X' % w for w in c.def_extra))
        for n in self.notes:
            L.append('// NOTE: %s' % n)
        L.append('')
        L.append('#include "xenos_common.hlsli"')
        L.append('')

        # ---- uniforms
        decl = []
        if self.const_array:
            decl.append('float4 c[256] : register(c0);')
            for k in self.ctab_float:
                decl.append('//   c[%d..%d] = %s (%s)' % (k.index, k.index + k.count - 1, k.name, self.type_desc(k.type)))
            if c.defs_float:
                decl.append('// NOTE: literal constants c%s are inlined; relative reads cannot see them.'
                            % ', c'.join(str(r) for r in sorted(c.defs_float)))
        else:
            declared = set()
            for k in self.ctab_float:
                dflt = ''
                if k.default:
                    dflt = ' // default: %s' % ', '.join(fmt_float(v) for v in k.default)
                decl.append('float4 %s%s : register(c%d); // %s%s'
                            % (k.name, '' if k.count == 1 else '[%d]' % k.count, k.index, self.type_desc(k.type), dflt))
                declared.update(range(k.index, k.index + k.count))
            for reg, rel in sorted(self.const_refs, key=lambda x: x[0]):
                if rel is None and reg not in declared:
                    decl.append('float4 c%d : register(c%d);' % (reg, reg))
                    declared.add(reg)
        for b in sorted(self.bools):
            decl.append('bool %s : register(b%d);' % (self.bool_name(b), self.bool_reg(b)))
        for i in sorted(self.ints):
            decl.append('int3 %s : register(i%d); // x = count, y = start, z = step'
                        % (self.int_name(i), self.int_reg(i)))
        for s in sorted(self.samplers):
            t = ['sampler1D', 'sampler2D', 'sampler3D', 'samplerCUBE'][self.samplers[s]]
            n = self.ctab_names['sampler'].get(s)
            decl.append('%s %s : register(s%d);' % (t, n[0] if n else 's%d' % s, s))
        for s, dim in sorted(self.sampler_extra):
            t = ['sampler1D', 'sampler2D', 'sampler3D', 'samplerCUBE'][dim]
            decl.append('%s %s_%s; // fetch constant %d again, sampled as %s'
                        % (t, self.sampler_ident(s), DIM_NAMES[dim], s, DIM_NAMES[dim]))
        for s in sorted(self.texel_size):
            decl.append('float4 %s_TexelSize; // xyz = 1 / texture size. Xenos applied fetch texel offsets '
                        'and computed LODs natively; SM3 needs it passed in.' % self.sampler_ident(s))
        if self.vbuffers:
            decl.append('sampler2D xe_vfetch_heap; // indexed vertex buffers as one float4 texture '
                        '(one texel per 4 dwords)')
            decl.append('float2 xe_vfetch_heap_Size; // heap width, height in texels')
        for fc in sorted(self.vbuffers):
            decl.append('float vfetch_c%d_Base; // first heap texel of vertex fetch constant %d' % (fc, fc))
        if decl:
            L += decl
            L.append('')

        # ---- IO structs
        if self.vs:
            L.append('struct VS_INPUT')
            L.append('{')
            seen = set()
            for addr in sorted(self.vfetch_attrs):
                field, sem, ins = self.vfetch_attrs[addr]
                if field in seen:
                    continue
                seen.add(field)
                if sem is None:
                    sem = 'TEXCOORD%d' % (8 + addr % 8)
                L.append('    float4 %s : %s;' % (field, sem))
            L.append('};')
            L.append('')
            outs = self.vs_outputs()
            L.append('struct VS_OUTPUT')
            L.append('{')
            for reg, field, sem, n, src in outs:
                L.append('    %s %s : %s;' % ('float' if n == 1 else 'float%d' % n, field, sem))
            L.append('};')
            L.append('')
            L.append('VS_OUTPUT main(VS_INPUT In)')
        else:
            has_input = bool(c.interpolators) or c.param_gen_reg is not None
            if has_input:
                L.append('struct PS_INPUT')
                L.append('{')
                for reg, usage, idx, mask in sorted(c.interpolators):
                    L.append('    float4 %s%d : %s%d; // r%d'
                             % (DECL_USAGE[usage].lower(), idx, DECL_USAGE[usage], idx, reg))
                if c.param_gen_reg is not None:
                    L.append('    float2 vPos : VPOS;   // r%d (pixel parameters)' % c.param_gen_reg)
                    L.append('    float vFace : VFACE;  // r%d' % c.param_gen_reg)
                L.append('};')
                L.append('')
            outs = self.ps_outputs()
            L.append('struct PS_OUTPUT')
            L.append('{')
            for name, field, sem in outs:
                L.append('    %s %s : %s;' % ('float' if name == 'oDepth' else 'float4', field, sem))
            L.append('};')
            L.append('')
            L.append('PS_OUTPUT main(%s)' % ('PS_INPUT In' if has_input else ''))
        if self.early_return:
            # Xenos can end the shader inside a conditional exec; return the outputs so far.
            main_sig = L.pop()
            L.append('#define XE_RETURN { %s }' % ' '.join(self.output_stmts()))
            L.append('')
            L.append(main_sig)
        L.append('{')

        # ---- locals
        interp_init = {}
        if not self.vs:
            for reg, usage, idx, mask in c.interpolators:
                interp_init[reg] = 'In.%s%d' % (DECL_USAGE[usage].lower(), idx)
            if c.param_gen_reg is not None:
                # Xenos pixel parameters: |xy| = pixel position, sign of x = back facing.
                interp_init[c.param_gen_reg] = 'float4(In.vFace < 0.0 ? -In.vPos.x : In.vPos.x, In.vPos.y, 0.0, 0.0)'
        if self.temp_array:
            L.append('    float4 r[32];')
            for i in range(32):
                L.append('    r[%d] = %s;' % (i, interp_init.get(i, '0.0')))
        else:
            for t in sorted(self.temps | set(interp_init)):
                L.append('    float4 r%d = %s;' % (t, interp_init.get(t, '0.0')))
        for o in sorted(self.outputs):
            if o == 'oDepth':
                L.append('    float4 oDepth = 0.0;')
            else:
                L.append('    float4 %s = 0.0;' % o)
        if self.uses_ps:
            L.append('    float ps = 0.0;')
        if self.uses_p0:
            L.append('    bool p0 = false;')
        if self.uses_a0:
            L.append('    int a0 = 0;')
        if self.uses_aL and not self.ints:
            L.append('    int aL = 0;')
        if self.uses_lod:
            L.append('    float xe_lod = 0.0;')
        L.append('')
        L += body
        L.append('')
        L += ['    ' + s for s in self.output_stmts()]
        L.append('}')
        return '\n'.join(L) + '\n'

    def output_stmts(self):
        if self.vs:
            out = ['VS_OUTPUT Out;']
            for reg, field, sem, n, src in self.vs_outputs():
                sw = '' if n == 4 else '.' + 'xyzw'[:n]
                out.append('Out.%s = %s%s;' % (field, src, sw))
        else:
            out = ['PS_OUTPUT Out;']
            for name, field, sem in self.ps_outputs():
                out.append('Out.%s = %s%s;' % (field, name, '.x' if name == 'oDepth' else ''))
        out.append('return Out;')
        return out

    def vs_outputs(self):
        outs = [(62, 'position', 'POSITION', 4, 'oPos')]
        for o in sorted(self.outputs, key=lambda n: (len(n), n)):
            if o.startswith('o') and o[1:].isdigit():
                reg = int(o[1:])
                field, sem, n = self.semantic_for_interp(reg)
                outs.append((reg, field, sem, n, o))
        if 'oPts' in self.outputs:
            outs.append((63, 'pointSize', 'PSIZE', 1, 'oPts'))
        if 'oPos' not in self.outputs:
            self.outputs.add('oPos')
        return outs

    def ps_outputs(self):
        outs = []
        for o in sorted(self.outputs):
            if o.startswith('oC'):
                outs.append((o, 'color%s' % o[2:], 'COLOR%s' % o[2:]))
        if 'oDepth' in self.outputs:
            outs.append(('oDepth', 'depth', 'DEPTH'))
        if not outs:
            self.outputs.add('oC0')
            outs.append(('oC0', 'color0', 'COLOR0'))
        return outs

    @staticmethod
    def type_desc(t):
        cls = CTAB_CLASS.get(t.cls, '?')
        typ = CTAB_TYPE.get(t.typ, '?')
        if cls.startswith('matrix'):
            s = '%s%dx%d (%s)' % (typ, t.rows, t.cols, cls)
        elif cls == 'vector':
            s = '%s%d' % (typ, t.cols)
        elif cls == 'struct':
            s = 'struct { %s }' % '; '.join('%s %s' % (Decompiler.type_desc(mt), mn) for mn, mt in t.members)
        else:
            s = typ
        if t.elements > 1:
            s += '[%d]' % t.elements
        return s


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    import shutil
    shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xenos_common.hlsli'), dst)
    failures = []
    for path in sorted(glob.glob(os.path.join(src, '*.bin'))):
        name = os.path.basename(path)
        try:
            c = Container(open(path, 'rb').read())
            text = Decompiler(name, c).run()
        except Exception as e:  # keep going; report at the end
            import traceback
            traceback.print_exc()
            failures.append((name, repr(e)))
            continue
        with open(os.path.join(dst, name[:-4] + '.hlsl'), 'w', newline='\n') as f:
            f.write(text)
    for n, e in failures:
        print('FAILED', n, e)


if __name__ == '__main__':
    main()
