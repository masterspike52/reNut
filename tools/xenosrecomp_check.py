"""Run XenosRecomp on every dumped shader one at a time and compile each result with DXC.

usage: python xenosrecomp_check.py <XenosRecomp.exe> <shader .bin dir> <hlsl out dir> [-j N] [--dxil]

Single-file mode is used so one crashing shader doesn't hide the rest. Each generated
HLSL is compiled to SPIR-V (and optionally DXIL) with the same arguments XenosRecomp's
dxc_compiler.cpp uses, and failures are grouped by their first error message.
"""
import collections
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XR_ROOT = os.path.join(ROOT, 'thirdparty', 'reblue-XenosRecomp')
INCLUDE = os.path.join(XR_ROOT, 'XenosRecomp', 'shader_common.h')
DXC = os.path.join(XR_ROOT, 'thirdparty', 'dxc-bin', 'bin', 'x64', 'dxc.exe')


def dxc_args(hlsl, pixel, spirv, library=False):
    # main.cpp compiles DXIL shaders that use spec constants as libraries (specConstantsMask != 0),
    # which the runtime then has to link.
    target = 'lib_6_3' if library else ('ps_6_0' if pixel else 'vs_6_0')
    args = [DXC, '-T', target, '-HV', '2021', '-all-resources-bound']
    if spirv:
        args += ['-spirv', '-fvk-use-dx-layout']
        if not pixel:
            args.append('-fvk-invert-y')
    else:
        args += ['-Wno-ignored-attributes', '-Qstrip_reflect']
    return args + ['-Qstrip_debug', '-DRENUT_RECOMP', hlsl, '-Fo', os.devnull]


def first_error(text):
    for line in text.splitlines():
        m = re.search(r'error: (.*)', line)
        if m:
            # Normalise identifiers / numbers so similar errors group together.
            msg = re.sub(r"'[^']*'", "'X'", m.group(1))
            return re.sub(r'\d+', 'N', msg)
    return text.strip().splitlines()[-1] if text.strip() else 'unknown'


def check(exe, path, out_dir, dxil):
    name = os.path.basename(path)[:-4]
    pixel = name.startswith('ps')
    hlsl = os.path.join(out_dir, name + '.hlsl')
    r = subprocess.run([exe, path, hlsl, INCLUDE], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(hlsl):
        return name, 'recompile', 'XenosRecomp exit code %d' % (r.returncode & 0xFFFFFFFF), r.stderr
    linked = False
    for spirv in ([True, False] if dxil else [True]):
        c = subprocess.run(dxc_args(hlsl, pixel, spirv), capture_output=True, text=True)
        if c.returncode != 0 and not spirv and 'External function used in non-library profile' in c.stderr:
            linked = True
            c = subprocess.run(dxc_args(hlsl, pixel, spirv, library=True), capture_output=True, text=True)
        if c.returncode != 0:
            return name, 'spirv' if spirv else 'dxil', first_error(c.stderr), c.stderr
    return name, 'ok-linked' if linked else 'ok', '', ''


def main():
    args = sys.argv[1:]
    jobs = os.cpu_count() or 1
    if '-j' in args:
        i = args.index('-j')
        jobs = int(args[i + 1])
        del args[i:i + 2]
    dxil = '--dxil' in args
    args = [a for a in args if a != '--dxil']
    exe, src, out_dir = args
    os.makedirs(out_dir, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(src, '*.bin')))

    groups = collections.defaultdict(list)
    details = {}
    linked = 0
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for name, stage, err, text in ex.map(lambda p: check(exe, p, out_dir, dxil), paths):
            if stage == 'ok-linked':
                linked += 1
            elif stage != 'ok':
                groups[(stage, err)].append(name)
                details[name] = text

    failed = sum(len(v) for v in groups.values())
    for (stage, err), names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('%5d  [%s] %s' % (len(names), stage, err))
        print('       e.g. %s' % ', '.join(names[:4]))
    with open(os.path.join(out_dir, 'failures.txt'), 'w') as f:
        for name in sorted(details):
            f.write('==== %s\n%s\n' % (name, details[name]))
    if dxil:
        print('DXIL shaders needing runtime linking (spec constants): %d' % linked)
    print('ok %d failed %d (details: %s)' % (len(paths) - failed, failed, os.path.join(out_dir, 'failures.txt')))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
