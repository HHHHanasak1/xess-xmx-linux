// ControlLib.c - a stand-in for the Intel Graphics Control Library runtime (ControlLib.dll) inside a Proton prefix.
// On Windows, Intel Graphics Software can override the number of frames XeSS frame generation generates: libxess_fg.dll
// asks ControlLib.dll for the 3D feature "frame generation" (CTL_3D_FEATURE_FRAME_GENERATION) when the game creates its
// frame generation context, and uses the answer as the maximum number of interpolated frames (XeSS-FG developer guide,
// "Interaction with Intel Graphics Software"). Wine has no ControlLib.dll, so the question fails and the game's own
// choice stands. This DLL answers only that question, from the game's environment:
//   XESS_FG=<n>      n x frame generation (2 = one interpolated frame, 4 = three); unset: the game's choice
//   XESS_FG_LOG=1    log every call to C:\igcl_stub.log (also with IGDEXT_TRACE=1)
// Devices are the Intel adapters DXGI lists, identified by their LUID. Every other function is not implemented.
// The structures below follow the layout of Intel's public igcl_api.h (github.com/intel/drivers.gpu.control-library).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef uint32_t ctl_result_t;
#define CTL_RESULT_SUCCESS                   0x00000000u
#define CTL_RESULT_ERROR_NOT_IMPLEMENTED     0x40000015u
#define CTL_RESULT_ERROR_UNSUPPORTED_FEATURE 0x4000000au
#define CTL_RESULT_ERROR_INVALID_NULL_POINTER 0x40000017u

#define CTL_3D_FEATURE_FRAME_GENERATION 17
#define CTL_PROPERTY_VALUE_TYPE_ENUM    4
#define CTL_DEVICE_TYPE_GRAPHICS        1
#define CTL_MAKE_VERSION(major, minor)  ((((uint32_t)(major)) << 16) | (((uint32_t)(minor)) & 0x0000ffff))

typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } ctl_application_id_t;
typedef struct {
    uint32_t Size; uint8_t Version;
    uint32_t AppVersion; uint32_t flags; uint32_t SupportedVersion;
    ctl_application_id_t ApplicationUID;
} ctl_init_args_t;

typedef union {
    struct { bool Enable; } BoolType;
    struct { bool Enable; float Value; } FloatType;
    struct { bool Enable; int32_t Value; } IntType;
    struct { uint32_t EnableType; } EnumType;
    struct { bool Enable; uint32_t Value; } UIntType;
} ctl_property_t;

typedef struct {
    uint32_t Size; uint8_t Version;
    uint32_t FeatureType;
    char* ApplicationName; int8_t ApplicationNameLength;
    bool bSet;
    uint32_t ValueType;
    ctl_property_t Value;
    int32_t CustomValueSize; void* pCustomValue;
} ctl_3d_feature_getset_t;

typedef struct { uint64_t major_version, minor_version, build_number; } ctl_firmware_version_t;
typedef struct {
    uint32_t Size; uint8_t Version;
    void* pDeviceID; uint32_t device_id_size;
    uint32_t device_type; uint32_t supported_subfunction_flags;
    uint64_t driver_version; ctl_firmware_version_t firmware_version;
    uint32_t pci_vendor_id, pci_device_id, rev_id;
    uint32_t num_eus_per_sub_slice, num_sub_slices_per_slice, num_slices;
    char name[100];
    // later members (Version > 0) are left as the caller set them
} ctl_device_adapter_properties_t;

typedef struct { int magic; } api_t;
typedef struct { int magic; LUID luid; uint32_t vendor, device, rev; char name[100]; } dev_t;

static api_t g_api = { 0x49474341 };
static dev_t g_devs[4];
static uint32_t g_ndev = 0;
static FILE* g_log = NULL;
static int g_logInit = 0;

static void say(const char* fmt, ...)
{
    if (!g_logInit)
    {
        char b[8];
        g_logInit = 1;
        if (GetEnvironmentVariableA("XESS_FG_LOG", b, sizeof(b)) > 0 || GetEnvironmentVariableA("IGDEXT_TRACE", b, sizeof(b)) > 0)
            g_log = fopen("C:\\igcl_stub.log", "a");
    }
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// XESS_FG=<n> -> override value n-1 (2x = 1 ... 4x = 3); 0 = the application's choice. XeSS-FG accepts 1..3 (Intel's
// public enum ends at 4x) and ignores anything else, so more than 4x is handed over as 4x; the shim raises it further
// (igdext64.dll, XefgHook). The shim also sets XESS_FG_PROBE in the process while it creates a probe context.
static uint32_t override_value(void)
{
    char b[16];
    if (GetEnvironmentVariableA("XESS_FG_PROBE", b, sizeof(b)) > 0) return (uint32_t) atoi(b);
    if (GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) == 0) return 0;
    int n = atoi(b);
    if (n < 2 || n > 16) return 0;
    return n > 4 ? 3 : (uint32_t)(n - 1);
}

