#!/usr/bin/env python3
"""check_placeholders.py <anv 0001 patch> [placeholders.inc] [D3D12Api.cpp]
Consistency check between the places that define the placeholder shaders of run-time compiled kernels: the driver
(ANV_CM_* in src/intel/vulkan/anv_cm.c of the ANV patch), the shim's templates (shim/src/dll/placeholders.inc, from
tools/gen_placeholders.py) and the shim's self-test values (D3D12Api.cpp). Checks the templates' DXBC checksums, their
numthreads and that the store immediates sit where the shim patches them, and that a patched copy (the shim's
procedure) is a valid container again.
The layer and vulkan-1.dll have the same constants in their own sources (they are built apart from the driver and the
shim): layer/src/placeholder.h and vkwrap/vkwrap.c must give the driver's magic, work-group sizes and self-test
values, and vkwrap.c the layer's set number for XeSS' descriptor push (layer/src/layer.c).
Exits non-zero on any mismatch."""
import math, os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_placeholders as gen  # noqa: E402

def fail(msg):
    sys.exit("check_placeholders: " + msg)


def define(patch, name):
    m = re.search(r"^\+#define %s (0x[0-9a-fA-F]+|\d+)u?\b" % name, patch, re.M)
    if not m:
        fail("%s not found in the patch" % name)
    return int(m.group(1), 0)


def c_define(path, name):
    """#define <name> <number> of a C source in the tree (path from the repository's root)"""
    text = open(os.path.join(here, "..", *path.split("/")), encoding="utf-8").read()
    m = re.search(r"^#define %s (0x[0-9a-fA-F]+|\d+)u?\b" % name, text, re.M)
    if not m:
        fail("%s not found in %s" % (name, path))
    return int(m.group(1), 0)


def check_copies(patch):
    """the constants the layer and vulkan-1.dll define for themselves, against the driver's"""
    names = {"XMX_MAGIC": "ANV_CM_DUMMY_MAGIC", "XMX_HASH_WG_Z": "ANV_CM_HASH_WG_Z", "XMX_CANARY_WG_Z": "ANV_CM_CANARY_WG_Z",
             "XMX_CANARY_LO": "ANV_CM_CANARY_LO", "XMX_CANARY_HI": "ANV_CM_CANARY_HI",
             "XMX_LEGACY_CANARY_WG_Z": "ANV_CM_LEGACY_CANARY_WG_Z"}
    for path in ("layer/src/placeholder.h", "vkwrap/vkwrap.c"):
        for ours, drv in names.items():
            if path.endswith("vkwrap.c") and ours == "XMX_LEGACY_CANARY_WG_Z":
                continue   # vulkan-1.dll never makes the self-test of an older shim
            a, b = c_define(path, ours), define(patch, drv)
            if a != b:
                fail("%s: %s is %#x, the driver's %s is %#x" % (path, ours, a, drv, b))
    a, b = c_define("vkwrap/vkwrap.c", "PUSH_SET"), c_define("layer/src/layer.c", "XMX_VK_PUSH_SET")
    if a != b:
        fail("vkwrap/vkwrap.c: PUSH_SET is %#x, layer/src/layer.c's XMX_VK_PUSH_SET is %#x" % (a, b))


# ---- the DXBC checksum: MD5 (RFC 1321) with DXBC's own end of message (see shim/src/dll/DxbcChecksum.h)
K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]
S = [(7, 12, 17, 22), (5, 9, 14, 20), (4, 11, 16, 23), (6, 10, 15, 21)]


