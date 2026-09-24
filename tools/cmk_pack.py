#!/usr/bin/env python3
"""cmk_pack.py <zebin> <kernel> <lx> <ly> <lz> <out.cmk> [--heap SET BINDING PCBASE] [--surf TABLE[:ELEM] ...]

Packs one kernel of an Intel zebin (ocloc/IGC output) into the CMK2 format read by the patched ANV.
--heap/--surf describe how the kernel's surfaces (in argument order) are found at dispatch time: surface k is descriptor
`push_constants[PCBASE + 4*TABLE] + ELEM` of the descriptor heap at (SET, BINDING) - the vkd3d-proton root signature model."""
import os, sys, struct, re

args = sys.argv[1:]
zebin, kname, lx, ly, lz, out = args[0], args[1], *map(int, args[2:5]), args[5]
rest = args[6:]
heap = (0, 0, 0)
surfs = []
i = 0
while i < len(rest):
    if rest[i] == "--heap":
        if rest[i + 1] == "auto":
            heap = (0xFFFFFFFF, 0, 0); i += 2
        else:
            heap = tuple(int(x) for x in rest[i + 1:i + 4]); i += 4
    elif rest[i] == "--surf":
        i += 1
        while i < len(rest) and not rest[i].startswith("--"):
            if rest[i] == "auto":
                surfs = "auto"; i += 1; continue
            t, _, e = rest[i].partition(":")
            surfs.append((int(t), int(e or 0))); i += 1
    else:
        raise SystemExit("bad argument " + rest[i])

d = open(zebin, "rb").read()
assert d[:4] == b"\x7fELF"
shoff = struct.unpack_from("<Q", d, 0x28)[0]
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
secs = []
for k in range(shnum):
    o = shoff + k * shentsize
    name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", d, o)
    secs.append((name, typ, off, size))
stroff = secs[shstrndx][2]
def sname(n):
    e = d.index(b"\0", stroff + n)
    return d[stroff + n:e].decode()
named = {sname(s[0]): s for s in secs}
if kname == "auto":
    kname = next(n[6:] for n in named if n.startswith(".text.") and not n.startswith(".text.Intel_"))
text = named[".text." + kname]
code = d[text[2]:text[2] + text[3]]
zi = named[".ze_info"]
info = d[zi[2]:zi[2] + zi[3]].decode("latin1")
blocks = re.split(r"\n  - name:\s+", "\n" + info[info.index("kernels:"):])
blk = next(b for b in blocks if b.startswith(kname))
def geti(key, default=0):
    m = re.search(r"\n\s+" + key + r":\s+(\d+)", blk)
    return int(m.group(1)) if m else default
grf = geti("grf_count", 128)
slm = geti("slm_size")
barriers = geti("barrier_count")
if re.search(r"addrspace:\s+sampler", blk):
    barriers |= 0x80000000

cross = bytearray(128)
cross_size = 0
pay = blk.split("payload_arguments:")[1].split("per_thread_payload_arguments:")[0] if "payload_arguments:" in blk else ""
for m in re.finditer(r"- arg_type:\s+(\w+)\s+offset:\s+(\d+)\s+size:\s+(\d+)", pay):
    typ, off, size = m.group(1), int(m.group(2)), int(m.group(3))
    if size == 0:
        continue
    cross_size = max(cross_size, off + size)
    if typ in ("local_size", "enqueued_local_size"):
        struct.pack_into("<III", cross, off, lx, ly, lz)
    elif typ == "group_count":
        struct.pack_into("<III", cross, off, *[int(v) for v in os.environ.get("CMK_GC", "1,1,1").split(",")])
    elif typ == "work_dimensions":
        struct.pack_into("<I", cross, off, 3 if lz > 1 else (2 if ly > 1 else 1))
    elif typ not in ("arg_bypointer", "private_base_stateless"):
        print("warning: unhandled cross-thread arg", typ, "at", off, file=sys.stderr)
cross_size = (cross_size + 31) // 32 * 32
assert cross_size <= 128, cross_size
per = blk.split("per_thread_payload_arguments:")[1] if "per_thread_payload_arguments:" in blk else ""
per_size = 0
for m in re.finditer(r"- arg_type:\s+(\w+)\s+offset:\s+(\d+)\s+size:\s+(\d+)", per):
    per_size = max(per_size, int(m.group(2)) + int(m.group(3)))
    if m.group(1) != "packed_local_ids":
        print("warning: unhandled per-thread arg", m.group(1), file=sys.stderr)
grf_bytes = int(os.environ.get("CMK_GRF_BYTES", "64"))  # per-thread stride = one GRF: 64 bytes on Xe2/Xe3, 32 on Xe-HPG/Xe-LPG
per_size = (per_size + grf_bytes - 1) // grf_bytes * grf_bytes

btis = {}
if "binding_table_indices:" in blk:
    for m in re.finditer(r"bti_value:\s+(\d+)\s+arg_index:\s+(\d+)", blk.split("binding_table_indices:")[1]):
        btis[int(m.group(2))] = int(m.group(1))
bti_list = [btis[a] for a in sorted(btis)]
if surfs == "auto":
    surfs = [(k, 0) for k in range(len(bti_list))]      # surface k = k-th descriptor-table root parameter
if surfs and len(surfs) != len(bti_list):
    raise SystemExit(f"kernel has {len(bti_list)} surfaces, --surf gave {len(surfs)}")
ns = len(surfs)
bti = (bti_list + [0] * 24)[:24] if ns else [0] * 24
tab = [s[0] for s in surfs] + [0] * (24 - ns)
elem = [s[1] for s in surfs] + [0] * (24 - ns)

hdr = (b"CMK2" + struct.pack("<9I", len(code), grf, slm, barriers, lx, ly, lz, cross_size, per_size) + bytes(cross)
       + struct.pack("<4I", heap[0], heap[1], heap[2], ns) + struct.pack("<24I", *bti) + struct.pack("<24I", *tab) + struct.pack("<24I", *elem))
open(out, "wb").write(hdr + code)
print(f"{out}: code {len(code)} B, grf {grf}, slm {slm}, barriers {barriers & 0x7fffffff}{" +sampler" if barriers >> 31 else ""}, lws {lx}x{ly}x{lz}, cross {cross_size} B, per-thread {per_size} B, surfaces {ns} bti {bti_list}")
