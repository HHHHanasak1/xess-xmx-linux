#!/usr/bin/env python3
"""check_dyn_ids.py <anv patch> [dyn_dummies.inc]
Consistency check between the three places that define the placeholder id layout of run-time compiled kernels:
the driver (anv_cm_dyn_planes[] + anv_cm_dyn_id() in the ANV patch), the generator (tools/gen_dyn_dummies.py) and the
generated shim table (shim/src/dll/dyn_dummies.inc). Exits non-zero on any mismatch."""
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import gen_dyn_dummies as gen  # noqa: E402

patch = open(sys.argv[1], encoding="utf-8").read()
inc = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "shim", "src", "dll", "dyn_dummies.inc")

m = re.search(r"anv_cm_dyn_planes\[\]\s*=\s*\{([^}]*)\}", patch)
if not m:
    sys.exit("anv_cm_dyn_planes not found in the patch")
planes = tuple(int(v) for v in m.group(1).replace("+", "").split(",") if v.strip())
if planes != tuple(gen.PLANES):
    sys.exit("planes differ: driver %s, generator %s" % (planes, gen.PLANES))


def driver_id(wx, wy, wz):
    """Python port of anv_cm_dyn_id() in the ANV patch."""
    base = 0
    for z in planes:
        lim = 1024 // z
        if wz == z:
            if wx < 1 or wy < 1 or wx * wy > lim:
                return -1
            return base + sum(lim // y for y in range(1, wy)) + wx - 1
        base += sum(lim // y for y in range(1, lim + 1))
    return -1


expected = list(gen.ids())
for i, (x, y, z) in enumerate(expected):
    if driver_id(x, y, z) != i:
        sys.exit("id %d = %s maps back to %d in the driver" % (i, (x, y, z), driver_id(x, y, z)))

table = re.findall(r"\{ kDynDummy(\d+), sizeof\(kDynDummy\d+\) \},\s*// (\d+),(\d+),(\d+)", open(inc, encoding="utf-8").read())
if len(table) != len(expected):
    sys.exit("shim table has %d entries, generator %d" % (len(table), len(expected)))
for (i, x, y, z), e in zip(table, expected):
    if (int(x), int(y), int(z)) != e:
        sys.exit("shim entry %s is %s, expected %s" % (i, (x, y, z), e))
print("ok: %d placeholder ids on planes %s" % (len(expected), planes))
