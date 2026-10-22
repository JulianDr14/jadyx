/*
 * SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
 * SPDX-License-Identifier: MIT
 *
 * Filtered MIN/MAX sampler reduction in the shader (EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION).
 *
 * D3D12 only creates samplers with MINIMUM/MAXIMUM reduction on tiled resources tier 2 devices;
 * the Xbox Series reports tier 1. The renderer then binds the same sampler with STANDARD reduction
 * and writes the guest reduction and filter to the binding's row 3 of the b0 space 29 table. Each
 * float sample reads that row: when the reduction is MIN or MAX it replaces the filtered value by
 * the per-component minimum or maximum of the texels the filter would weigh, as D3D defines the
 * reduction (texels with non-zero weight, the border color counting as a texel):
 *  - min or mag filter per sample (lod <= 0 magnifies), nearest or linear between two mip levels
 *  - linear axes: texels floor(u * size - 0.5) and the next one, the next one only when the
 *    fraction is non-zero at the 8-bit subtexel precision filtering hardware weighs with
 *  - each texel wrapped by the sampler's address mode, then read with a texel fetch
 * The hardware sample stays unconditional, outside the branch, so the implicit LOD of every other
 * sample keeps its derivatives; the branch is uniform for a draw and holds only fetches and ALU.
 */
#ifndef EDEN_REDUCTION_SAMPLING_H
#define EDEN_REDUCTION_SAMPLING_H

#include "eden_integer_sampling.h"
#include "util/u_dynarray.h"

static bool
reduction_sample_filter(const nir_instr *instr)
{
   if (instr->type != nir_instr_type_tex) return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
       tex->op != nir_texop_txl && tex->op != nir_texop_txd)
      return false;
   if (nir_alu_type_get_base_type(tex->dest_type) != nir_type_float || tex->def.bit_size != 32 ||
       tex->is_shadow)
      return false;
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_1D && tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
       tex->sampler_dim != GLSL_SAMPLER_DIM_3D)
      return false;
   return nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) >= 0;
}

typedef struct {
   nir_tex_instr *tex;
   unsigned ncoord;
   nir_def *coord;
   nir_def *offset; /* NULL without a texel offset */
   nir_def *size0;
   nir_def *wrap;
   nir_def *border;
   nir_def *is_max;
   nir_def *linear; /* min or mag filter of this sample */
} reduction_params;

/* Folds the texels the filter weighs at one mip level into acc. */
static nir_def *
reduce_level(nir_builder *b, const reduction_params *p, nir_def *level, nir_def *acc)
{
   nir_def *size = nir_i2f32(b, nir_imax(b, nir_ishr(b, p->size0, level), nir_imm_int(b, 1)));
   nir_def *base[3];
   nir_def *has_next[3];
   for (unsigned i = 0; i < p->ncoord; ++i) {
      nir_def *u = nir_fmul(b, nir_channel(b, p->coord, i), nir_channel(b, size, i));
      nir_def *fixed = nir_fround_even(b, nir_fmul_imm(b, nir_fadd_imm(b, u, -0.5f), 256.0f));
      nir_def *linear_base = nir_ffloor(b, nir_fmul_imm(b, fixed, 1.0f / 256.0f));
      nir_def *fraction = nir_fneu(b, fixed, nir_fmul_imm(b, linear_base, 256.0f));
      base[i] = nir_bcsel(b, p->linear, linear_base, nir_ffloor(b, u));
      has_next[i] = nir_iand(b, p->linear, fraction);
      if (p->offset)
         base[i] = nir_fadd(b, base[i], nir_i2f32(b, nir_channel(b, p->offset, i)));
   }
   /* Each axis wraps its two texel coordinates once; the corners combine them. */
   wrap_result_t wrapped[3][2];
   for (unsigned i = 0; i < p->ncoord; ++i) {
      for (unsigned next = 0; next < 2; ++next) {
         nir_def *c = next ? nir_fadd_imm(b, base[i], 1.0f) : base[i];
         wrapped[i][next] = wrap_dynamic(b, c, nir_channel(b, p->wrap, i), nir_channel(b, size, i));
      }
   }
   wrap_result_t layer = {0};
   if (p->tex->is_array) {
      nir_def *index = nir_ffloor(b, nir_fadd_imm(b, nir_channel(b, p->coord, p->ncoord), 0.5f));
      layer = wrap_coords(b, index, DXIL_TEX_WRAP_CLAMP_TO_EDGE,
                          nir_i2f32(b, nir_channel(b, p->size0, p->ncoord)));
   }
   for (unsigned corner = 0; corner < (1u << p->ncoord); ++corner) {
      wrap_lower_param_t params = {.lod = level, .ncoord_comp = p->ncoord};
      nir_def *valid = nir_imm_true(b);
      nir_def *use_border = nir_imm_false(b);
      for (unsigned i = 0; i < p->ncoord; ++i) {
         const unsigned next = (corner >> i) & 1;
         if (next)
            valid = nir_iand(b, valid, has_next[i]);
         params.wrap[i] = wrapped[i][next];
         use_border = nir_ior(b, use_border, wrapped[i][next].use_border_color);
      }
      if (p->tex->is_array)
         params.wrap[p->ncoord] = layer;
      /* Out-of-range fetches read zero; a bordered texel takes the border color instead. */
      nir_def *texel = nir_bcsel(b, use_border, p->border, load_texel(b, p->tex, &params));
      nir_def *reduced = nir_bcsel(b, p->is_max, nir_fmax(b, acc, texel), nir_fmin(b, acc, texel));
      acc = nir_bcsel(b, valid, reduced, acc);
   }
   return acc;
}