static void enum_devices(void)
{
    if (g_ndev) return;
    IDXGIFactory1* f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&f))) return;
    IDXGIAdapter1* a;
    for (UINT i = 0; g_ndev < 4 && f->lpVtbl->EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->lpVtbl->GetDesc1(a, &d)) && d.VendorId == 0x8086 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            dev_t* v = &g_devs[g_ndev++];
            v->magic = 0x49474344; v->luid = d.AdapterLuid;
            v->vendor = d.VendorId; v->device = d.DeviceId; v->rev = d.Revision;
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, v->name, sizeof(v->name), NULL, NULL);
            say("  device %u: %04x:%04x '%s' luid %08lx:%08lx", g_ndev - 1, v->vendor, v->device, v->name,
                (unsigned long)v->luid.HighPart, (unsigned long)v->luid.LowPart);
        }
        a->lpVtbl->Release(a);
    }
    f->lpVtbl->Release(f);
}

__declspec(dllexport) ctl_result_t ctlInit(ctl_init_args_t* args, void** api)
{
    if (!args || !api) return CTL_RESULT_ERROR_INVALID_NULL_POINTER;
    say("ctlInit(app version %u.%u, flags %x) XESS_FG override %u", args->AppVersion >> 16, args->AppVersion & 0xffff,
        args->flags, override_value());
    args->SupportedVersion = CTL_MAKE_VERSION(1, 1);
    *api = &g_api;
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) ctl_result_t ctlClose(void* api)
{
    say("ctlClose");
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) ctl_result_t ctlSetRuntimePath(void* args)
{
    say("ctlSetRuntimePath");
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) ctl_result_t ctlEnumerateDevices(void* api, uint32_t* count, void** devices)
{
    if (!count) return CTL_RESULT_ERROR_INVALID_NULL_POINTER;
    enum_devices();
    say("ctlEnumerateDevices(count in %u, list %p) -> %u", *count, (void*)devices, g_ndev);
    if (devices)
        for (uint32_t i = 0; i < g_ndev && i < *count; i++) devices[i] = &g_devs[i];
    *count = devices && *count < g_ndev ? *count : g_ndev;
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) ctl_result_t ctlGetDeviceProperties(void* device, ctl_device_adapter_properties_t* p)
{
    dev_t* d = (dev_t*)device;
    if (!d || !p) return CTL_RESULT_ERROR_INVALID_NULL_POINTER;
    say("ctlGetDeviceProperties(size %u, version %u, id buffer %p/%u)", p->Size, p->Version, p->pDeviceID, p->device_id_size);
    if (p->pDeviceID && p->device_id_size >= sizeof(LUID)) memcpy(p->pDeviceID, &d->luid, sizeof(LUID));
    p->device_type = CTL_DEVICE_TYPE_GRAPHICS;
    p->pci_vendor_id = d->vendor; p->pci_device_id = d->device; p->rev_id = d->rev;
    memcpy(p->name, d->name, sizeof(p->name));
    return CTL_RESULT_SUCCESS;
}

__declspec(dllexport) ctl_result_t ctlGetSet3DFeature(void* device, ctl_3d_feature_getset_t* f)
{
    if (!f) return CTL_RESULT_ERROR_INVALID_NULL_POINTER;
    char app[128] = "";
    if (f->ApplicationName && f->ApplicationNameLength > 0)
        snprintf(app, sizeof(app), "%.*s", (int)f->ApplicationNameLength, f->ApplicationName);
    if (f->FeatureType == CTL_3D_FEATURE_FRAME_GENERATION && !f->bSet)
    {
        f->ValueType = CTL_PROPERTY_VALUE_TYPE_ENUM;
        f->Value.EnumType.EnableType = override_value();
        say("ctlGetSet3DFeature(get frame generation, app '%s', size %u, version %u) -> %u", app, f->Size, f->Version,
            f->Value.EnumType.EnableType);
        return CTL_RESULT_SUCCESS;
    }
    say("ctlGetSet3DFeature(%s feature %u, app '%s', value type %u) -> unsupported", f->bSet ? "set" : "get",
        f->FeatureType, app, f->ValueType);
    return CTL_RESULT_ERROR_UNSUPPORTED_FEATURE;
}

__declspec(dllexport) ctl_result_t ctlGetSupported3DCapabilities(void* device, void* caps)
{
    say("ctlGetSupported3DCapabilities -> not implemented");
    return CTL_RESULT_ERROR_NOT_IMPLEMENTED;
}

__declspec(dllexport) ctl_result_t ctlCheckDriverVersion(void* device, uint32_t version)
{
    say("ctlCheckDriverVersion(%u.%u)", version >> 16, version & 0xffff);
    return CTL_RESULT_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
