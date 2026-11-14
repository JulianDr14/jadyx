// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>

#include "video_core/renderer_d3d12/d3d12_vertex_layout.h"

namespace D3D12 {

namespace {

constexpr u32 AlignUp4(u32 value) noexcept {
    return (value + 3U) & ~3U;
}

constexpr u32 DivCeil(u32 value, u32 divisor) noexcept {
    return static_cast<u32>((u64{value} + divisor - 1) / divisor);
}

constexpr u32 Mix(u32 hash, u32 value) noexcept {
    hash ^= value + 0x9E3779B9U + (hash << 6) + (hash >> 2);
    return hash;
}

/// Signatures of a repack layout (VertexRepack::signature); the low byte is the mode.
constexpr u64 LINEAR_SIGNATURE = 1;
constexpr u64 ALIGN_TAG = 2;
constexpr u64 REMAP_TAG = 3;

} // Anonymous namespace

bool VertexLayout::Fetchable(const VertexAttributeDesc& attribute, u32 offset,
                             u32 stride) noexcept {
    // Largest power of two (up to 4) every vertex's address of the attribute is aligned to.
    const u32 bits = offset | stride | 4U;
    const u32 alignment = bits & (~bits + 1U);
    if (alignment >= std::min<u32>(4U, attribute.bytes)) {
        return true;
    }
    // Several 1- or 2-byte components: fetched in aligned parts the shader joins.
    return attribute.component_bytes < attribute.bytes && alignment >= attribute.component_bytes;
}

VertexLayout VertexLayout::Build(std::span<const VertexAttributeDesc> attributes,
                                 std::span<const u16> strides) {
    VertexLayout layout;
    const size_t num_attributes = std::min<size_t>(attributes.size(), NUM_ATTRIBUTES);
    const auto guest_stride = [&](u32 slot) -> u32 {
        return slot < strides.size() ? strides[slot] : 0U;
    };
    for (u32 slot = 0; slot < NUM_SLOTS; ++slot) {
        layout.slots[slot].host_stride = static_cast<u16>(guest_stride(slot));
    }
    u32 broken = 0;      // slots with an attribute D3D12 cannot fetch in place
    u32 not_aligned = 0; // ... nor once the stride is a multiple of 4
    for (size_t index = 0; index < num_attributes; ++index) {
        const VertexAttributeDesc& attribute = attributes[index];
        layout.host_offsets[index] = attribute.offset;
        if (!attribute.enabled || attribute.slot >= NUM_SLOTS) {
            continue;
        }
        const u32 bit = 1U << attribute.slot;
        const u32 stride = guest_stride(attribute.slot);
        if (!Fetchable(attribute, attribute.offset, stride)) {
            broken |= bit;
        }
        // Rounding the stride up only helps when the offset itself is fine and the copied
        // vertex still holds the whole attribute.
        const u32 aligned = AlignUp4(stride);
        if (stride == 0 || aligned > MAX_STRIDE || u32{attribute.offset} + attribute.bytes > aligned ||
            !Fetchable(attribute, attribute.offset, 0)) {
            not_aligned |= bit;
        }
    }
    layout.repack_mask = broken;
    u32 next_word = 0;
    for (u32 mask = broken; mask != 0; mask &= mask - 1) {
        const u32 slot = static_cast<u32>(std::countr_zero(mask));
        Slot& entry = layout.slots[slot];
        if ((not_aligned & (1U << slot)) == 0) {
            entry.mode = Mode::Align;
            entry.host_stride = static_cast<u16>(AlignUp4(guest_stride(slot)));
            entry.signature = entry.host_stride;
            continue;
        }
        // Each attribute gets its own 4-aligned run of dwords, in attribute order.
        entry.mode = Mode::Remap;
        entry.first_word = static_cast<u16>(next_word);
        u32 hash = slot;
        u32 cursor = 0;
        for (size_t index = 0; index < num_attributes; ++index) {
            const VertexAttributeDesc& attribute = attributes[index];
            if (!attribute.enabled || attribute.slot != slot) {
                continue;
            }
            layout.host_offsets[index] = static_cast<u16>(cursor);
            const u32 num_words = DivCeil(std::max<u32>(attribute.bytes, 1U), 4U);
            for (u32 word = 0; word < num_words; ++word) {
                const u32 source = attribute.offset + word * 4U;
                layout.words[next_word++] = static_cast<u16>(source);
                hash = Mix(hash, source);
            }
            cursor += num_words * 4U;
        }
        entry.host_stride = static_cast<u16>(cursor);
        entry.signature = Mix(hash, cursor);
    }
    return layout;
}

std::optional<VertexRepack> VertexLayout::PlanBind(u32 slot, u32 offset, u32 size,
                                                   u32 stride) const noexcept {
    if (slot >= NUM_SLOTS || size == 0) {
        return std::nullopt;
    }
    const Slot& entry = slots[slot];
    switch (entry.mode) {
    case Mode::Direct:
        return PlanDirectBind(offset, size, stride);
    case Mode::Align: {
        if (stride == 0) {
            return PlanDirectBind(offset, size, stride);
        }
        // The bound stride is the key's; any multiple of 4 fetches the same split.
        const u32 host_stride = AlignUp4(stride);
        return VertexRepack{
            .src_stride = stride,
            .count = DivCeil(size, stride),
            .words_per_vertex = host_stride / 4U,
            .map = {},
            .view_stride = host_stride,
            .signature = (u64{host_stride} << 8) | ALIGN_TAG,
        };
    }
    case Mode::Remap: {
        const u32 num_words = entry.host_stride / 4U;
        return VertexRepack{
            .src_stride = stride,
            .count = stride != 0 ? DivCeil(size, stride) : 1U,
            .words_per_vertex = num_words,
            .map = std::span<const u16>(words).subspan(entry.first_word, num_words),
            .view_stride = stride != 0 ? entry.host_stride : 0U,
            .signature = (u64{entry.signature} << 8) | REMAP_TAG,
        };
    }
    }
    return std::nullopt;
}

std::optional<VertexRepack> VertexLayout::PlanDirectBind(u32 offset, u32 size,
                                                         u32 stride) noexcept {
    if ((offset & 3U) == 0 || size == 0) {
        return std::nullopt;
    }
    // The stream moves to a 4-aligned address as one block; every vertex keeps its place.
    return VertexRepack{
        .src_stride = 0,
        .count = 1,
        .words_per_vertex = DivCeil(size, 4U),
        .map = {},
        .view_stride = stride,
        .signature = LINEAR_SIGNATURE,
    };
}

} // namespace D3D12
