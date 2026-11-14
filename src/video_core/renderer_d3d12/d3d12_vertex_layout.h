// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>

#include "common/common_types.h"

namespace D3D12 {

/// What the host input assembler fetches for one guest vertex attribute.
struct VertexAttributeDesc {
    bool enabled{};
    u8 slot{};
    /// Bytes the host format reads: three 8- and 16-bit components widen to four.
    u8 bytes{};
    /// Size of one component. Formats of several 1- or 2-byte components can be fetched in
    /// aligned parts (MaxwellToD3D12::SplitAttributeFetch); the others need min(4, bytes).
    u8 component_bytes{};
    u16 offset{};
};

/// A copy of one vertex stream into memory the input assembler can fetch: destination dword w of
/// vertex v holds the 4 source bytes at v * src_stride + map[w] (4 * w without a map; zero for
/// NO_SOURCE), counted from the bound offset.
struct VertexRepack {
    static constexpr u16 NO_SOURCE = 0xFFFF;

    u32 src_stride{};
    u32 count{};            ///< vertices
    u32 words_per_vertex{}; ///< destination dwords per vertex
    std::span<const u16> map;
    u32 view_stride{}; ///< StrideInBytes of the bound copy
    u64 signature{};   ///< tells repacks of the same source bytes apart, for caches

    [[nodiscard]] u64 Words() const noexcept {
        return u64{count} * words_per_vertex;
    }
};

/// Host placement of the vertex attributes of a pipeline. Maxwell fetches an attribute at any
/// byte address; D3D12 requires min(4, size) alignment and, past what parts can fix, AMD input
/// assemblers fetch garbage (giant triangles from 32-bit floats at a stride of 69). A slot whose
/// attributes D3D12 cannot fetch where the guest put them is repacked when bound
/// (VertexRealigner) into a layout this plan decides, and the pipeline's input layout and shader
/// fetch split read the host offsets and strides (HostOffset/HostStride).
///
/// Pure and cheap: built once per pipeline from its key, and the same answer wherever a
/// pipeline asks (input layout, runtime info, fetch fixups) and wherever a stream is bound.
class VertexLayout {
public:
    static constexpr u32 NUM_SLOTS = 32;
    static constexpr u32 NUM_ATTRIBUTES = 32;
    static constexpr u32 MAX_STRIDE = 2048; ///< D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES

    enum class Mode : u8 {
        Direct, ///< fetched in place, as the guest laid it out
        Align,  ///< offsets stay, the stride rounds up to a multiple of 4
        Remap,  ///< every attribute moves to a 4-aligned offset of a packed vertex
    };

    VertexLayout() = default;

    [[nodiscard]] static VertexLayout Build(std::span<const VertexAttributeDesc> attributes,
                                            std::span<const u16> strides);

    /// Whether D3D12 fetches the attribute right at this offset in vertices this far apart (with
    /// the stream itself 4-aligned).
    [[nodiscard]] static bool Fetchable(const VertexAttributeDesc& attribute, u32 offset,
                                        u32 stride) noexcept;

    [[nodiscard]] Mode SlotMode(u32 slot) const noexcept {
        return slots[slot].mode;
    }
    /// Slots that are repacked whatever offset they are bound at.
    [[nodiscard]] u32 RepackMask() const noexcept {
        return repack_mask;
    }
    /// Offset of the attribute in the vertex the input assembler sees.
    [[nodiscard]] u32 HostOffset(u32 attribute) const noexcept {
        return host_offsets[attribute];
    }
    /// Stride the pipeline assumes for the slot: the guest one for Direct slots.
    [[nodiscard]] u32 HostStride(u32 slot) const noexcept {
        return slots[slot].host_stride;
    }

    /// How a stream bound to slot is repacked; empty when it is bound in place. A Direct slot is
    /// only repacked when its offset is not 4-aligned, keeping its stride.
    [[nodiscard]] std::optional<VertexRepack> PlanBind(u32 slot, u32 offset, u32 size,
                                                       u32 stride) const noexcept;
    /// PlanBind of a pipeline whose slots are all Direct.
    [[nodiscard]] static std::optional<VertexRepack> PlanDirectBind(u32 offset, u32 size,
                                                                    u32 stride) noexcept;

private:
    struct Slot {
        Mode mode{Mode::Direct};
        u16 host_stride{};
        u16 first_word{}; ///< Remap: where its map starts in words
        u32 signature{};  ///< Align and Remap: hash of the layout
    };

    std::array<Slot, NUM_SLOTS> slots{};
    std::array<u16, NUM_ATTRIBUTES> host_offsets{};
    u32 repack_mask{};
    /// Remap word maps of every slot, source byte per destination dword: 32 attributes of at most
    /// 16 bytes each.
    std::array<u16, NUM_ATTRIBUTES * 4> words{};
};

} // namespace D3D12