def md5_block(st, blk):
    m = struct.unpack("<16I", blk)
    a, b, c, d = st
    for i in range(64):
        r = i // 16
        if r == 0:
            f, g = (b & c) | (~b & d), i
        elif r == 1:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif r == 2:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        x = (a + (f & 0xFFFFFFFF) + K[i] + m[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + ((x << S[r][i % 4]) | (x >> (32 - S[r][i % 4])))) & 0xFFFFFFFF
    return [(s + v) & 0xFFFFFFFF for s, v in zip(st, (a, b, c, d))]


def dxbc_checksum(blob):
    data = blob[20:]
    n = len(data)
    full = n & ~63
    rem = n - full
    bits = (n * 8) & 0xFFFFFFFF
    tail = (bits >> 2) | 1
    st = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    for o in range(0, full, 64):
        st = md5_block(st, data[o:o + 64])
    if rem >= 56:
        st = md5_block(st, data[full:] + b"\x80" + bytes(63 - rem))
        last = struct.pack("<I", bits) + bytes(56)
    else:
        last = struct.pack("<I", bits) + data[full:] + b"\x80" + bytes(55 - rem)
    st = md5_block(st, last + struct.pack("<I", tail))
    return struct.pack("<4I", *st)


def md5(msg):
    """plain MD5, only to check md5_block against RFC 1321's test values"""
    st = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    m = msg + b"\x80" + bytes((55 - len(msg)) % 64) + struct.pack("<Q", 8 * len(msg))
    for o in range(0, len(m), 64):
        st = md5_block(st, m[o:o + 64])
    return struct.pack("<4I", *st).hex()


def array(inc, inc_path, name):
    m = re.search(r"static const unsigned char %s\[\] = \{([^}]*)\};" % name, inc)
    if not m:
        fail("%s not found in %s" % (name, inc_path))
    return bytes(int(v, 16) for v in m.group(1).split(","))


def size_const(name, text, where):
    m = re.search(r"static const (?:size_t|unsigned|uint32_t|unsigned int) %s = (0x[0-9a-fA-F]+|\d+)u?;" % name, text)
    if not m:
        fail("%s not found in %s" % (name, where))
    return int(m.group(1), 0)


def numthreads(blob):
    """dcl_thread_group of the SHEX/SHDR chunk"""
    count = struct.unpack_from("<I", blob, 28)[0]
    for k in range(count):
        off = struct.unpack_from("<I", blob, 32 + 4 * k)[0]
        tag = blob[off:off + 4]
        if tag in (b"SHEX", b"SHDR"):
            size = struct.unpack_from("<I", blob, off + 4)[0]
            toks = struct.unpack_from("<%dI" % (size // 4), blob, off + 8)
            i = 2
            while i < len(toks):
                opcode, length = toks[i] & 0x7FF, (toks[i] >> 24) & 0x7F
                if opcode == 0x9B:   # dcl_thread_group
                    return toks[i + 1:i + 4]
                i += max(length, 1)
    fail("no dcl_thread_group")


def main():
    patch = open(sys.argv[1], encoding="utf-8").read()
    inc_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "shim", "src", "dll", "placeholders.inc")
    api_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(here, "..", "shim", "src", "dll", "D3D12Api.cpp")
    inc = open(inc_path, encoding="utf-8").read()
    api = open(api_path, encoding="utf-8").read()
    if md5(b"") != "d41d8cd98f00b204e9800998ecf8427e" or md5(b"abc") != "900150983cd24fb0d6963f7d28e17f72" or \
            md5(b"12345678901234567890123456789012345678901234567890123456789012345678901234567890") != \
            "57edf4a22be3c955ac49da2e2107b67a":
        fail("MD5 does not match RFC 1321's test values")
    magic = define(patch, "ANV_CM_DUMMY_MAGIC")
    if magic != gen.MAGIC:
        fail("magic: driver %#x, generator %#x" % (magic, gen.MAGIC))
    shapes = {"kernel": (define(patch, "ANV_CM_HASH_WG_Z"), gen.KERNEL_Z),
              "canary": (define(patch, "ANV_CM_CANARY_WG_Z"), gen.CANARY_Z)}
    for what, (drv, g) in shapes.items():
        if drv != g:
            fail("%s workgroup z: driver %d, generator %d" % (what, drv, g))
    canary_lo, canary_hi = define(patch, "ANV_CM_CANARY_LO"), define(patch, "ANV_CM_CANARY_HI")
    if (size_const("kCanaryLo", api, api_path), size_const("kCanaryHi", api, api_path)) != (canary_lo, canary_hi):
        fail("self-test values differ between the shim (%s) and the driver" % api_path)
    for name, at_name, z, (lo, hi) in (("kPlaceholderKernel", "kPlaceholderKernelHashAt", gen.KERNEL_Z, (0x89ABCDEF, 0x01234567)),
                                       ("kPlaceholderCanary", "kPlaceholderCanaryHashAt", gen.CANARY_Z, (canary_lo, canary_hi))):
        blob = array(inc, inc_path, name)
        at = size_const(at_name, inc, inc_path)
        if blob[:4] != b"DXBC" or struct.unpack_from("<I", blob, 24)[0] != len(blob):
            fail("%s is not a DXBC container of its size" % name)
        if dxbc_checksum(blob) != blob[4:20]:
            fail("%s: the checksum does not match (fxc's own output, so the checksum code is wrong)" % name)
        if tuple(numthreads(blob)) != (1, 1, z):
            fail("%s: numthreads %s, expected (1, 1, %d)" % (name, numthreads(blob), z))
        if struct.unpack_from("<3I", blob, at - 4) != (magic, gen.SENTINEL_LO, gen.SENTINEL_HI):
            fail("%s: no l(magic, lo, hi) at byte %d" % (name, at))
        # the shim's procedure: the two dwords, then the checksum
        p = bytearray(blob)
        struct.pack_into("<2I", p, at, lo, hi)
        p[4:20] = dxbc_checksum(bytes(p))
        if dxbc_checksum(bytes(p)) != bytes(p[4:20]) or p[4:20] == blob[4:20]:
            fail("%s: patched copy not consistent" % name)
    check_copies(patch)
    print("ok: placeholder templates (kernel 1,1,%d, self-test 1,1,%d) match the driver, checksums verified; the layer "
          "and vkwrap have the driver's constants" % (gen.KERNEL_Z, gen.CANARY_Z))


if __name__ == "__main__":
    main()
