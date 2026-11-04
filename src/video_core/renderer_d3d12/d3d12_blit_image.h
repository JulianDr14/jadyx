// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/texture_cache/types.h"

namespace D3D12 {

class CpuDescriptorAllocator;
class DescriptorRing;
class SamplerHeap;
class Scheduler;
class ShaderCompiler;

/// How guest memory packs one depth-stencil texel (the layout_mode of the pack shaders). The
/// Maxwell names list the bits from the most significant (as Ryujinx and nouveau use them).
enum class DepthStencilLayout : u32 {
    Z24S8,      ///< S8_UINT_D24_UNORM: depth in bits 31:8, stencil in 7:0
    S8Z24,      ///< D24_UNORM_S8_UINT: stencil in bits 31:24, depth in 23:0
    X8Z24,      ///< X8_D24_UNORM: depth in bits 23:0, nothing in 31:24
    ZF32_X24S8, ///< D32_FLOAT_S8_UINT: the float depth, then a word with the stencil in 7:0
    S8,         ///< S8_UINT: one stencil byte, kept in the stencil plane of a D24S8 resource
};

/// One region repacked between guest depth-stencil texels and the copy footprints of both planes
/// of a D3D12 depth-stencil subresource (depth: 32-bit words, stencil: bytes). Offsets, rows and
/// pitches are in 32-bit words; the region is in texels of the footprints.
struct DepthStencilPack {
    /// Read through a root SRV: the guest texels (split) or the footprints (merge). 4-byte aligned.
    D3D12_GPU_VIRTUAL_ADDRESS source;
    /// Written through a root UAV: the footprints (split) or the guest texels (merge).
    D3D12_GPU_VIRTUAL_ADDRESS destination;
    u32 packed_offset;  ///< first guest texel, from the packed buffer's address
    u32 packed_row;     ///< words per guest row
    u32 depth_pitch;    ///< words per depth footprint row
    u32 stencil_offset; ///< start of the stencil footprint, from the footprints' address
    u32 stencil_pitch;  ///< words per stencil footprint row
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    u32 row_texels; ///< merge: guest texels per row (at least width; the rest is written as zero)
    DepthStencilLayout layout;
};

/// One band of a guest ASTC mip decoded on the GPU. Both descriptors are bounded so malformed
/// guest offsets cannot turn into an unbounded root-descriptor access.
struct AstcDecode {
    /// Raw R32_TYPELESS SRV containing the level's swizzled ASTC blocks.
    D3D12_CPU_DESCRIPTOR_HANDLE source;
    /// Offline UAV (R8G8B8A8_UNORM, Texture2DArray) of the level; copied into the ring.
    D3D12_CPU_DESCRIPTOR_HANDLE destination;
    u32 block_width;  ///< ASTC block size in texels
    u32 block_height;
    /// The block-linear layout (VideoCommon::Accelerated::BlockLinearSwizzle2DParams).
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 gob_block_height;
    u32 gob_block_height_mask;
    u32 blocks_x; ///< ASTC blocks per row and column of the level
    u32 blocks_y;
    u32 layers;
    u32 input_words;
    u32 first_block_row;
};

/// One guest BC1-5 or BC7 mip decoded on the GPU (d3d12_bcn_decoder.comp) into its
/// DecodedBcFormat texels. Both descriptors are bounded, as in AstcDecode.
struct BcnDecode {
    /// Raw R32_TYPELESS SRV containing the level's swizzled BC blocks.
    D3D12_CPU_DESCRIPTOR_HANDLE source;
    /// Offline UAV (the decoded format's UNORM or SNORM view, Texture2DArray) of the level.
    D3D12_CPU_DESCRIPTOR_HANDLE destination;
    u32 mode; ///< the shader's BCN_* format, BCN_SIGNED and BCN_GUEST_PALETTE bits
    /// The block-linear layout (VideoCommon::Accelerated::BlockLinearSwizzle2DParams).
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 gob_block_height;
    u32 gob_block_height_mask;
    u32 blocks_x; ///< 4x4 blocks per row and column of the level
    u32 blocks_y;
    u32 layers;
    u32 input_words;
};

/// One horizontal RGBA8 band encoded as BC3, BC1 without its alpha or BC4 of its alpha alone,
/// into a raw buffer footprint.
struct BcEncode {
    D3D12_CPU_DESCRIPTOR_HANDLE source;      ///< R8G8B8A8_UNORM Texture2D SRV
    D3D12_GPU_VIRTUAL_ADDRESS destination;   ///< raw buffer UAV
    u32 width;
    u32 height;
    u32 band_height;
    u32 output_row_words;
    bool bc1;
    bool bc4_alpha = false;
};

/// One band of a block-linear 2D level (one layer) unswizzled into a copy footprint. The shader
/// bounds its reads with source_bytes: the root SRV has no bounds of its own.
struct BlockLinearUnswizzle {
    D3D12_GPU_VIRTUAL_ADDRESS source;      ///< raw buffer, 4-byte aligned
    D3D12_GPU_VIRTUAL_ADDRESS destination; ///< raw buffer UAV, 4-byte aligned
    u32 source_offset;                     ///< bytes from source to the level (and layer)
    u32 source_bytes;                      ///< readable bytes from source
    u32 block_size;
    u32 x_shift;
    u32 block_height;
    u32 block_height_mask;
    u32 row_words; ///< words per row of texel blocks
    u32 rows;      ///< rows of texel blocks in the band
    u32 first_row;
    u32 row_pitch_words;
};

/// Draws Eden's host blit and clear shaders into guest images: the D3D12 counterpart of Vulkan's
/// BlitImageHelper. Serves scaled texture blits (Fermi2D), DrawTexture and the clears D3D12 cannot
/// express with ClearRenderTargetView/ClearDepthStencilView (partial color or stencil masks).
///
/// The caller transitions the images (sources to PIXEL_SHADER_RESOURCE, targets to RENDER_TARGET
/// or DEPTH_WRITE) and passes descriptors of them; the helper records a full-screen triangle with
/// its own root signature and PSO. Guest draws record their whole state, so nothing is restored.
class BlitImageHelper {
public:
    /// An offline sampler and the unique key SamplerHeap deduplicates tables by.
    struct Sampling {
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        u64 key;
    };

