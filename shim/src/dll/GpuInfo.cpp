#include "Stdafx.h"
#include "Trace.h"
#include "GpuInfo.h"
#include <cstdio>
#include <cstdlib>

namespace
{
struct Range { unsigned lo, hi; XmxGpu gpu; };

// Device id ranges per family (Linux i915/xe PCI id lists). EU/core counts are the largest SKU of the family.
const Range kRanges[] = {
    { 0xB080, 0xB0FF, { 0, "Xe3-LPG", 30, 0, 30, 96, 12, true,  true,  true } },   // Panther Lake
    { 0xFD80, 0xFD8F, { 0, "Xe3-LPG", 30, 0, 30, 96, 12, true,  true,  true } },   // Wildcat Lake
    { 0x6420, 0x64FF, { 0, "Xe2-LPG", 20, 4, 20, 64,  8, true,  true,  true } },   // Lunar Lake
    { 0xE200, 0xE2FF, { 0, "Xe2-HPG", 20, 1, 20, 160, 20, true, true,  true } },   // Battlemage
    { 0x5690, 0x56FF, { 0, "Xe-HPG", 12, 55, 12, 512, 32, true, false, true } },   // Alchemist (DG2)
    { 0x7D51, 0x7D51, { 0, "Xe-LPG+", 12, 74, 12, 128, 8, true, false, true } },   // Arrow Lake-H
    { 0x7DD1, 0x7DD1, { 0, "Xe-LPG+", 12, 74, 12, 128, 8, true, false, true } },
    { 0x7D40, 0x7DFF, { 0, "Xe-LPG", 12, 70, 12, 128, 8, false, false, true } },   // Meteor / Arrow Lake
};

XmxGpu Detect()
{
    XmxGpu g = kRanges[0].gpu;   // default: Panther Lake, the tested device
    g.known = false;
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        char name[256];
        for (DWORD i = 0;; ++i)
        {
            DWORD n = sizeof(name);
            if (RegEnumKeyExA(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            unsigned ven = 0, dev = 0;
            if (sscanf(name, "VEN_%x&DEV_%x", &ven, &dev) != 2 || ven != 0x8086) continue;
            for (const Range& r : kRanges)
                if (dev >= r.lo && dev <= r.hi) { g = r.gpu; g.known = true; break; }
            g.deviceId = dev;
            if (g.known) break;
        }
        RegCloseKey(k);
    }
    // explicit overrides (experiments, or a device the table does not know)
    char b[64];
    if (GetEnvironmentVariableA("IGDEXT_GMD", b, sizeof(b)) > 0) sscanf(b, "%d,%d", &g.gmdArch, &g.gmdRel);
    if (GetEnvironmentVariableA("IGDEXT_GTGEN", b, sizeof(b)) > 0) g.gtGen = atoi(b);
    if (GetEnvironmentVariableA("IGDEXT_EUS", b, sizeof(b)) > 0) sscanf(b, "%d,%d", &g.eus, &g.cores);
    TraceF("GPU: PCI 8086:%04x -> %s%s (GMD %d.%d, XMX %d, SIMD16 %d)", g.deviceId, g.family, g.known ? "" : " (unknown id, assumed)",
           g.gmdArch, g.gmdRel, (int)g.xmx, (int)g.simd16);
    return g;
}
}

const XmxGpu& XmxDetectGpu()
{
    static const XmxGpu g = Detect();
    return g;
}
