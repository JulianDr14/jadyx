/*
 * SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
 * SPDX-License-Identifier: MIT
 *
 * Linked-pipeline entry point Jadyx adds to Mesa's spirv_to_dxil.dll
 * (tools/xbox/mesa/eden_pipeline.c, built by tools/xbox/build-spirv-to-dxil.ps1).
 *
 * spirv_to_dxil() translates one stage in isolation, so each stage packs its varyings on its own
 * and the signatures of two stages only match when both use the exact same locations. This entry
 * point runs Mesa's inter-stage link (dxil_spirv_nir_link, as spirv2dxil and Dozen do) before
 * emitting DXIL, which kills unused outputs, packs both sides of every interface the same way and
 * propagates interpolation modes.
 */

#ifndef EDEN_SPIRV_TO_DXIL_H
#define EDEN_SPIRV_TO_DXIL_H

#include "spirv_to_dxil.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDEN_SPIRV_TO_DXIL_MAX_STAGES 5

/* v2 integer texture sampling: b0 space 29, indexed by the original texture binding.
 * Keep fields in 16-byte rows for DXIL constant-buffer loads. No C bitfields in this ABI. */
#define EDEN_INTEGER_SAMPLER_SPACE 29
#define EDEN_INTEGER_SAMPLER_MAX_BINDINGS 1024
struct eden_integer_sampler_state {
   uint32_t border_color[4];
   float lod_bias, min_lod, max_lod;
   uint32_t last_level;
   uint32_t wrap[3]; /* D3D12_TEXTURE_ADDRESS_MODE values */
   uint32_t padding;
   /* Read by EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION only. */
   uint32_t reduction; /* EDEN_SAMPLER_REDUCTION_* */
   uint32_t filter;    /* EDEN_SAMPLER_FILTER_* bits */
   uint32_t reserved[2];
};

#define EDEN_SAMPLER_REDUCTION_NONE 0u
#define EDEN_SAMPLER_REDUCTION_MIN 1u
#define EDEN_SAMPLER_REDUCTION_MAX 2u

#define EDEN_SAMPLER_FILTER_MIN_LINEAR (1u << 0)
#define EDEN_SAMPLER_FILTER_MAG_LINEAR (1u << 1)
#define EDEN_SAMPLER_FILTER_MIP_LINEAR (1u << 2)

struct eden_spirv_to_dxil_stage {
   const uint32_t *words;
   size_t word_count;
   dxil_spirv_shader_stage stage;
   const char *entry_point;
   /* Per stage: yz_flip may only be set on the last pre-rasterization stage. */
   const struct dxil_spirv_runtime_conf *conf;
};

/* Translates the stages of one pipeline, given in pipeline order (vertex first), and links each
 * stage with the previous one. On success, out[i] holds stage i and must be released with
 * spirv_to_dxil_free(); on failure, nothing needs releasing. A single compute stage is also
 * accepted and behaves like spirv_to_dxil(). */
bool
eden_spirv_to_dxil_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                            enum dxil_validator_version validator_version_max,
                            const struct dxil_spirv_debug_options *debug_options,
                            const struct dxil_spirv_logger *logger,
                            struct dxil_spirv_object *out);

/* Separate symbol: an older DLL must never silently compile shaders that expect the v2 ABI. */
bool
eden_spirv_to_dxil_pipeline_v2(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                             enum dxil_validator_version validator_version_max,
                             const struct dxil_spirv_debug_options *debug_options,
                             const struct dxil_spirv_logger *logger,
                             struct dxil_spirv_object *out);

/* v3: v2 plus translation flags. 0 is the same as v2. */
/* Every branch of the vertex stage becomes selects, storage buffer loads included. Works around
 * the Xbox Series' shader compiler faulting on some vertex shaders; results are unchanged. */