    /// A render target or depth-stencil view and what its PSO needs to know about it.
    struct Target {
        D3D12_CPU_DESCRIPTOR_HANDLE view;
        DXGI_FORMAT format; ///< the RTV or DSV format
        u32 samples;
    };

    BlitImageHelper(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                    DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap,
                    CpuDescriptorAllocator& sampler_descriptors);
    ~BlitImageHelper();

    BlitImageHelper(const BlitImageHelper&) = delete;
    BlitImageHelper& operator=(const BlitImageHelper&) = delete;

    /// False when the shader path is missing or a helper shader failed to translate (logged).
    [[nodiscard]] bool IsAvailable() const noexcept {
        return available;
    }

    [[nodiscard]] Sampling NearestSampler() const noexcept {
        return nearest_sampler;
    }
    [[nodiscard]] Sampling LinearSampler() const noexcept {
        return linear_sampler;
    }

    /// Draws src_region of a 2D color SRV (src_size texels) into dst_region of a color target.
    /// Every blit returns whether it drew: false leaves the target untouched.
    bool BlitColor(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv, const Sampling& sampler,
                   const VideoCommon::Region2D& dst_region,
                   const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size);

    /// False when a helper for multisampled sources failed to translate (logged).
    [[nodiscard]] bool CanBlitMultisampled() const noexcept {
        return multisample_available;
    }

    /// BlitColor from a Texture2DMSArray SRV: a resolve that averages the source's samples, or,
    /// with per_sample and a target of the same sample count, a copy of each sample.
    bool BlitColorMultisampled(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                               u32 src_samples, bool per_sample,
                               const VideoCommon::Region2D& dst_region,
                               const VideoCommon::Region2D& src_region,
                               const VideoCommon::Extent2D& src_size);

    /// BlitColor between integer formats of one signedness, without a float in between, point
    /// sampled. A multisampled source (src_samples > 1, a Texture2DMSArray SRV) gives each target
    /// sample one of its own: the same one at equal counts, sample 0 for a target of one, spread
    /// evenly between other counts.
    bool BlitInteger(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv, bool is_signed,
                     u32 src_samples, const VideoCommon::Region2D& dst_region,
                     const VideoCommon::Region2D& src_region,
                     const VideoCommon::Extent2D& src_size);

    /// Same for depth: the source's depth (its SRV's red channel) becomes the target's depth. A
    /// multisampled source (a Texture2DMSArray SRV) goes as in BlitInteger.
    bool BlitDepth(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                   const VideoCommon::Region2D& dst_region,
                   const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size,
                   u32 src_samples = 1, bool linear = false);

    /// Same for depth and stencil, without shader stencil export (the Series has no
    /// SV_StencilRef): one draw writes depth and a zero stencil, then one draw per bit sets that
    /// bit (write mask 1 << bit, reference 0xFF) where the texel of stencil_srv, a Texture2DArray
    /// (multisampled source: Texture2DMSArray) SRV of the source's stencil plane with the value in
    /// red, has it. Draws nothing unless every pipeline is available, so the target never keeps
    /// half a copy.
    bool BlitDepthStencil(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE depth_srv,
                          D3D12_CPU_DESCRIPTOR_HANDLE stencil_srv,
                          const VideoCommon::Region2D& dst_region,
                          const VideoCommon::Region2D& src_region,
                          const VideoCommon::Extent2D& src_size, u32 src_samples = 1,
                          bool linear = false);

