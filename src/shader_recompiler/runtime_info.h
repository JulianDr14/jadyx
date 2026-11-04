// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/varying_state.h"

namespace Shader {

enum class AttributeType : u8 {
    Float,
    SignedInt,
    UnsignedInt,
    SignedScaled,
    UnsignedScaled,
    Disabled,
    /// Packed SNORM 10:10:10:2 fetched as four uints and normalized in the shader, for hosts
    /// without the vertex format (D3D12).
    SignedNormA2B10G10R10,
};

/// How the stage runs when tessellation is emulated (tessellation_emulation.h).
enum class TessEmulation : u8 {
    None,
    /// The control shader runs as compute and writes patch records.
    ControlCompute,
    /// The evaluation shader runs as a vertex shader over the tessellator's domain points.
    EvalVertex,
};

enum class InputTopology {
    Points,
    Lines,
    LinesAdjacency,
    Triangles,
    TrianglesAdjacency,
};

struct InputTopologyVertices {
    static u32 vertices(InputTopology input_topology) {
        switch (input_topology) {
        case InputTopology::Lines:
            return 2;
        case InputTopology::LinesAdjacency:
            return 4;
        case InputTopology::Triangles:
            return 3;
        case InputTopology::TrianglesAdjacency:
            return 6;
        case InputTopology::Points:
        default:
            return 1;
        }
    }
};

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessThanEqual,
    Greater,
    NotEqual,
    GreaterThanEqual,
    Always,
};

enum class TessPrimitive {
    Isolines,
    Triangles,
    Quads,
};

enum class TessSpacing {
    Equal,
    FractionalOdd,
    FractionalEven,
};

struct TransformFeedbackVarying {
    u32 buffer{};
    u32 stream{};
    u32 stride{};
    u32 offset{};
    u32 components{};
};

/// A capture of components that another TransformFeedbackVarying already captures: the same
/// attribute in a second buffer, or twice in one.
struct TransformFeedbackRepeat {
    u32 attribute{};
    TransformFeedbackVarying varying;
};

/// A blend factor that reads the blend constant.
enum class BlendConstantFactor : u8 {
    None,
    Color,
    OneMinusColor,
    Alpha,
    OneMinusAlpha,
};

struct RuntimeInfo {
    std::array<AttributeType, 32> generic_input_types{};
    /// D3D12: vertex inputs fetched in aligned parts because their bytes sit where the whole
    /// format cannot be fetched: part k at location N + 32k, each of
    /// generic_input_part_components components. 0 or 1 parts: fetched whole.
    std::array<u8, 32> generic_input_parts{};
    std::array<u8, 32> generic_input_part_components{};
    /// D3D12: components the guest format has, when the host fetches more (three 8- or 16-bit
    /// components widen to four). The rest read as 0, 0, 0, 1, like on Maxwell. 0: all four.
    std::array<u8, 32> generic_input_components{};
    VaryingState previous_stage_stores;
    std::map<IR::Attribute, IR::Attribute> previous_stage_legacy_stores_mapping;

    bool convert_depth_mode{};
    bool force_early_z{};

    /// Host workaround for direct VS -> FS pipelines: export a*clipW with smooth
    /// interpolation, then reconstruct a using reciprocal fragment W. Zero keeps
    /// native NoPerspective, including all other backends and intermediate stages.
    u32 lower_noperspective_mask{};

    TessPrimitive tess_primitive{};
    TessSpacing tess_spacing{};
    bool tess_clockwise{};

    InputTopology input_topology{};

    /// D3D12: vertices per patch the tessellation stage reads, as a constant (the DXIL
    /// translation has no patch vertices input). 0 reads the input.
    u32 patch_vertices{};

    std::optional<float> fixed_state_point_size;
    std::optional<CompareFunction> alpha_test_func;
    float alpha_test_reference{};

