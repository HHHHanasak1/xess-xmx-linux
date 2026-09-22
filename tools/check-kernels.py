#!/usr/bin/env python3
"""check-kernels.py <kernel dir>: every wg<x>_<y>_5.cmk must be a CMK2 file whose size is header (472 B) + code_size.
A file packed with an older cmk_pack (376 B header) makes the patched ANV print short read and silently run the no-op dummy instead of the kernel."""
import glob, os, struct, sys
d = sys.argv[1] if len(sys.argv) > 1 else "."
bad = 0
for f in sorted(glob.glob(os.path.join(d, "wg*_*_5.cmk"))):
    b = open(f, "rb").read()
    ok = b[:4] == b"CMK2" and len(b) == 472 + struct.unpack_from("<I", b, 4)[0]
    if not ok:
        bad += 1; print("BAD", os.path.basename(f), len(b))
print(len(glob.glob(os.path.join(d, "wg*_*_5.cmk"))), "kernels,", bad, "bad")
sys.exit(1 if bad else 0)
