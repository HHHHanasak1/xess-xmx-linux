// LdrNotify.h - ntdll's LdrRegisterDllNotification: the loader calls a function for every DLL as it is mapped, before
// the game can call into it. Not in the SDK headers; the structures are documented in the Windows SDK (ntddk) and
// implemented by Wine. Used by XellDriver.cpp (the XeLL probe) and XellHook.cpp (the hook threads).
#pragma once
#include <windows.h>
#include <winternl.h>   // UNICODE_STRING
#include <cwchar>

struct LdrDllNotificationData { ULONG Flags; const UNICODE_STRING* FullDllName; const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
enum { kLdrDllLoaded = 1 };   // LDR_DLL_NOTIFICATION_REASON_LOADED
typedef VOID (CALLBACK* PFN_LdrDllNotification)(ULONG reason, const LdrDllNotificationData* data, PVOID ctx);
typedef NTSTATUS (NTAPI* PFN_LdrRegisterDllNotification)(ULONG flags, PFN_LdrDllNotification fn, PVOID ctx, PVOID* cookie);
typedef NTSTATUS (NTAPI* PFN_LdrUnregisterDllNotification)(PVOID cookie);

// a DLL's base name as the loader gives it against `name`, whatever the case
inline bool LdrDllNameIs(const UNICODE_STRING* s, const wchar_t* name)
{
    const size_t n = wcslen(name);
    return s && s->Buffer && s->Length == n * sizeof(wchar_t) && _wcsnicmp(s->Buffer, name, n) == 0;
}