    /// Bit i: color output i goes to an 8-bit UNORM target without blending; its float is
    /// rounded to the 8-bit step with ties down (0.5 -> 127), as the guest GPU converts.
    u8 unorm8_output_mask{};
    /// Alpha-to-coverage computed in the fragment shader from output 0's alpha (a discard on
    /// single-sample targets): 0 off, 1 one-pixel footprint, 2 dithered 2x2, 3 virtual 1x1.
    u8 alpha_to_coverage_footprint{};
    /// The fragment shader outputs the coverage of a guest sample mask that changes with the
    /// pixel's position: four bits of samples turned off per position of the 2x2 footprint,
    /// (x & 1) | (y & 1) << 1 being its index. 0 leaves the mask to the host pipeline.
    u16 sample_mask_footprint{};
    /// Source blend factor of each color output, applied by the fragment shader to its RGB when
    /// the host's single blend constant has to serve the destination factors.
    std::array<BlendConstantFactor, 8> blend_constant_factors{};
    /// The fragment shader adds the depth bias to its depth (the host pipeline has none):
    /// constant * unit + slope * max(|dz/dx|, |dz/dy|), clamped by clamp when it is not 0.
    /// unit 0 is a float depth format: 2^(exponent(z) - 23).
    bool depth_bias_in_shader{};
    float depth_bias_unit{};
    float depth_bias_constant{};
    float depth_bias_slope{};
    float depth_bias_clamp{};

    /// Generic plus one with the pixels from the line's start (0: no line stipple). The fragment
    /// program discards where the guest's pattern, repeated factor times per bit, has a zero.
    u32 line_stipple_generic{};
    u32 line_stipple_factor{};
    u32 line_stipple_pattern{};
    /// The fragment program discards by the pattern after its stage constants.
    bool polygon_stipple{};
    /// Maxwell viewport swizzle applied to the last pre-raster stage's position (0: none).
    u16 viewport_swizzle{};

    /// Logic op the fragment shader applies to output 0: bit 0 keeps source & destination, bit 1
    /// source & ~destination, bit 2 ~source & destination and bit 3 ~source & ~destination.
    bool logic_op_enable{};
    u8 logic_op{};
    /// Bits of each channel of the UNORM target.
    std::array<u8, 4> logic_op_bits{};

    /// Static Y negate value
    bool y_negate{};
    /// Use storage buffers instead of global pointers on GLASM
    bool glasm_use_storage_buffers{};

    /// Transform feedback state for each varying
    std::array<TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_count{0};
    /// Captures that overlap one in xfb_varyings, which holds one per attribute.
    std::vector<TransformFeedbackRepeat> xfb_repeats;
    /// The vertex shader writes the varyings above to storage buffers instead of declaring them
    /// for stream output (transform_feedback_storage.h).
    bool xfb_storage{};
    /// Captures the shader cannot decorate in place (the position, the other built-ins and the
    /// repeats) go out through copies at free generic locations, stored before any position
    /// transform, so stream output reads the values the guest wrote (D3D12).
    bool xfb_copy_outputs{};

    /// Tessellation without the host tessellator (tessellation_emulation.h).
    TessEmulation tess_emulation{};
    /// The vertex shader of that tessellation: stores its tess_input_slots to the capture.
    bool tess_capture{};
    /// Slot plus one of each attribute group (attribute / 4) in a captured vertex (control
    /// inputs) and in a record's output vertex (control outputs, evaluation inputs); 0: absent.
    std::array<u8, 128> tess_input_slots{};
    std::array<u8, 128> tess_output_slots{};
    /// Slot plus one of each generic patch (IR::GenericPatchIndex) in a record; 0: absent.
    std::array<u8, 30> tess_patch_slots{};
    /// Slots of a captured vertex, of a record's output vertex, and generic patch slots.
    u32 tess_input_vertex_slots{};
    u32 tess_output_vertex_slots{};
    u32 tess_record_patch_slots{};
    /// Vertices of an input patch and of an output patch.
    u32 tess_input_vertices{};
    u32 tess_output_vertices{};

    /// Output types for each color attachment
    std::array<AttributeType, 8> color_output_types{};

    /// Dual source blending
    bool dual_source_blend{};
};

} // namespace Shader
