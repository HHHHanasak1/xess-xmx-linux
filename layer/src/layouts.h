/* layouts.h - ANV's serialized compute shader, one table per Mesa minor version. The values are measured, not taken
 * from Mesa's sources: 26.1 in E3 of the decoupling work, 26.3 with tests/layout_probe.py on pipeline binaries of the
 * variant shaders (tests/variants) exported by an unpatched 26.3.0-rc3 (`probe export`). A version not in the list is
 * not served (the shim's self-test stays unanswered, XeSS uses its generic paths).
 * This project's patched drivers (patches/) serialize the same layout with prog_data longer by the C-for-Metal
 * fields of brw_cs_prog_data (pd_size_patched; every offset in the table is in front of them): measured the same way
 * on patched 26.1.2 (656) and 26.3.0 (664). The template tells which one a driver is (layer.c, template_get). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct anv_layout {
   const char *version;         /* driverInfo prefix, e.g. "Mesa 26.1." */
   uint32_t pd_size, pd_size_patched, stats_size, reloc_size, emb_size, input_att_size, push_ranges_size, dyn_desc_size, max_inline;
   /* brw_stage_prog_data */
   uint32_t stage, total_scratch, total_shared, program_size, num_relocs, grf_used, source_hash, source_hash_size;
   /* brw_cs_prog_data */
   uint32_t local_size, prog_offset, prog_mask, prog_spilled, uses_barrier, generate_local_id, walk_order, uses_sampler;
   int uses_inline_push_addr;   /* -1: none (26.3: inline data follows the bind map's inline dword count) */
   int uses_systolic;           /* -1: not located (left as the template has it) */
   bool zero_prog_data;         /* start from a zeroed prog_data (26.1, as tested) or from the template's (26.3) */
   int null_set;                /* set id of the null surface for unused binding-table slots; -1: not measured, the
                                 * slots get the kernel's first surface instead (never accessed) */
   uint16_t layout_type_direct;
   uint8_t inl_num_wg[3];       /* inline dword ids of num_workgroups x/y/z */
};

static const struct anv_layout anv_layouts[] = {
   {
      .version = "Mesa 26.1.",
      .pd_size = 152, .pd_size_patched = 656, .stats_size = 72, .reloc_size = 16, .emb_size = 40, .input_att_size = 36,
      .push_ranges_size = 32, .dyn_desc_size = 32, .max_inline = 8,
      .stage = 0, .total_scratch = 20, .total_shared = 24, .program_size = 28, .num_relocs = 40, .grf_used = 68,
      .source_hash = 72, .source_hash_size = 4,
      .local_size = 80, .prog_offset = 92, .prog_mask = 104, .prog_spilled = 108, .uses_barrier = 112,
      .generate_local_id = 117, .walk_order = 120, .uses_sampler = 124,
      .uses_inline_push_addr = 114, .uses_systolic = 116, .zero_prog_data = true,
      .layout_type_direct = 2, .inl_num_wg = { 149, 150, 151 }, .null_set = 252,
   },
   {
      /* 26.3.0-rc3: prog_data 8 bytes longer (64-bit source hash), the compute fields 8 bytes further on,
       * uses_inline_push_addr gone, "direct" layout type 1, num_workgroups inline ids 66..68 */
      .version = "Mesa 26.3.",
      .pd_size = 160, .pd_size_patched = 664, .stats_size = 80, .reloc_size = 16, .emb_size = 40, .input_att_size = 36,
      .push_ranges_size = 32, .dyn_desc_size = 32, .max_inline = 8,
      .stage = 0, .total_scratch = 20, .total_shared = 24, .program_size = 28, .num_relocs = 40, .grf_used = 68,
      .source_hash = 72, .source_hash_size = 8,
      .local_size = 88, .prog_offset = 100, .prog_mask = 112, .prog_spilled = 116, .uses_barrier = 120,
      .generate_local_id = 125, .walk_order = 128, .uses_sampler = 132,
      .uses_inline_push_addr = -1, .uses_systolic = -1, .zero_prog_data = false,
      .layout_type_direct = 1, .inl_num_wg = { 66, 67, 68 }, .null_set = -1,
   },
   {
      /* 27.0.0-devel (main a84fa7dd2c2, 2026-11-06): measured the same way, every offset as in 26.3 (the variants'
       * prog_data differs only in program_size and grf_used, values of the compiled code) */
      .version = "Mesa 27.0.",
      .pd_size = 160, .stats_size = 80, .reloc_size = 16, .emb_size = 40, .input_att_size = 36,
      .push_ranges_size = 32, .dyn_desc_size = 32, .max_inline = 8,
      .stage = 0, .total_scratch = 20, .total_shared = 24, .program_size = 28, .num_relocs = 40, .grf_used = 68,
      .source_hash = 72, .source_hash_size = 8,
      .local_size = 88, .prog_offset = 100, .prog_mask = 112, .prog_spilled = 116, .uses_barrier = 120,
      .generate_local_id = 125, .walk_order = 128, .uses_sampler = 132,
      .uses_inline_push_addr = -1, .uses_systolic = -1, .zero_prog_data = false,
      .layout_type_direct = 1, .inl_num_wg = { 66, 67, 68 }, .null_set = -1,
   },
};

static inline const struct anv_layout *
anv_layout_for(const char *driver_info)
{
   for (unsigned i = 0; i < sizeof(anv_layouts) / sizeof(anv_layouts[0]); i++)
      if (!strncmp(driver_info, anv_layouts[i].version, strlen(anv_layouts[i].version)))
         return &anv_layouts[i];
   return NULL;
}

/* binding table entry (the same in 26.1 and 26.3) */
#define SIZEOF_BINDING 16
#define BINDING_OFF_index 0
#define BINDING_OFF_binding 4
#define BINDING_OFF_set_offset 8
#define BINDING_OFF_set 12
#define BINDING_OFF_plane 13
#define SET_NULL 252
#define STAGE_COMPUTE 5
