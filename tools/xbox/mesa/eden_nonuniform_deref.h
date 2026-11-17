/* SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
 * SPDX-License-Identifier: MIT
 *
 * Included by nir_to_dxil.c (build-spirv-to-dxil.ps1 copies it next to it).
 *
 * emit_deref creates the handle of an indexed texture/sampler/image array with
 * nonUniformIndex = false, even when SPIR-V decorated the index NonUniform (vtn records that on
 * the users: nir_tex_instr.texture_non_uniform / sampler_non_uniform and ACCESS_NON_UNIFORM). A
 * driver may then read the index once per wave, so one resource is used for every lane. This asks
 * the deref's users instead, as DXC does with NonUniformResourceIndex.
 */

#ifndef EDEN_NONUNIFORM_DEREF_H
#define EDEN_NONUNIFORM_DEREF_H

#include "nir.h"

static bool
eden_deref_non_uniform(nir_deref_instr *deref)
{
   nir_foreach_use(src, &deref->def) {
      nir_instr *user = nir_src_use_instr(src);
      if (user->type == nir_instr_type_tex) {
         nir_tex_instr *tex = nir_instr_as_tex(user);
         const nir_tex_src *tex_src = container_of(src, nir_tex_src, src);
         if (tex_src->src_type == nir_tex_src_texture_deref && tex->texture_non_uniform)
            return true;
         if (tex_src->src_type == nir_tex_src_sampler_deref && tex->sampler_non_uniform)
            return true;
      } else if (user->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(user);
         if (nir_intrinsic_has_access(intr) && (nir_intrinsic_access(intr) & ACCESS_NON_UNIFORM))
            return true;
      }
   }
   return false;
}

#endif
