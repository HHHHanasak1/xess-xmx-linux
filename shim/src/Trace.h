#pragma once
#include <windows.h>
#include <cstdarg>
void TraceF(const char* fmt, ...);
// trace mode only: logs the modules on the current call stack (outermost last), e.g. to tell XeSS SR from XeSS-FG callers
void TraceCallers(const char* tag);
// writes a blob into C:\igdext_dump\<name>; returns true on success
bool DumpBlob(const char* name, const void* data, size_t size);
const char* SafeStr(const void* p, char* buf, size_t cap);
