/* SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
 * SPDX-License-Identifier: MIT
 *
 * Included by nir_to_dxil.c (build-spirv-to-dxil.ps1 copies it next to it).
 *
 * nir_to_dxil emits a dx.op.createHandle wherever NIR computes a resource: texture/sampler/image
 * derefs and Vulkan descriptor loads sit in the block of each use, so a resource used in several
 * branches gets the same handle re-created in every branch, and handles of resources used only
 * after a branch are created there. The Series' driver compiler faults (access violation inside
 * Create*PipelineState) on such shaders, graphics and compute; the same shaders with each handle
 * created once in the entry block build. DXC emits handles that way, so this does the same: every
 * resource handle whose binding and index are constants is computed once, at the start of the entry
 * block. Handles with a dynamic index stay where they are.
 */

#ifndef EDEN_HOIST_RESOURCE_HANDLES_H
#define EDEN_HOIST_RESOURCE_HANDLES_H

#include "nir.h"
#include "nir_instr_set.h"
#include "util/hash_table.h"
#include "util/set.h"

static bool
eden_is_resource_handle(const nir_instr *instr)
{
   if (instr->type == nir_instr_type_deref) {
      const nir_deref_instr *deref = nir_instr_as_deref(instr);
      if (deref->deref_type != nir_deref_type_var && deref->deref_type != nir_deref_type_array)
         return false;
      const nir_variable *var = nir_deref_instr_get_variable(deref);
      if (!var)
         return false;
      const struct glsl_type *type = glsl_without_array(var->type);
      return glsl_type_is_sampler(type) || glsl_type_is_texture(type) || glsl_type_is_image(type);
   }
   if (instr->type == nir_instr_type_intrinsic) {
      switch (nir_instr_as_intrinsic(instr)->intrinsic) {
      case nir_intrinsic_vulkan_resource_index:
      case nir_intrinsic_vulkan_resource_reindex:
      case nir_intrinsic_load_vulkan_descriptor:
         return true;
      default:
         return false;
      }
   }
   return false;
}

struct eden_hoist_state {
   nir_shader *shader;
   nir_cursor cursor;     /* end of the hoisted prologue in the entry block */
   struct set *hoisted;   /* instructions already in the prologue */
   struct set unique;     /* nir_instr_set over the prologue */
};

/* Places instr at the end of the prologue, or drops it for an identical one already there. Returns
 * the instruction that now provides the value. */
static nir_instr *
eden_hoist_place(struct eden_hoist_state *st, nir_instr *instr, bool inserted)
{
   if (inserted)
      nir_instr_remove(instr);
   nir_instr_insert(st->cursor, instr);
   nir_instr *match = nir_instr_set_add_or_rewrite(&st->unique, instr, NULL);
   if (match) {
      nir_instr_remove(instr);
      return match;
   }
   st->cursor = nir_after_instr(instr);
   _mesa_set_add(st->hoisted, instr);
   return instr;
}

static bool
eden_hoist_source(nir_src *src, void *data)
{
   struct eden_hoist_state *st = data;
   nir_instr *parent = nir_def_instr(src->ssa);
   if (_mesa_set_search(st->hoisted, parent))
      return true;
   /* dxil_nir_move_consts put a copy of each constant in its use block: give the prologue one. */
   nir_load_const_instr *load = nir_instr_as_load_const(parent);
   nir_load_const_instr *copy =
      nir_load_const_instr_create(st->shader, load->def.num_components, load->def.bit_size);
   memcpy(copy->value, load->value, sizeof(load->value[0]) * load->def.num_components);
   nir_instr *placed = eden_hoist_place(st, &copy->instr, false);
   nir_src_rewrite(src, nir_instr_def(placed));
   return true;
}

static bool
eden_source_is_constant(nir_src *src, void *data)
{
   const struct eden_hoist_state *st = data;
   const nir_instr *parent = nir_def_instr(src->ssa);
   return parent->type == nir_instr_type_load_const || _mesa_set_search(st->hoisted, parent);
}

static bool
eden_hoist_resource_handles(nir_shader *s)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   if (!impl)
      return false;

   struct eden_hoist_state st = {
      .shader = s,
      .cursor = nir_before_block(nir_start_block(impl)),
      .hoisted = _mesa_pointer_set_create(NULL),
   };
   nir_instr_set_init(&st.unique, NULL);
   bool progress = false;

   /* Block order visits each source before its users (phis aside, which are never candidates). */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (_mesa_set_search(st.hoisted, instr) || !eden_is_resource_handle(instr) ||
             !nir_foreach_src(instr, eden_source_is_constant, &st))
            continue;
         nir_foreach_src(instr, eden_hoist_source, &st);
         eden_hoist_place(&st, instr, true);
         progress = true;
      }
   }

   nir_instr_set_fini(&st.unique);
   _mesa_set_destroy(st.hoisted, NULL);
   return nir_progress(progress, impl, nir_metadata_control_flow);
}

#endif
