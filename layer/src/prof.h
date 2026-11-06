/* prof.h - GPU time of the replaced kernels (XMX_LAYER_PROF=1), reported to the layer's log. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*xprof_log_fn)(const char *fmt, ...);

bool xprof_enabled(void);
/* 1: a time stamp pair around every dispatch (per kernel, the dispatches serialised); 2: one pair around every run of
 * replaced dispatches in a command buffer (the run as it executes: kernels, barriers, copies in between) */
int xprof_mode(void);
/* the statistics slot of a kernel, by its hash; -1 if the table is full */
int xprof_kernel(const char *hash);
/* one dispatch of kernel slot k took ns nanoseconds on the GPU */
void xprof_add(int k, uint64_t ns);
/* n dispatches whose time stamps were not ready or not recorded */
void xprof_lost(unsigned n);
/* a present; reports the last interval when it is older than the report period (XMX_LAYER_PROF_SEC, default 5) */
void xprof_present(xprof_log_fn log);
/* everything since the start */
void xprof_final(xprof_log_fn log);