#define EDEN_SPIRV_TO_DXIL_FLATTEN_VERTEX (1u << 0)
/* Float texture samples (not cube, not shadow) read their binding's row of the b0 space 29 table
 * and, when its reduction is MIN or MAX, return the per-component minimum or maximum of the texels
 * the filter would weigh, loaded one by one: filtered MIN/MAX samplers need tiled resources
 * tier 2, which the Xbox Series does not report. Other reductions sample as before. */
#define EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION (1u << 1)

/* The flags this DLL understands. An older DLL lacks the symbol: it supports FLATTEN_VERTEX only. */
uint32_t
eden_spirv_to_dxil_supported_flags(void);

bool
eden_spirv_to_dxil_pipeline_v3(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger, uint32_t flags,
                               struct dxil_spirv_object *out);

/* v4: stream output (transform feedback) of the last pre-rasterization stage, as a
 * D3D12_SO_DECLARATION_ENTRY list whose semantics name that stage's DXIL output signature. Built
 * from the SPIR-V Xfb decorations (XfbBuffer, Offset, Stream); words no output covers are gaps. */
#define EDEN_SPIRV_TO_DXIL_SO_MAX_ENTRIES 512

enum eden_so_semantic {
   EDEN_SO_SEMANTIC_GAP = 0, /* SemanticName NULL: ComponentCount words skipped */
   EDEN_SO_SEMANTIC_TEXCOORD,
   EDEN_SO_SEMANTIC_POSITION,
   EDEN_SO_SEMANTIC_CLIP_DISTANCE,
   EDEN_SO_SEMANTIC_CULL_DISTANCE,
   EDEN_SO_SEMANTIC_PRIMITIVE_ID,
   EDEN_SO_SEMANTIC_VIEWPORT_ARRAY_INDEX,
   EDEN_SO_SEMANTIC_RENDER_TARGET_ARRAY_INDEX,
};

struct eden_spirv_to_dxil_so_entry {
   uint8_t stream;
   uint8_t semantic; /* enum eden_so_semantic */
   uint8_t semantic_index;
   uint8_t start_component;
   uint8_t component_count;
   uint8_t output_slot;
   uint8_t padding[2];
};

struct eden_spirv_to_dxil_stream_output {
   uint32_t entry_count;
   /* Captured outputs missing from the signature (written as gaps), and entries past the limit. */
   uint32_t missing_outputs;
   uint32_t dropped_entries;
   uint32_t padding;
   struct eden_spirv_to_dxil_so_entry entries[EDEN_SPIRV_TO_DXIL_SO_MAX_ENTRIES];
};

/* Time spent in each phase of the pipeline entry points since the DLL was loaded, in
 * nanoseconds, indexed by enum eden_spirv_to_dxil_phase. Writes at most count values. */
enum eden_spirv_to_dxil_phase {
   EDEN_PHASE_SPIRV_TO_NIR = 0,
   EDEN_PHASE_NIR_PASSES,  /* dxil_spirv_nir_prep, dxil_spirv_nir_passes and our lowering */
   EDEN_PHASE_LINK,
   EDEN_PHASE_NIR_TO_DXIL, /* whole nir_to_dxil call */
   EDEN_PHASE_NTD_OPTIMIZE, /* its two optimize_nir loops */
   EDEN_PHASE_NTD_EMIT,     /* its emit_module */
   EDEN_PHASE_PIPELINES,    /* count of pipelines, not time */
   EDEN_PHASE_STAGES,       /* count of stages, not time */
   EDEN_PHASE_COUNT,
};

void
eden_spirv_to_dxil_phase_times(uint64_t *ns, unsigned count);

/* stream_output may be NULL; otherwise it is filled on success (entry_count 0: no Xfb outputs). */
bool
eden_spirv_to_dxil_pipeline_v4(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger, uint32_t flags,
                               struct dxil_spirv_object *out,
                               struct eden_spirv_to_dxil_stream_output *stream_output);

#ifdef __cplusplus
}
#endif

#endif
