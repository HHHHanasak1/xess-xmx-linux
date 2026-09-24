#pragma once
// GpuInfo: which Intel GPU the shim is running on, so that it can describe the device to XeSS like the Windows driver
// would (device info, XMX support, SIMD16 kernel variant). Wine lists the GPU under HKLM\System\CurrentControlSet\Enum\PCI.
struct XmxGpu
{
    unsigned deviceId;     // PCI device id, 0 if unknown
    const char* family;    // "Xe3-LPG", "Xe2-LPG", "Xe2-HPG", "Xe-HPG", "Xe-LPG", "Xe-LPG+"
    int gmdArch, gmdRel;   // graphics IP version (GMD ID)
    int gtGen;             // GTGeneration as XeSS reads it
    int eus, cores;        // execution units / Xe cores (typical SKU of the family; IGDEXT_EUS overrides)
    bool xmx;              // matrix engines present
    bool simd16;           // SIMD16-native (Xe2 and later): XeSS must hand out its SIMD16 kernel variant
    bool known;            // device id found in the table
};

const XmxGpu& XmxDetectGpu();
