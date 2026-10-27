/*
 * SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
 * SPDX-License-Identifier: MIT
 *
 * Linked-pipeline variant of spirv_to_dxil() for Jadyx. build-spirv-to-dxil.ps1
 * copies this file into Mesa's src/microsoft/spirv_to_dxil/ and exports its entry point.
 *
 * It follows spirv_to_dxil() (spirv_to_dxil.c) for each stage, and the link loop of spirv2dxil.c
 * and Dozen (dzn_pipeline.c): translate + prep + passes per stage, then link from the last stage
 * back to the first, then emit DXIL.
 */

#include "dxil_spirv_nir.h"
#include "eden_spirv_to_dxil.h"
#include "eden_integer_sampling.h"
#include "eden_reduction_sampling.h"
#include "nir_builder.h"
#include "nir_to_dxil.h"
#include "nir_xfb_info.h"
#include "spirv/nir_spirv.h"
#include "spirv/spirv_info.h"
#include "util/blob.h"
#include "util/os_time.h"
#include "util/u_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
free_objects(struct dxil_spirv_object *out, unsigned count)
{
   for (unsigned i = 0; i < count; ++i) {
      free(out[i].binary.buffer);
      memset(&out[i], 0, sizeof(out[i]));
   }
}

/* Guest (Maxwell) shaders rely on IEEE results: rsq(0) = inf, min/max picking the non-NaN operand,
 * x * 0 staying NaN for x = inf... nir_to_dxil tags every non-exact float op with fast math
 * (DXIL_UNSAFE_ALGEBRA, "no NaN/Inf"), and the Xbox Series' shader compiler takes it: lit pixels
 * of Game C came out NaN there (the black silhouettes) while the PC's driver kept IEEE. */
static bool
mark_alu_exact(nir_builder *b, nir_instr *instr, void *data)
{
   (void)b;
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_instr_as_alu(instr)->fp_math_ctrl |= nir_fp_exact;
   return true;
}

/* The Xbox Series' shader compiler faults (access violation inside CreateGraphicsPipelineState)
 * on some vertex shaders whose position comes out of a large if/else (an early "invisible" exit
 * next to the full transform). The same shaders build once every branch is turned into selects,
 * so the renderer retranslates a faulting pipeline with EDEN_SPIRV_TO_DXIL_FLATTEN_VERTEX.
 *
 * Storage buffers are bound through descriptor tables, where an out-of-range read returns zero:
 * loading one on a path that would not have run is harmless, so the loads may leave their branch.
 * Without that, nir_opt_peephole_select keeps every branch holding one. */
static bool
speculate_ssbo_loads(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)b;
   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_ssbo)
      return false;
   const enum gl_access_qualifier access =
      nir_intrinsic_access(intr) | ACCESS_CAN_REORDER | ACCESS_CAN_SPECULATE;
   if (access == nir_intrinsic_access(intr))
      return false;
   nir_intrinsic_set_access(intr, access);
   return true;
}

static void
flatten_branches(nir_shader *s)
{
   NIR_PASS(_, s, nir_shader_intrinsics_pass, speculate_ssbo_loads, nir_metadata_all, NULL);
   /* limit ~0 is the "no control flow hardware" mode: every if without a jump becomes selects. */
   const nir_opt_peephole_select_options options = {
      .limit = ~0u,
      .indirect_load_ok = true,
      .expensive_alu_ok = true,
   };
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_peephole_select, &options);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
   } while (progress);
}

static nir_variable *
find_stream_output_var(nir_shader *s, unsigned location, unsigned component, unsigned count)
{
   nir_foreach_variable_with_modes(var, s, nir_var_shader_out) {
      if (var->data.location != location)
         continue;
      const unsigned components =
         var->data.compact ? glsl_get_length(var->type) : glsl_get_components(var->type);
      if (var->data.location_frac <= component &&
          var->data.location_frac + components >= component + count)
         return var;
   }
   return NULL;
}

static void
add_stream_output_entry(struct eden_spirv_to_dxil_stream_output *so, unsigned stream,
                        unsigned semantic, unsigned semantic_index, unsigned start_component,
                        unsigned component_count, unsigned slot)
{
   if (so->entry_count >= EDEN_SPIRV_TO_DXIL_SO_MAX_ENTRIES) {
      so->dropped_entries++;
      return;
   }
   so->entries[so->entry_count++] = (struct eden_spirv_to_dxil_so_entry){
      .stream = stream,
      .semantic = semantic,
      .semantic_index = semantic_index,
      .start_component = start_component,
      .component_count = component_count,
      .output_slot = slot,
   };
}

static int
compare_xfb_outputs(const void *a_, const void *b_)
{
   const nir_xfb_output_info *a = a_, *b = b_;
   if (a->buffer != b->buffer)
      return a->buffer - b->buffer;
   return a->offset - b->offset;
}