static void
reduction_sample(nir_builder *b, nir_tex_instr *tex)
{
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *binding = texture_binding(b, tex);
   nir_def *reduction = load_state(b, binding, 3);
   nir_def *mode = nir_channel(b, reduction, 0);
   nir_def *filter = nir_channel(b, reduction, 1);
   nir_def *state = load_state(b, binding, 1);
   nir_def *size0 = nir_get_texture_size(b, tex);
   /* Unclamped: sample_lod applies the sampler's bias and clamps itself. Outside the branch, as
    * CalculateLOD needs the derivatives of every lane of the quad. */
   nir_def *lod = sample_lod(b, tex, state, size0, 1);

   nir_if *emulate = nir_push_if(b, nir_ine_imm(b, mode, EDEN_SAMPLER_REDUCTION_NONE));
   reduction_params p = {
      .tex = tex,
      .ncoord = tex->coord_components - tex->is_array,
      .coord = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa,
      .size0 = size0,
      .wrap = load_state(b, binding, 2),
      .border = load_state(b, binding, 0),
      .is_max = nir_ieq_imm(b, mode, EDEN_SAMPLER_REDUCTION_MAX),
   };
   int offset_index = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   if (offset_index >= 0)
      p.offset = tex->src[offset_index].src.ssa;
   nir_def *magnify = nir_fle_imm(b, lod, 0.0f);
   nir_def *linear_bits = nir_bcsel(b, magnify,
      nir_iand_imm(b, filter, EDEN_SAMPLER_FILTER_MAG_LINEAR),
      nir_iand_imm(b, filter, EDEN_SAMPLER_FILTER_MIN_LINEAR));
   p.linear = nir_ine_imm(b, linear_bits, 0);
   nir_def *mip_linear = nir_ine_imm(b, nir_iand_imm(b, filter, EDEN_SAMPLER_FILTER_MIP_LINEAR), 0);
   nir_def *last_level = nir_channel(b, state, 3);

   /* Nearest mip: ceil(lod + 0.5) - 1. Linear mip: floor(lod), and the next level when the
    * fraction is non-zero at 8 bits. */
   nir_def *nearest = nir_f2i32(b, nir_fadd_imm(b, nir_fceil(b, nir_fadd_imm(b, lod, 0.5f)), -1));
   nir_def *fixed = nir_fround_even(b, nir_fmul_imm(b, lod, 256.0f));
   nir_def *floor_level = nir_ffloor(b, nir_fmul_imm(b, fixed, 1.0f / 256.0f));
   nir_def *fraction = nir_fneu(b, fixed, nir_fmul_imm(b, floor_level, 256.0f));
   nir_def *level0 = nir_bcsel(b, mip_linear, nir_f2i32(b, floor_level), nearest);
   level0 = nir_iclamp(b, level0, nir_imm_int(b, 0), last_level);
   nir_def *level1 = nir_imin(b, nir_iadd_imm(b, level0, 1), last_level);
   nir_def *second = nir_iand(b, nir_iand(b, mip_linear, fraction), nir_ine(b, level1, level0));

   nir_def *acc = nir_bcsel(b, p.is_max, nir_imm_vec4(b, -INFINITY, -INFINITY, -INFINITY, -INFINITY),
                            nir_imm_vec4(b, INFINITY, INFINITY, INFINITY, INFINITY));
   acc = reduce_level(b, &p, level0, acc);
   nir_if *second_if = nir_push_if(b, second);
   nir_def *acc_both = reduce_level(b, &p, level1, acc);
   nir_pop_if(b, second_if);
   acc = nir_if_phi(b, acc_both, acc);
   nir_def *emulated = nir_trim_vector(b, acc, tex->def.num_components);
   nir_push_else(b, emulate);
   nir_def *unused = nir_undef(b, tex->def.num_components, 32);
   nir_pop_if(b, emulate);
   nir_def *value = nir_if_phi(b, emulated, unused);
   nir_def *active = nir_ine_imm(b, mode, EDEN_SAMPLER_REDUCTION_NONE);

   b->cursor = nir_after_instr(&tex->instr);
   nir_def *result = nir_bcsel(b, active, value, &tex->def);
   nir_def_rewrite_uses_after(&tex->def, result);
}

static bool
eden_lower_reduction_sampling(nir_shader *nir)
{
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      /* Collected first: the lowering adds blocks and must not visit its own fetches. */
      struct util_dynarray samples;
      util_dynarray_init(&samples, NULL);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (reduction_sample_filter(instr))
               util_dynarray_append_typed(&samples, nir_tex_instr *, nir_instr_as_tex(instr));
         }
      }
      if (util_dynarray_num_elements(&samples, nir_tex_instr *) == 0) {
         util_dynarray_fini(&samples);
         nir_no_progress(impl);
         continue;
      }
      declare_sampler_table(nir);
      nir_builder b = nir_builder_create(impl);
      util_dynarray_foreach(&samples, nir_tex_instr *, tex)
         reduction_sample(&b, *tex);
      util_dynarray_fini(&samples);
      progress |= nir_progress(true, impl, nir_metadata_none);
   }
   return progress;
}

#endif
