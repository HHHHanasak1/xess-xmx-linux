#!/usr/bin/env python3
"""prologue_asm.py - the layer's kernel prologue as IGA assembly text, and IGA through its C API.

A kernel compiled with -noLoadPayload expects its payload in registers instead of loading it: the cross-thread data
in r2 (r3 when it is larger than 64 bytes) and the local ids in r1.0-2:uw. An unpatched ANV starts the thread with
r0 (thread id in r0.2[7:0]) and, with inline data, num_workgroups in r1.0-2. The prologue builds the payload from
those (Xe3, 64-byte GRF): the cross-thread data image of the .cmk header as immediates, group_count copied from the
inline data, then the local ids from the thread id. r4 is scratch. Tested on the device in E3/E3b of the decoupling
work.

Used by gen_prologue_table.py (the C table of the layer) and by the layer's tests (reference prologues).
Needs libiga64 of the IGC bundle: IGC=<bundle>/igc (the folder that holds igc/local/lib).
"""
import ctypes as C
import os
import struct
import sys

IGA_XE3 = (3 << 24) | 0


class CtxOpts(C.Structure):
    _fields_ = [('cb', C.c_size_t), ('gen', C.c_int)]


class AsmOpts(C.Structure):
    _fields_ = [('cb', C.c_uint32), ('enabled_warnings', C.c_uint32), ('encoder_opts', C.c_uint32),
                ('syntax_opts', C.c_uint32), ('r0', C.c_uint32), ('r1', C.c_uint32),
                ('sbid_count', C.c_uint32), ('swsb_encode_mode', C.c_uint32)]


class Diag(C.Structure):
    _fields_ = [('line', C.c_uint32), ('column', C.c_uint32), ('offset', C.c_uint32), ('extent', C.c_uint32),
                ('message', C.c_char_p)]


_lib = None


def lib():
    global _lib
    if _lib is None:
        igc = os.environ.get('IGC', os.path.expanduser('~/decouple-exp/igc/igc'))
        _lib = C.CDLL(f'{igc}/igc/local/lib/libiga64.so.2')
    return _lib


def assemble(text, gen=IGA_XE3):
    """IGA text -> bytes; no auto-compaction, SWSB as written (every instruction is 16 bytes)."""
    L = lib()
    ctx = C.c_void_p()
    st = L.iga_context_create(C.byref(CtxOpts(C.sizeof(CtxOpts), gen)), C.byref(ctx))
    assert st == 0, f'iga_context_create {st}'
    o = AsmOpts(C.sizeof(AsmOpts), 0x5, 0, 0, 0, 0, 16, 0)
    out = C.c_void_p()
    n = C.c_uint32()
    st = L.iga_context_assemble(ctx, C.byref(o), text.encode(), C.byref(out), C.byref(n))
    if st != 0:
        ds = C.POINTER(Diag)()
        dn = C.c_uint32()
        L.iga_context_get_errors(ctx, C.byref(ds), C.byref(dn))
        msg = '; '.join(ds[i].message.decode() for i in range(dn.value))
        L.iga_context_release(ctx)
        raise SystemExit(f'iga_context_assemble failed ({st}): {msg}')
    code = C.string_at(out, n.value)
    L.iga_context_release(ctx)
    return code


def log2(v):
    assert v & (v - 1) == 0, f'{v} is not a power of two'
    return v.bit_length() - 1


def ins(s):
    return f'(W)     {s} {{A@1}}'


# the instruction forms, each with its variable fields as parameters
def clr(r):                   # rR = 0
    return ins(f'mov (16|M0)              r{r}.0<1>:ud    0x0:ud')


def imm(slot, v):             # dword `slot` of r2..r3 = v
    return ins(f'mov (1|M0)               r{2 + slot // 16}.{slot % 16}<1>:ud    0x{v:x}:ud')


def gc(slot, i):              # dword `slot` of r2..r3 = num_workgroups[i] (inline data, r1.i)
    return ins(f'mov (1|M0)               r{2 + slot // 16}.{slot % 16}<1>:ud    r1.{i}<0;1,0>:ud')


def tid():                    # r4.0 = thread id in the group
    return ins('and (1|M0)               r4.0<1>:ud    r0.2<0;1,0>:ud    0xff:ud')


def r1clr():
    return ins('mov (16|M0)              r1.0<1>:ud    0x0:ud')


def and41(m):
    return ins(f'and (1|M0)               r4.1<1>:ud    r4.0<0;1,0>:ud    0x{m:x}:ud')


def shr42(s):
    return ins(f'shr (1|M0)               r4.2<1>:ud    r4.0<0;1,0>:ud    0x{s:x}:ud')


def and42(m):
    return ins(f'and (1|M0)               r4.2<1>:ud    r4.2<0;1,0>:ud    0x{m:x}:ud')


def shr43(s):
    return ins(f'shr (1|M0)               r4.3<1>:ud    r4.0<0;1,0>:ud    0x{s:x}:ud')


def lid(i):                   # r1.i:uw = local id i
    return ins(f'mov (1|M0)               r1.{i}<1>:uw    r4.{i + 1}<0;1,0>:ud')


def prologue_lines(hdr, gc_off=None):
    """hdr: the CMK2/CML1 header (472 bytes or more). Same program as E3b's gen_prologue2.py."""
    code_size, grf, slm, barriers, lx, ly, lz, cross, per = struct.unpack_from('<9I', hdr, 4)
    cd = hdr[40:40 + 128]
    nreg = (cross + 63) // 64
    assert 1 <= nreg <= 2 or cross == 0
    nreg = max(nreg, 1)
    L = [clr(2 + r) for r in range(nreg)]
    for i in range(nreg * 16):
        v = struct.unpack_from('<I', cd, 4 * i)[0] if 4 * i < 128 else 0
        if v and 4 * i < cross:
            L.append(imm(i, v))
    if gc_off is not None:
        for i in range(3):
            L.append(gc(gc_off // 4 + i, i))
    L += [tid(), r1clr(), and41(lx - 1), shr42(log2(lx)), and42(ly - 1), shr43(log2(lx * ly)), lid(0), lid(1), lid(2)]
    return L


def prologue_bytes(hdr, gc_off=None):
    return assemble('\n'.join(prologue_lines(hdr, gc_off)) + '\n')


if __name__ == '__main__':
    # prologue_asm.py <.cmk or .cml> [GC_OFF] <out.bin>: the assembled prologue of that kernel
    d = open(sys.argv[1], 'rb').read()
    g = int(sys.argv[2], 0) if len(sys.argv) > 3 else None
    if d[:4] == b'CML1':
        gv = struct.unpack_from('<I', d, 472)[0]
        g = None if gv == 0xffffffff else gv
    open(sys.argv[-1], 'wb').write(prologue_bytes(d, g))
