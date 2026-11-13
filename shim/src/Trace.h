#pragma once
#include <windows.h>
#include <cstdarg>
bool TraceOn();   // IGDEXT_TRACE set: work done only for the trace (call stacks, string copies) can be skipped otherwise
void TraceF(const char* fmt, ...);
// one line "hh:mm:ss.mmm [thread id] <text>" appended to the file (opened and closed for each line); the shim's logs
// (igdext_trace.log, igdext_xell.log, igdext_xefg.log) are all written with it
void AppendLogLine(const char* path, const char* fmt, va_list ap);
// trace mode only: logs the modules on the current call stack (outermost last), e.g. to tell XeSS SR from XeSS-FG callers
void TraceCallers(const char* tag);
// writes a blob into C:\igdext_dump\<name>; returns true on success
bool DumpBlob(const char* name, const void* data, size_t size);
const char* SafeStr(const void* p, char* buf, size_t cap);
