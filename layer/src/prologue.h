/* prologue.h - the code the layer gives the driver: prologue (payload into registers, see tools/prologue_asm.py) +
 * the kernel compiled with -noLoadPayload. Xe3 only for now (64-byte GRF encodings). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cmk.h"

/* prologue of a kernel: malloc'd, *n bytes (a multiple of 16); NULL if the kernel cannot get one (work-group size
 * not a power of two in x/y, cross-thread data larger than two registers). */
uint8_t *xmx_prologue(const struct xmx_cml *k, size_t *n);

/* prologue + kernel code, malloc'd */
uint8_t *xmx_full_code(const struct xmx_cml *k, size_t *n);
