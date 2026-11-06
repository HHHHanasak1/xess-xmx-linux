#!/usr/bin/env python3
"""layout_probe.py <dir> - work out ANV's serialized compute shader layout from pipeline binaries of the variant
shaders (tests/variants, exported with `probe export`). Prints sizes and offsets only.

1. the payload's structure sizes: the serialization is walked with candidate sizes until every variant ends exactly
   at its last byte with sensible counts (stage = compute, one or more statistics, surfaces of set 7, ...);
2. prog_data fields from what changes between variants: program_size = code length, local_size = 8,4,2 in ls842,
   total_shared = 1024 / 4096, the barrier byte, the sampler byte;
3. the binding table entries and the inline data of the group-count variant (numwg).
Checked against Mesa 26.1.2, whose layout is known (src/layout261.h)."""
import os
import struct
import sys

VARIANTS = ['base', 'ls842', 'ls1611', 'shared256', 'shared1024', 'barrier', 'sample', 'numwg', 'ls3211', 'barrier32']


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


class Rd:
    def __init__(self, d):
        self.d, self.p, self.bad = d, 0, False

    def align(self, a):
        self.p = (self.p + a - 1) & ~(a - 1)

    def take(self, n):
        if n < 0 or self.p + n > len(self.d):
            self.bad = True
            return b'\0' * max(n, 0)
        x = self.d[self.p:self.p + n]
        self.p += n
        return x

    def u32(self):
        self.align(4)
        return struct.unpack('<I', self.take(4))[0]

    def u16(self):
        self.align(2)
        return struct.unpack('<H', self.take(2))[0]

    def u8(self):
        return self.take(1)[0]


def walk(d, L):
    """the 26.1 field order with sizes from L; returns the parsed pieces or None"""
    r = Rd(d)
    out = {}
    out['stage'] = r.u32()
    out['code_len'] = r.u32()
    out['code_off'] = r.p
    r.take(out['code_len'])
    out['pd_off'] = r.p
    pd = r.take(L['pd'])
    out['pd'] = pd
    nrel = u32(pd, L['nrel']) if L['nrel'] + 4 <= len(pd) else 999
    if nrel > 64:
        return None
    r.take(nrel * L['rel'])
    out['nstats'] = r.u32()
    if not 1 <= out['nstats'] <= 8:
        return None
    r.take(out['nstats'] * L['stats'])
    xfb = r.u32()
    if xfb > 4096:
        return None
    r.take(xfb)
    out['inst_mult'], out['used_desc'], out['fully_promoted'] = r.u32(), r.u32(), r.u32()
    r.u8()
    r.take(L['b3'])
    out['layout_type'], out['binding_mask'] = r.u16(), r.u16()
    out['nsurf'], out['nsamp'] = r.u8(), r.u8()
    out['nemb'] = r.u16()
    out['surf'] = [r.take(L['bind']) for _ in range(out['nsurf'])]
    out['samp'] = [r.take(L['bind']) for _ in range(out['nsamp'])]
    r.take(out['nemb'] * L['emb'])
    r.take(L['inatt'])
    r.take(L['pushr'])
    out['ninl'] = r.u8()
    if out['ninl'] > 16:
        return None
    out['inl'] = list(r.take(out['ninl']))
    out['used_sets'] = (r.u8(), r.u8(), r.u8())
    r.take(L['dyn'])
    r.u8()
    if r.bad or r.p != len(d):
        return None
    return out


def search(blobs):
    """structure sizes that walk every variant exactly"""
    base = dict(rel=16, b3=96, emb=40, bind=16, inatt=36, pushr=32, dyn=32)
    hits = []
    for pd in range(64, 513, 4):
        for nrel in (40, 44, 48, 52, 56, 60, 64):
            if nrel >= pd:
                continue
            for stats in range(16, 257, 4):
                L = dict(base, pd=pd, nrel=nrel, stats=stats)
                res = [walk(b, L) for b in blobs.values()]
                if all(res) and all(x['stage'] == 5 for x in res) and all(x['code_len'] % 16 == 0 for x in res):
                    hits.append((L, res))
    return hits


def main():
    dd = sys.argv[1]
    blobs = {v: open(os.path.join(dd, v + '.pb'), 'rb').read() for v in VARIANTS if os.path.exists(os.path.join(dd, v + '.pb'))}
    print(f'{len(blobs)} variants, sizes ' + ' '.join(f'{k}:{len(v)}' for k, v in blobs.items()))
    hits = search(blobs)
    print(f'structure candidates: {len(hits)}')
    if not hits:
        return 1
    # the candidates differ only in where num_relocs is read (all zero); keep the smallest prog_data / stats pairs
    sizes = sorted({(L['pd'], L['stats']) for L, _ in hits})
    print('prog_data size / statistics size candidates:', sizes[:8])
    want = int(os.environ.get('LP_PD', '0'))
    L, res = next(((l, r) for l, r in hits if l['pd'] == want), hits[0])
    print('using prog_data size', L['pd'], 'statistics size', L['stats'], 'num_relocs read at', L['nrel'])
    R = dict(zip(blobs.keys(), res))
    pdsz = L['pd']
    # 2. prog_data fields
    def offs(pred):
        return [o for o in range(0, pdsz - 3, 4) if pred(o)]
    pdv = {k: v['pd'] for k, v in R.items()}
    clen = {k: v['code_len'] for k, v in R.items()}
    print('program_size at', offs(lambda o: all(u32(pdv[k], o) == clen[k] for k in pdv)))
    print('local_size at', offs(lambda o: u32(pdv['ls842'], o) == 8 and u32(pdv['ls842'], o + 4) == 4 and
                                  u32(pdv['ls842'], o + 8) == 2 and u32(pdv['base'], o) == 1))
    print('total_shared at', offs(lambda o: u32(pdv['shared256'], o) == 1024 and u32(pdv['shared1024'], o) == 4096 and
                                    u32(pdv['base'], o) == 0))
    bytes_ = lambda pred: [o for o in range(pdsz) if pred(o)]
    print('barrier byte at', bytes_(lambda o: pdv['barrier32'][o] == 1 and pdv['ls3211'][o] == 0))
    print('sampler byte at', bytes_(lambda o: pdv['sample'][o] == 1 and pdv['base'][o] == 0 and pdv['barrier'][o] == 0))
    print('stage at', offs(lambda o: all(u32(pdv[k], o) == 5 for k in pdv)))
    # fields that differ between base and numwg only (inline data / push related)
    print('numwg-only bytes', [o for o in range(pdsz) if pdv['numwg'][o] != pdv['base'][o]][:24])
    # 3. bind map
    for k in ('base', 'sample', 'numwg'):
        x = R[k]
        ents = [(e[12], u32(e, 4), u32(e, 0), u32(e, 8), e[13]) for e in x['surf']]
        sents = [(e[12], u32(e, 4), u32(e, 0), u32(e, 8)) for e in x['samp']]
        print(f'{k}: layout_type {x["layout_type"]} binding_mask {x["binding_mask"]:#x} surfaces {ents} samplers {sents} '
              f'inline {x["inl"]} used_sets {x["used_sets"]} stats {x["nstats"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