    /// Writes color into the channels of color_mask (bit 0 red ... bit 3 alpha) inside rect.
    void ClearColor(const Target& dst, u8 color_mask, const std::array<f32, 4>& color,
                    const D3D12_RECT& rect);

    /// ClearColor of an integer target with the exact bits. False when the helper is missing; an
    /// empty rect counts as drawn.
    bool ClearColorInteger(const Target& dst, u8 color_mask, const std::array<u32, 4>& bits,
                           bool is_signed, const D3D12_RECT& rect);

    /// Writes depth (when clear_depth) and the stencil bits of stencil_mask inside rect.
    void ClearDepthStencil(const Target& dst, bool clear_depth, f32 depth, u8 stencil_mask,
                           u8 stencil_value, const D3D12_RECT& rect);

    /// False when the depth-stencil pack shaders are missing (logged): guest depth-stencil
    /// texels then cannot move to or from D3D12 depth-stencil resources.
    [[nodiscard]] bool CanPackDepthStencil() const noexcept {
        return pack_available;
    }
    /// Guest texels -> both footprints (d3d12_depth_stencil_split.comp). The caller puts the
    /// source in a shader-readable state and the footprints in UNORDERED_ACCESS.
    void SplitDepthStencil(const DepthStencilPack& pack);
    /// Both footprints -> guest texels (d3d12_depth_stencil_merge.comp), same states.
    void MergeDepthStencil(const DepthStencilPack& pack);
    /// Float bits -> 24-bit UNORM in the depth footprint at pack.destination, in place
    /// (d3d12_depth_unorm24.comp); reads depth_pitch, width and height. The caller puts the
    /// footprints in UNORDERED_ACCESS.
    void DepthFloatToUnorm24(const DepthStencilPack& pack);

    /// False when the ASTC decoder failed to translate or build (logged): ASTC is then decoded
    /// on the CPU.
    [[nodiscard]] bool CanDecodeAstc() const noexcept {
        return astc_available;
    }
    [[nodiscard]] bool CanEncodeBc() const noexcept {
        return bc3_available;
    }
    /// False when the BCn decoder failed to translate or build (logged): BC arrays are then
    /// decoded on the CPU.
    [[nodiscard]] bool CanDecodeBcn() const noexcept {
        return bcn_available;
    }
    /// Records the decode of one level; the caller puts the image in UNORDERED_ACCESS.
    void DecodeAstc(const AstcDecode& decode);
    /// Records the decode of one level; the caller puts the image in UNORDERED_ACCESS.
    void DecodeBcn(const BcnDecode& decode);
    void EncodeBc(const BcEncode& encode);

    /// False when the unswizzle shader failed to translate or build (logged): uploads are then
    /// unswizzled on the CPU.
    [[nodiscard]] bool CanUnswizzle() const noexcept {
        return unswizzle_available;
    }
    /// The caller puts the destination in UNORDERED_ACCESS.
    void UnswizzleBlockLinear(const BlockLinearUnswizzle& unswizzle);

private:
    enum class Kind : u8 {
        BlitColor,
        BlitDepth,
        ClearColor,
        ClearDepthStencil,
        BlitStencilBit,
        BlitColorMsAverage,
        BlitColorMsSample,
        BlitUint,
        BlitSint,
        BlitUintMs,
        BlitSintMs,
        BlitDepthMs,
        BlitStencilBitMs,
        ClearUint,
        ClearSint,
        MsaaToFlat,
        MsaaToFlatUint,
        MsaaToFlatSint,
        MsaaToFlatDepth,
        MsaaToFlatStencilBit,
        FlatToMsaa,
        FlatToMsaaUint,
        FlatToMsaaSint,
        FlatToMsaaDepth,
        FlatToMsaaStencilBit,
        // These two have no program of their own.
        BlitDepthStencil,   ///< BlitDepth's program, also zeroing the stencil
        BlitDepthStencilMs, ///< BlitDepthMs's program, also zeroing the stencil
        MsaaToFlatDepthStencil, ///< MsaaToFlatDepth's program, also zeroing the stencil
        FlatToMsaaDepthStencil, ///< FlatToMsaaDepth's program, also zeroing the stencil
    };
    static constexpr size_t NUM_PROGRAMS = static_cast<size_t>(Kind::BlitDepthStencil);

