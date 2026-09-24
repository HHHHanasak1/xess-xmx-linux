#include "Stdafx.h"
#include "Trace.h"
#include "ExtensionVersions.h"

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
    // experiment switches: IGDEXT_GMD=<arch>,<release>  IGDEXT_GTGEN=<n>  IGDEXT_GTNAME=<ascii name>  IGDEXT_EUS=<eu count>,<xe cores>
    int gmdArch = 30, gmdRel = 0, gtGen = 30, eus = 96, cores = 12; char gtName[64] = "Xe3-LPG"; char eb[64];
    if (GetEnvironmentVariableA("IGDEXT_GMD", eb, sizeof(eb)) > 0) sscanf(eb, "%d,%d", &gmdArch, &gmdRel);
    if (GetEnvironmentVariableA("IGDEXT_GTGEN", eb, sizeof(eb)) > 0) gtGen = atoi(eb);
    if (GetEnvironmentVariableA("IGDEXT_GTNAME", gtName, sizeof(gtName)) == 0) strcpy_s(gtName, "Xe3-LPG");
    if (GetEnvironmentVariableA("IGDEXT_EUS", eb, sizeof(eb)) > 0) sscanf(eb, "%d,%d", &eus, &cores);
    { wchar_t wn[64]; size_t n = 0; mbstowcs_s(&n, wn, gtName, _TRUNCATE); wcsncpy_s(di.GTGenerationName, wn, _TRUNCATE); }
    di.GPUMaxFreq = 2500; di.GPUMinFreq = 300; di.GTGeneration = gtGen; di.EUCount = eus; di.PackageTDP = 25; di.MaxFillRate = 32;
    di.GMDID = INTC_GMD_ID(gmdArch, gmdRel); di.XeCoresCount = cores;
    TraceF("  device info: GMD %d.%d gen %d name %s EUs %d cores %d", gmdArch, gmdRel, gtGen, gtName, eus, cores);
    pExtensionInfo->pDeviceDriverDesc       = L"Intel(R) Graphics (Wine tracing igdext)";
    pExtensionInfo->pDeviceDriverVersion    = L"32.0.101.9999";
    pExtensionInfo->DeviceDriverBuildNumber = 9999;
    TraceF("  context created (granted)");
    return S_OK;
}
