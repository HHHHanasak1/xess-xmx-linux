#include "Stdafx.h"
#include "Trace.h"
#include "ExtensionVersions.h"
#include "dll/GpuInfo.h"
extern "C" bool XmxFallbackActive(struct ID3D12Device* dev);   // dll/D3D12Api.cpp

// Highest version we claim: HW feature level 5 (Xe2 and newer), API version 20
static const INTCExtensionVersion c_MaxD3D12ExtVersion = {EXTENSION_HW_FEATURE_LEVEL_5, EXTENSION_API_VERSION_20, EXTENSION_REVISION_0};

HRESULT D3D12ExtensionContext::GetSupportedVersions(const void* pDevice, INTCExtensionVersionHelper* driverExtensionVersion)
{
    m_pAppDevice = const_cast<ID3D12Device*>(reinterpret_cast<const ID3D12Device*>(pDevice));
    *driverExtensionVersion = c_MaxD3D12ExtVersion;
    return S_OK;
}

HRESULT D3D12ExtensionContext::InitExtensions(const void* pDevice, void** ppfnExtensionFuncs, UINT32 extensionFuncsSize, INTCExtensionInfo1* pExtensionInfo, INTCExtensionAppInfo1* pExtensionAppInfo, bool internalExtensions)
{
    (void)ppfnExtensionFuncs; (void)extensionFuncsSize;
    TraceF("D3D12 InitExtensions: requested HW=%u API=%u REV=%u internal=%d device=%p",
           pExtensionInfo->RequestedExtensionVersion.HWFeatureLevel, pExtensionInfo->RequestedExtensionVersion.APIVersion,
           pExtensionInfo->RequestedExtensionVersion.Revision, (int)internalExtensions, pDevice);
    TraceCallers("InitExtensions");
    if (XmxFallbackActive(reinterpret_cast<ID3D12Device*>(const_cast<void*>(pDevice))))
    {
        // behave like a system without the Intel extension: XeSS takes its DP4a path and XeSS frame generation its generic
        // path (answering LSCSupported=0 instead would switch frame generation off altogether)
        TraceF("  context declined (fallback): XeSS and XeFG use their generic paths");
        return E_NOTIMPL;
    }
    if (pExtensionAppInfo)
    {
        char a[128] = {}, e[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, pExtensionAppInfo->pApplicationName ? pExtensionAppInfo->pApplicationName : L"", -1, a, sizeof(a) - 1, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, pExtensionAppInfo->pEngineName ? pExtensionAppInfo->pEngineName : L"", -1, e, sizeof(e) - 1, nullptr, nullptr);
        TraceF("  app='%s' engine='%s'", a, e);
    }
    if (pExtensionInfo->RequestedExtensionVersion.HWFeatureLevel == 0 && pExtensionInfo->RequestedExtensionVersion.APIVersion == 0)
        return E_NOINTERFACE;

    INTCExtensionVersionHelper driver(c_MaxD3D12ExtVersion);
    if (!driver.IsSupported(pExtensionInfo->RequestedExtensionVersion))
    {
        TraceF("  requested version NOT supported by this build");
        return E_NOINTERFACE;
    }
    m_pAppDevice = const_cast<ID3D12Device*>(reinterpret_cast<const ID3D12Device*>(pDevice));
    m_SupportedExtVersion = pExtensionInfo->RequestedExtensionVersion;

    INTCDeviceInfo1& di = pExtensionInfo->IntelDeviceInfo;
    di = {};
    // the detected GPU (GpuInfo.cpp); overrides: IGDEXT_GMD=<arch>,<release>  IGDEXT_GTGEN=<n>  IGDEXT_EUS=<eus>,<cores>  IGDEXT_GTNAME=<name>
    const XmxGpu& gpu = XmxDetectGpu();
    const int gmdArch = gpu.gmdArch, gmdRel = gpu.gmdRel, gtGen = gpu.gtGen, eus = gpu.eus, cores = gpu.cores;
    char gtName[64];
    if (GetEnvironmentVariableA("IGDEXT_GTNAME", gtName, sizeof(gtName)) == 0) strcpy_s(gtName, gpu.family);
    { wchar_t wn[64]; size_t n = 0; mbstowcs_s(&n, wn, gtName, _TRUNCATE); wcsncpy_s(di.GTGenerationName, wn, _TRUNCATE); }
    di.GPUMaxFreq = 2500; di.GPUMinFreq = 300; di.GTGeneration = gtGen; di.EUCount = eus; di.PackageTDP = 25; di.MaxFillRate = 32;
    di.GMDID = INTC_GMD_ID(gmdArch, gmdRel); di.XeCoresCount = cores;
    TraceF("  device info: GMD %d.%d gen %d name %s EUs %d cores %d", gmdArch, gmdRel, gtGen, gtName, eus, cores);
    pExtensionInfo->pDeviceDriverDesc       = L"Intel(R) Graphics (xess-xmx-linux)";
    pExtensionInfo->pDeviceDriverVersion    = L"32.0.101.9999";
    pExtensionInfo->DeviceDriverBuildNumber = 9999;
    TraceF("  context created (granted)");
    return S_OK;
}
