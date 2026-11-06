#!/usr/bin/env python3
"""layout_bytes.py <dir> <prog_data size> <from> <to> - the prog_data bytes from..to of each variant's pipeline binary,
as numbers, side by side (for the flag bytes layout_probe.py cannot tell apart by itself)."""
import os
import struct
import sys

d, pdsz, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
names = ['base', 'ls842', 'ls3211', 'barrier32', 'sample', 'numwg', 'shared256']
pds = {}
for n in names:
    x = open(os.path.join(d, n + '.pb'), 'rb').read()
    clen = struct.unpack_from('<I', x, 4)[0]
    pds[n] = x[8 + clen:8 + clen + pdsz]
print('off  ' + ' '.join(f'{n[:9]:>9}' for n in names))
for o in range(a, b, 4):
    print(f'{o:3d}  ' + ' '.join(f'{pds[n][o:o + 4].hex():>9}' for n in names))