    struct Program {
        std::vector<u8> vs;
        std::vector<u8> ps;
    };

    /// The PSO of one kind for one target format; mask is the color write mask or the stencil
    /// write mask, depth whether a depth-stencil clear writes depth.
    ID3D12PipelineState* Pipeline(Kind kind, const Target& target, u8 mask, bool depth);
    void CreateRootSignature();
    void CreateSamplers(CpuDescriptorAllocator& sampler_descriptors);
    /// Binds the root signature, the PSO, the target and the viewport and scissor of rect.
    void Begin(ID3D12PipelineState* pipeline, const Target& target, bool is_depth,
               const D3D12_RECT& rect);
    /// The source's and the target's sample counts for the sample-by-sample blits.
    void SetSampleCounts(u32 src_samples, u32 dst_samples, bool average);

public:
    /// What CopyMultisample moves: the shader's texel type, or depth and maybe stencil.
    enum class MsaaCopy : u8 { Float, Uint, Sint, Depth, DepthStencil };

    /// Copies between a multisampled image and its single-sample alias, whose texels are the
    /// samples: scale_x by scale_y texels per pixel, sample index = x + scale_x * y, as the guest
    /// stores them. to_flat reads src_srv as a one-layer Texture2DMSArray into a single-sample
    /// target; otherwise it reads a one-level, one-layer Texture2DArray into a multisampled
    /// target. dst_rect is what it writes; src_offset is in the single-sample alias's texels, as
    /// the texture cache's ImageCopy gives it. DepthStencil also needs stencil_srv (same
    /// dimension, stencil in red) and writes the stencil bit by bit, as BlitDepthStencil.
    /// One draw (nine with stencil) per call, its pipelines cached like every blit's.
    bool CopyMultisample(const Target& dst, bool to_flat, MsaaCopy type,
                         D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                         D3D12_CPU_DESCRIPTOR_HANDLE stencil_srv, const D3D12_RECT& dst_rect,
                         s32 src_offset_x, s32 src_offset_y, s32 scale_x, s32 scale_y);

private:
    /// Begin, then the source mapping of a blit and its descriptor tables.
    void BeginBlit(ID3D12PipelineState* pipeline, const Target& dst, bool is_depth,
                   const D3D12_RECT& rect, D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                   D3D12_GPU_DESCRIPTOR_HANDLE sampler_table,
                   const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size);
    bool Blit(Kind kind, const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
              const Sampling& sampler, const VideoCommon::Region2D& dst_region,
              const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size,
              u32 src_samples = 1);
    /// Builds the pack root signature and both compute pipelines; throws on failure.
    void CreatePackPipelines(const ShaderCompiler& compiler);
    void DispatchPack(ID3D12PipelineState* pipeline, const DepthStencilPack& pack, u32 groups_x,
                      u32 groups_y);
    /// Builds the ASTC decoder's root signature and pipeline; throws on failure.
    void CreateAstcPipeline(const ShaderCompiler& compiler);
    void CreateBc3Pipeline(const ShaderCompiler& compiler);
    /// Builds the BCn decoder's pipeline on the ASTC root signature; throws on failure.
    void CreateBcnPipeline(const ShaderCompiler& compiler);
    /// Builds the block-linear unswizzle pipeline on the pack root signature; throws on failure.
    void CreateUnswizzlePipeline(const ShaderCompiler& compiler);

    const Device& device;
    Scheduler& scheduler;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;

    ComPtr<ID3D12RootSignature> root_signature;
    std::array<Program, NUM_PROGRAMS> programs;
    std::unordered_map<u64, ComPtr<ID3D12PipelineState>> pipelines;
    Sampling nearest_sampler{};
    Sampling linear_sampler{};
    bool available{};
    bool multisample_available{};
    bool integer_clear_available{};

    ComPtr<ID3D12RootSignature> pack_root_signature;
    ComPtr<ID3D12PipelineState> split_pipeline;
    ComPtr<ID3D12PipelineState> merge_pipeline;
    ComPtr<ID3D12PipelineState> unorm24_pipeline;
    bool pack_available{};

    ComPtr<ID3D12RootSignature> astc_root_signature;
    ComPtr<ID3D12PipelineState> astc_pipeline;
    bool astc_available{};

    ComPtr<ID3D12RootSignature> bc3_root_signature;
    ComPtr<ID3D12PipelineState> bc3_pipeline;
    bool bc3_available{};

    ComPtr<ID3D12PipelineState> bcn_pipeline;
    bool bcn_available{};

    ComPtr<ID3D12PipelineState> unswizzle_pipeline;
    bool unswizzle_available{};
};

} // namespace D3D12