/* D3D12 stream output declaration of the last pre-rasterization stage. Read after nir_to_dxil,
 * so the semantics are the ones its output signature got (get_semantic_name in dxil_signature.c).
 * Follows fill_so_declaration in the d3d12 gallium driver: skipped words become gap entries. */
static void
fill_stream_output(nir_shader *s, struct eden_spirv_to_dxil_stream_output *so)
{
   memset(so, 0, sizeof(*so));
   const nir_xfb_info *xfb = s->xfb_info;
   if (!xfb || xfb->output_count == 0)
      return;
   nir_xfb_output_info *outputs = malloc(xfb->output_count * sizeof(outputs[0]));
   if (!outputs) {
      so->dropped_entries = xfb->output_count;
      return;
   }
   memcpy(outputs, xfb->outputs, xfb->output_count * sizeof(outputs[0]));
   /* nir_gather_xfb_info sorts by offset only; D3D12 writes each slot in declaration order. */
   qsort(outputs, xfb->output_count, sizeof(outputs[0]), compare_xfb_outputs);

   unsigned next_offset[NIR_MAX_XFB_BUFFERS] = {0};
   for (unsigned i = 0; i < xfb->output_count; ++i) {
      const nir_xfb_output_info *output = &outputs[i];
      const unsigned buffer = output->buffer;
      const unsigned stream = xfb->buffer_to_stream[buffer];
      const unsigned components = util_bitcount(output->component_mask);
      if (output->offset < next_offset[buffer]) {
         /* Overlaps an earlier output: a declaration cannot write a word twice. */
         so->missing_outputs++;
         continue;
      }
      unsigned gap = (output->offset - next_offset[buffer]) / 4;
      while (gap > 0) {
         const unsigned words = MIN2(gap, 4);
         add_stream_output_entry(so, stream, EDEN_SO_SEMANTIC_GAP, 0, 0, words, buffer);
         gap -= words;
      }
      next_offset[buffer] = output->offset + components * 4;

      nir_variable *var =
         find_stream_output_var(s, output->location, output->component_offset, components);
      unsigned semantic = EDEN_SO_SEMANTIC_GAP;
      unsigned index = 0;
      unsigned location = var ? var->data.location : 0;
      if (var && (location == VARYING_SLOT_CLIP_DIST0 || location == VARYING_SLOT_CLIP_DIST1)) {
         const unsigned component =
            (location - VARYING_SLOT_CLIP_DIST0) * 4 + var->data.location_frac;
         if (component >= s->info.clip_distance_array_size)
            location = VARYING_SLOT_CULL_DIST0 +
                       (component - s->info.clip_distance_array_size) / 4;
      }
      if (var) {
         switch (location) {
         case VARYING_SLOT_POS:
            semantic = EDEN_SO_SEMANTIC_POSITION;
            break;
         case VARYING_SLOT_CLIP_DIST0:
         case VARYING_SLOT_CLIP_DIST1:
            semantic = EDEN_SO_SEMANTIC_CLIP_DISTANCE;
            index = location - VARYING_SLOT_CLIP_DIST0;
            break;
         case VARYING_SLOT_CULL_DIST0:
         case VARYING_SLOT_CULL_DIST1:
            semantic = EDEN_SO_SEMANTIC_CULL_DISTANCE;
            index = location - VARYING_SLOT_CULL_DIST0;
            break;
         case VARYING_SLOT_PRIMITIVE_ID:
            semantic = EDEN_SO_SEMANTIC_PRIMITIVE_ID;
            break;
         case VARYING_SLOT_VIEWPORT:
            semantic = EDEN_SO_SEMANTIC_VIEWPORT_ARRAY_INDEX;
            break;
         case VARYING_SLOT_LAYER:
            semantic = EDEN_SO_SEMANTIC_RENDER_TARGET_ARRAY_INDEX;
            break;
         case VARYING_SLOT_PSIZ:
            /* DXIL has no point size output. */
            break;
         default:
            semantic = EDEN_SO_SEMANTIC_TEXCOORD;
            index = var->data.driver_location;
            break;
         }
      }
      if (semantic == EDEN_SO_SEMANTIC_GAP) {
         /* Not in the signature: its words are left as they were. */
         so->missing_outputs++;
         add_stream_output_entry(so, stream, semantic, 0, 0, components, buffer);
      } else {
         add_stream_output_entry(so, stream, semantic, index,
                                 output->component_offset - var->data.location_frac, components,
                                 buffer);
      }
   }
   free(outputs);
}

/* The NIR-to-DXIL parts are added by the nir_to_dxil.c patch of build-spirv-to-dxil.ps1. */
uint64_t eden_phase_ns[EDEN_PHASE_COUNT];

