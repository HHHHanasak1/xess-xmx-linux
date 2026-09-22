#!/usr/bin/env python3
# s16_map.py <dumpdir> <mapfile>: unique XeSS kernels of a dump (same hash as the igdext shim: FNV-1a over SPIR-V + options)
# -> mapfile lines: id dumpindex hash name lws_x lws_y lws_z typed
import os, re, sys, glob
d, out = sys.argv[1], sys.argv[2]
def fnv(a, b):
    h = 14695981039346656037
    for x in a + b:
        h ^= x; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
seen = {}; rows = []
for f in sorted(glob.glob(os.path.join(d, "cs_*_type2.bin")), key=lambda p: int(re.search(r"cs_(\d+)_", p).group(1))):
    n = int(re.search(r"cs_(\d+)_", f).group(1))
    blob = open(f, "rb").read()
    o = os.path.join(d, "cs_%04d_options.txt" % n)
    opts = open(o, "rb").read() if os.path.exists(o) else b""
    h = fnv(blob, opts)
    if h in seen: continue
    seen[h] = len(rows)
    name = re.search(rb"(input_processing|output_filter|average_sum|average|clear_pad_area|cm_[A-Za-z0-9_]+|[a-z_]+_kernel)", blob)
    name = name.group(1).decode() if name else "?"
    m = re.search(rb"LWS_SIZE_X=(\d+) -DLWS_SIZE_Y=(\d+) -DLWS_SIZE_Z=(\d+)", opts)
    lx, ly, lz = (m.groups() if m else (b"1", b"1", b"1"))
    typed = blob.count(b"gather4.typed") + blob.count(b"scatter4.typed")
    rows.append((len(rows), n, h, name, int(lx), int(ly), int(lz), typed))
with open(out, "w") as fo:
    for r in rows: fo.write("%d %d %016x %s %d %d %d %d\n" % r)
print("unique kernels:", len(rows), " legacy-typed:", sum(1 for r in rows if r[7]))
names = {}
for r in rows: names[r[3]] = names.get(r[3], 0) + 1
print("names:", sorted(names.items(), key=lambda x: -x[1])[:12])
