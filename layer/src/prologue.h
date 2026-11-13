/* prologue.h - the code the layer gives the driver: prologue (payload into registers, see tools/prologue_asm.py) +
 * the kernel compiled with -noLoadPayload. 64-byte GRF encodings, one table per instruction set (Xe3, Xe2). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cmk.h"

/* the instruction set of the prologue (prologue_table.inc has one table each) */
enum xmx_isa {
   XMX_ISA_XE3,
   XMX_ISA_XE2,
   XMX_ISA_COUNT,
};

/* prologue of a kernel: malloc'd, *n bytes (a multiple of 16); NULL if the kernel cannot get one (work-group size
 * not a power of two in x/y, cross-thread data larger than two registers, unknown isa). */
uint8_t *xmx_prologue(const struct xmx_cml *k, enum xmx_isa isa, size_t *n);

/* prologue + kernel code, malloc'd */
uint8_t *xmx_full_code(const struct xmx_cml *k, enum xmx_isa isa, size_t *n);