static void
add_phase(enum eden_spirv_to_dxil_phase phase, int64_t start)
{
   p_atomic_add(&eden_phase_ns[phase], (uint64_t)(os_time_get_nano() - start));
}

void
eden_spirv_to_dxil_phase_times(uint64_t *ns, unsigned count)
{
   for (unsigned i = 0; i < count && i < EDEN_PHASE_COUNT; ++i)
      ns[i] = p_atomic_read(&eden_phase_ns[i]);
}

static bool
translate_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                   enum dxil_validator_version validator_version_max,
                   const struct dxil_spirv_debug_options *debug_options,
                   const struct dxil_spirv_logger *logger, struct dxil_spirv_object *out,
                   bool lower_integer_sampling, uint32_t flags,
                   struct eden_spirv_to_dxil_stream_output *stream_output)
{
   if (count == 0 || count > EDEN_SPIRV_TO_DXIL_MAX_STAGES)
      return false;
   for (unsigned i = 0; i < count; ++i) {
      const dxil_spirv_shader_stage stage = stages[i].stage;
      if (stage == DXIL_SPIRV_SHADER_NONE || stage == DXIL_SPIRV_SHADER_KERNEL)
         return false;
      if (stage == DXIL_SPIRV_SHADER_COMPUTE && count != 1)
         return false;
      if (i > 0 && stage <= stages[i - 1].stage)
         return false;
   }
   memset(out, 0, sizeof(*out) * count);
   p_atomic_inc(&eden_phase_ns[EDEN_PHASE_PIPELINES]);
   p_atomic_add(&eden_phase_ns[EDEN_PHASE_STAGES], count);

   glsl_type_singleton_init_or_ref();

   /* Each shader keeps a pointer to its compiler options until it is freed. */
   nir_shader_compiler_options nir_options[EDEN_SPIRV_TO_DXIL_MAX_STAGES];
   nir_shader *nir[EDEN_SPIRV_TO_DXIL_MAX_STAGES] = {0};
   /* The renderer lets vertex and tessellation stages write Layer and ViewportIndex when D3D12
    * reports VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, but the
    * capabilities of Mesa's DXIL translator leave ShaderViewportIndexLayerEXT off, so the parser
    * rejected those shaders ("invalid stage for SpvBuiltInLayer") and their draws were skipped.
    * nir_to_dxil already maps both outputs and flags the module for them. */
   struct spirv_capabilities spirv_caps = *dxil_spirv_nir_get_spirv_options()->capabilities;
   spirv_caps.ShaderViewportIndexLayerEXT = true;
   struct spirv_to_nir_options spirv_options = *dxil_spirv_nir_get_spirv_options();
   spirv_options.capabilities = &spirv_caps;
   const struct spirv_to_nir_options *spirv_opts = &spirv_options;
   const unsigned supported_bit_sizes = 16 | 32 | 64;
   bool success = true;

   for (unsigned i = 0; i < count && success; ++i) {
      const struct dxil_spirv_runtime_conf *conf = stages[i].conf;
      dxil_get_nir_compiler_options(&nir_options[i], conf->shader_model_max,
                                    supported_bit_sizes, supported_bit_sizes);
      nir_options[i].lower_base_vertex =
         conf->first_vertex_and_base_instance_mode != DXIL_SPIRV_SYSVAL_TYPE_ZERO;

      int64_t start = os_time_get_nano();
      nir[i] = spirv_to_nir(stages[i].words, stages[i].word_count, NULL,
                            (mesa_shader_stage)stages[i].stage, stages[i].entry_point,
                            spirv_opts, &nir_options[i]);
      add_phase(EDEN_PHASE_SPIRV_TO_NIR, start);
      if (!nir[i]) {
         success = false;
         break;
      }
      start = os_time_get_nano();
      nir_validate_shader(nir[i], "Validate before feeding NIR to the DXIL compiler");
      dxil_spirv_nir_prep(nir[i]);
      dxil_spirv_nir_passes(nir[i], conf, &out[i].metadata);
      /* After the passes: tex instructions are final (cube and implicit-LOD lowering done) and
       * nir_to_dxil's own optimization loop scalarizes and lowers what this emits. */
      if (lower_integer_sampling)
         eden_lower_integer_sampling(nir[i]);
      if (flags & EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION)
         eden_lower_reduction_sampling(nir[i]);
      add_phase(EDEN_PHASE_NIR_PASSES, start);
   }

