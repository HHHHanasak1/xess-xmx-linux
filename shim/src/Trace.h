#pragma once
#include <windows.h>
#include <cstdarg>
void TraceF(const char* fmt, ...);
// writes a blob into C:\igdext_dump\<name>; returns true on success
bool DumpBlob(const char* name, const void* data, size_t size);
const char* SafeStr(const void* p, char* buf, size_t cap);
