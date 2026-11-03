// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Maxwell {

[[nodiscard]] IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool,
                                           ObjectPool<IR::Block>& block_pool, Environment& env,
                                           Flow::CFG& cfg, const HostTranslateInfo& host_info);

// Expand guest passthrough before SSA/info collection; idempotent and preserves guest semantics.
void LowerGeometryPassthrough(IR::Program& program);

[[nodiscard]] IR::Program MergeDualVertexPrograms(IR::Program& vertex_a, IR::Program& vertex_b,
                                                  Environment& env_vertex_b);

void ConvertLegacyToGeneric(IR::Program& program, const RuntimeInfo& runtime_info);

// Maxwell v1 and older Nvidia cards don't support setting gl_Layer from non-geometry stages.
// This creates a workaround by setting the layer as a generic output and creating a
// passthrough geometry shader that reads the generic and sets the layer.
[[nodiscard]] IR::Program GenerateGeometryPassthrough(ObjectPool<IR::Inst>& inst_pool,
                                                      ObjectPool<IR::Block>& block_pool,
                                                      const HostTranslateInfo& host_info,
                                                      IR::Program& source_program,
                                                      Shader::OutputTopology output_topology);

/// Turns each point of the vertex program into a quad, for hosts whose points are one pixel wide.
/// size_attr: where the vertex program left the size (otherwise fixed_size). coord_attr: where the
/// fragment program expects the coordinates inside the point. The viewport comes from the stage
/// constants 4 and 5.
[[nodiscard]] IR::Program GeneratePointGeometry(ObjectPool<IR::Inst>& inst_pool,
                                                ObjectPool<IR::Block>& block_pool,
                                                const IR::Program& vertex_program,
                                                std::optional<IR::Attribute> size_attr,
                                                std::optional<IR::Attribute> coord_attr,
                                                f32 fixed_size);

/// Geometry stage drawing each line as a quad, as D3D12 lines are one pixel wide. The width comes
/// from stage constant 0, the viewport from 4 and 5. stipple_attr, when set, takes the pixels from
/// the line's start for the fragment program's stipple.
[[nodiscard]] IR::Program GenerateLineGeometry(ObjectPool<IR::Inst>& inst_pool,
                                               ObjectPool<IR::Block>& block_pool,
                                               const IR::Program& vertex_program,
                                               std::optional<IR::Attribute> stipple_attr);

/// Geometry stage drawing the vertices of each triangle as quads of fixed_size, for the point
/// polygon mode D3D12 lacks. The viewport comes from the stage constants 4 and 5.
[[nodiscard]] IR::Program GeneratePolygonPointGeometry(ObjectPool<IR::Inst>& inst_pool,
                                                       ObjectPool<IR::Block>& block_pool,
                                                       const IR::Program& vertex_program,
                                                       f32 fixed_size);

/// Geometry stage drawing each triangle's bounding box, varyings extended along its plane. Flat
/// varyings of the fragment program come from flat_vertex.
[[nodiscard]] IR::Program GenerateFillRectangleGeometry(ObjectPool<IR::Inst>& inst_pool,
                                                        ObjectPool<IR::Block>& block_pool,
                                                        const IR::Program& vertex_program,
                                                        const IR::Program& fragment_program,
                                                        u32 flat_vertex);

/// Geometry stage emitting each primitive of input_vertices starting with its last vertex, the one
/// D3D12 takes flat varyings from when the guest provokes with the last. Winding is kept.
[[nodiscard]] IR::Program GenerateProvokingLastGeometry(ObjectPool<IR::Inst>& inst_pool,
                                                        ObjectPool<IR::Block>& block_pool,
                                                        const IR::Program& vertex_program,
                                                        u32 input_vertices);

/// Maxwell runs a tessellation evaluation program without a control one, with the levels from its
/// registers; D3D12 needs a hull shader. Passes through what the evaluation program reads, levels
/// from the stage constants.
[[nodiscard]] IR::Program GenerateTessellationControl(ObjectPool<IR::Inst>& inst_pool,
                                                      ObjectPool<IR::Block>& block_pool,
                                                      const IR::Program& vertex_program,
                                                      const IR::Program& eval_program,
                                                      u32 patch_vertices);

} // namespace Shader::Maxwell