   if (success) {
      const int64_t start = os_time_get_nano();
      /* Reverse order, so outputs the next stage never reads are gone before the previous stage
       * is linked against its own predecessor. */
      for (unsigned i = count; i-- > 0;) {
         /* Linking the fragment stage can remove GS outputs and their input
          * loads. Drop the now-unused GS inputs before assigning the VS/GS
          * signature rows; otherwise an unused generic shifts SV_Position in
          * the GS signature without a matching VS output. */
         if (nir[i]->info.stage == MESA_SHADER_GEOMETRY) {
            NIR_PASS(_, nir[i], nir_opt_dce);
            NIR_PASS(_, nir[i], nir_remove_dead_derefs);
            NIR_PASS(_, nir[i], nir_remove_dead_variables, nir_var_shader_in, NULL);
            nir_shader_gather_info(nir[i], nir_shader_get_entrypoint(nir[i]));
         }
         struct dxil_spirv_metadata link_metadata = {0};
         dxil_spirv_nir_link(nir[i], i > 0 ? nir[i - 1] : NULL, stages[i].conf, &link_metadata);
         out[i].metadata.requires_runtime_data |= link_metadata.requires_runtime_data;
         out[i].metadata.needs_draw_sysvals |= link_metadata.needs_draw_sysvals;
      }
      add_phase(EDEN_PHASE_LINK, start);
   }

   struct dxil_logger logger_inner = {.priv = logger->priv, .log = logger->log};
   for (unsigned i = 0; i < count && success; ++i) {
      if (debug_options->dump_nir)
         nir_print_shader(nir[i], stderr);

      if (nir[i]->info.stage == MESA_SHADER_VERTEX && (flags & EDEN_SPIRV_TO_DXIL_FLATTEN_VERTEX))
         flatten_branches(nir[i]);

      /* lower_int16: widen every 16-bit ALU op to 32 bits. The Xbox Series has no native 16-bit
       * shader ops, and NIR still makes 16-bit ops out of mediump (RelaxedPrecision) code even
       * with the recompiler's fp16/int16 off; the console driver then rejects the pixel shader
       * (E_INVALIDARG). Dozen lowers only when the app enables 16-bit types, which misses those. */
      nir_shader_instructions_pass(nir[i], mark_alu_exact, nir_metadata_all, NULL);
      const struct nir_to_dxil_options opts = {
         .environment = DXIL_ENVIRONMENT_VULKAN,
         .lower_int16 = true,
         .disable_math_refactoring = true,
         .shader_model_max = stages[i].conf->shader_model_max,
         .validator_version_max = validator_version_max,
      };
      struct blob dxil_blob;
      const int64_t start = os_time_get_nano();
      const bool translated = nir_to_dxil(nir[i], &opts, &logger_inner, &dxil_blob);
      add_phase(EDEN_PHASE_NIR_TO_DXIL, start);
      if (!translated) {
         if (dxil_blob.allocated)
            blob_finish(&dxil_blob);
         success = false;
         break;
      }
      blob_finish_get_buffer(&dxil_blob, &out[i].binary.buffer, &out[i].binary.size);
   }

   if (success && stream_output) {
      memset(stream_output, 0, sizeof(*stream_output));
      /* The last stage before the fragment one feeds stream output. */
      nir_shader *last = NULL;
      for (unsigned i = 0; i < count; ++i) {
         if (nir[i]->info.stage != MESA_SHADER_FRAGMENT)
            last = nir[i];
      }
      if (last && last->info.stage != MESA_SHADER_COMPUTE &&
          last->info.stage != MESA_SHADER_TESS_CTRL)
         fill_stream_output(last, stream_output);
   }

   for (unsigned i = 0; i < count; ++i)
      ralloc_free(nir[i]);
   if (!success)
      free_objects(out, count);

   glsl_type_singleton_decref();
   return success;
}

uint32_t
eden_spirv_to_dxil_supported_flags(void)
{
   return EDEN_SPIRV_TO_DXIL_FLATTEN_VERTEX | EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION;
}

bool
eden_spirv_to_dxil_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                            enum dxil_validator_version validator_version_max,
                            const struct dxil_spirv_debug_options *debug_options,
                            const struct dxil_spirv_logger *logger,
                            struct dxil_spirv_object *out)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             false, 0, NULL);
}

/* Integer textures sampled through the b0 space 29 table (eden_integer_sampler_state). */
bool
eden_spirv_to_dxil_pipeline_v2(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger,
                               struct dxil_spirv_object *out)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             true, 0, NULL);
}

bool
eden_spirv_to_dxil_pipeline_v3(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger, uint32_t flags,
                               struct dxil_spirv_object *out)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             true, flags, NULL);
}

bool
eden_spirv_to_dxil_pipeline_v4(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger, uint32_t flags,
                               struct dxil_spirv_object *out,
                               struct eden_spirv_to_dxil_stream_output *stream_output)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             true, flags, stream_output);
}
