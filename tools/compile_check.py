"""Compile every .hlsl in a folder with d3dcompiler_47 (vs_3_0 / ps_3_0) to validate syntax.

usage: python compile_check.py <dir> [-v] [-j N]
"""
import ctypes
import glob
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from ctypes import wintypes

_d3dc = None


class ID3DBlobVtbl(ctypes.Structure):
    _fields_ = [('QueryInterface', ctypes.c_void_p), ('AddRef', ctypes.c_void_p),
                ('Release', ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
                ('GetBufferPointer', ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)),
                ('GetBufferSize', ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p))]


class ID3DBlob(ctypes.Structure):
    _fields_ = [('lpVtbl', ctypes.POINTER(ID3DBlobVtbl))]


def blob_text(p):
    if not p:
        return ''
    b = ctypes.cast(p, ctypes.POINTER(ID3DBlob))
    ptr = b.contents.lpVtbl.contents.GetBufferPointer(p)
    size = b.contents.lpVtbl.contents.GetBufferSize(p)
    s = ctypes.string_at(ptr, size).decode('latin-1').rstrip('\0')
    b.contents.lpVtbl.contents.Release(p)
    return s


def d3dc():
    global _d3dc
    if _d3dc is None:
        _d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
        _d3dc.D3DCompileFromFile.argtypes = [
            wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
        _d3dc.D3DCompileFromFile.restype = ctypes.c_long
        _d3dc.D3DCompile.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
        _d3dc.D3DCompile.restype = ctypes.c_long
    return _d3dc


def check(path):
    """Returns (status, name, message); status is 'ok', 'ok-xbox' or 'fail'."""
    lib = d3dc()
    name = os.path.basename(path)
    target = b'vs_3_0' if name.startswith('vs') else b'ps_3_0'
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = lib.D3DCompileFromFile(path, None, ctypes.c_void_p(1), b'main', target, 0, 0,
                                ctypes.byref(code), ctypes.byref(err))
    msg = blob_text(err.value)
    blob_text(code.value)
    status = 'ok'
    if hr < 0 and 'X4509' in msg:
        # Xbox 360 register ranges exceed PC SM3 (PS c224+, PS s16+, VS s4+): re-check with
        # the register bindings removed so everything else is still validated.
        src = open(path).read()
        src = re.sub(r'\s*:\s*register\([^)]*\)', '', src)
        data = src.encode()
        code, err = ctypes.c_void_p(), ctypes.c_void_p()
        hr = lib.D3DCompile(data, len(data), path.encode(), None, ctypes.c_void_p(1), b'main', target, 0, 0,
                            ctypes.byref(code), ctypes.byref(err))
        msg = blob_text(err.value)
        blob_text(code.value)
        status = 'ok-xbox'
    if hr < 0:
        return 'fail', name, '%s\n%s' % (hex(hr & 0xFFFFFFFF), msg.strip())
    return status, name, msg.strip()


def main():
    args = [a for a in sys.argv[1:]]
    verbose = '-v' in args
    jobs = os.cpu_count() or 1
    if '-j' in args:
        jobs = int(args[args.index('-j') + 1])
    folder = [a for a in args if not a.startswith('-') and not a.isdigit()][0]
    paths = sorted(glob.glob(os.path.join(folder, '*.hlsl')))
    ok = bad = 0
    with ProcessPoolExecutor(max_workers=jobs) as ex:
        for status, name, msg in ex.map(check, paths, chunksize=8):
            if status == 'fail':
                bad += 1
                hr, _, text = msg.partition('\n')
                print('FAIL', name, hr)
                print('   ', text.replace('\n', '\n    ')[:600], flush=True)
                continue
            ok += 1
            if status == 'ok-xbox' and verbose:
                print('OK (Xbox-only register range)', name)
            if msg and verbose:
                print('WARN', name, msg[:300])
    print('ok', ok, 'failed', bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
