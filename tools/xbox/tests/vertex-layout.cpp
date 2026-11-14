// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Gate of the D3D12 vertex layout plan (d3d12_vertex_layout) and a CPU model of the repack shader
// (d3d12_vertex_realign.comp): every attribute the host fetches from the copy must hold the bytes
// the guest fetches from its stream.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_vertex_layout.h"

using D3D12::VertexAttributeDesc;
using D3D12::VertexLayout;
using D3D12::VertexRepack;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

VertexAttributeDesc Attr(u8 slot, u16 offset, u8 bytes, u8 component_bytes) {
    return {.enabled = true, .slot = slot, .bytes = bytes, .component_bytes = component_bytes,
            .offset = offset};
}

/// The shader, invocation by invocation: the source is bound at offset & ~3 and clamped to the
/// end of the bound range rounded up to a dword.
std::vector<u8> RunShader(const std::vector<u8>& guest, u32 offset, u32 size,
                          const VertexRepack& plan) {
    const u32 base = offset & ~3U;
    const u32 shift = offset & 3U;
    const u32 end = std::min<u32>(static_cast<u32>(guest.size()), (offset + size + 3U) & ~3U);
    const u32 src_words = (end - base) / 4U;
    const auto load_word = [&](u32 index) -> u32 {
        if (index >= src_words) {
            return 0;
        }
        u32 word;
        std::memcpy(&word, guest.data() + base + index * 4U, 4);
        return word;
    };
    const auto load_bytes = [&](u32 address) -> u32 {
        const u32 bit = (address & 3U) * 8U;
        const u32 low = load_word(address >> 2);
        return bit == 0 ? low : (low >> bit) | (load_word((address >> 2) + 1U) << (32U - bit));
    };
    std::vector<u8> dst(plan.Words() * 4U);
    for (u64 index = 0; index < plan.Words(); ++index) {
        const u32 vertex = static_cast<u32>(index / plan.words_per_vertex);
        const u32 word = static_cast<u32>(index % plan.words_per_vertex);
        u32 value = 0;
        if (plan.map.empty()) {
            value = load_bytes(shift + vertex * plan.src_stride + word * 4U);
        } else if (plan.map[word] != VertexRepack::NO_SOURCE) {
            value = load_bytes(shift + vertex * plan.src_stride + plan.map[word]);
        }
        std::memcpy(dst.data() + index * 4U, &value, 4);
    }
    return dst;
}

std::vector<u8> Pattern(size_t bytes) {
    std::vector<u8> guest(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        guest[i] = static_cast<u8>(i * 37U + 11U);
    }
    return guest;
}

/// Binds slot at offset and checks every complete vertex: the host attribute (HostOffset in a
/// vertex of view_stride, the copy 4-aligned) equals the guest one; returns the plan.
std::optional<VertexRepack> Verify(const VertexLayout& layout,
                                   const std::vector<VertexAttributeDesc>& attributes, u32 slot,
                                   u32 offset, u32 vertices, u32 stride, const char* what) {
    const u32 size = stride != 0 ? vertices * stride : 16U;
    const std::vector<u8> guest = Pattern(offset + size + 64U);
    const std::optional<VertexRepack> plan = layout.PlanBind(slot, offset, size, stride);
    if (!plan) {
        return plan;
    }
    const std::vector<u8> copy = RunShader(guest, offset, size, *plan);
    const u32 checked = stride != 0 ? vertices : 1U;
    for (u32 index = 0; index < attributes.size(); ++index) {
        const VertexAttributeDesc& attribute = attributes[index];
        if (!attribute.enabled || attribute.slot != slot) {
            continue;
        }
        const u32 host_offset = layout.HostOffset(index);
        Check(VertexLayout::Fetchable(attribute, host_offset, plan->view_stride), what);
        for (u32 vertex = 0; vertex < checked; ++vertex) {
            const u32 host = vertex * plan->view_stride + host_offset;
            const u32 source = offset + vertex * stride + attribute.offset;
            Check(host + attribute.bytes <= copy.size(), what);
            Check(std::memcmp(copy.data() + host, guest.data() + source, attribute.bytes) == 0,
                  what);
        }
    }
    return plan;
}

VertexLayout Build(const std::vector<VertexAttributeDesc>& attributes,
                   const std::vector<u16>& strides) {
    return VertexLayout::Build(attributes, strides);
}
} // namespace

int main() {
    // Stride 69, 32-bit floats: the stride rounds up to 72, offsets stay.
    {
        const std::vector<VertexAttributeDesc> attributes{Attr(0, 0, 12, 4), Attr(0, 12, 4, 1),
                                                          Attr(0, 16, 8, 4), Attr(0, 64, 4, 4)};
        const VertexLayout layout = Build(attributes, {69});
        Check(layout.RepackMask() == 1, "stride 69 floats repack slot 0");
        Check(layout.SlotMode(0) == VertexLayout::Mode::Align, "stride 69 is Align");
        Check(layout.HostStride(0) == 72, "stride 69 host stride 72");
        Check(layout.HostOffset(2) == 16, "Align keeps offsets");
        const auto plan = Verify(layout, attributes, 0, 0, 100, 69, "stride 69 copy");
        Check(plan && plan->view_stride == 72 && plan->count == 100, "stride 69 plan");
        Check(Verify(layout, attributes, 0, 6, 100, 69, "stride 69 at offset 6").has_value(),
              "stride 69 bound unaligned");
    }
    // A float at offset 2: no stride fixes it, every attribute moves to a packed dword.
    {
        const std::vector<VertexAttributeDesc> attributes{Attr(1, 2, 4, 4), Attr(1, 8, 8, 4),
                                                          Attr(0, 0, 4, 4)};
        const VertexLayout layout = Build(attributes, {16, 16});
        Check(layout.RepackMask() == 2, "only slot 1 repacks");
        Check(layout.SlotMode(0) == VertexLayout::Mode::Direct, "slot 0 stays");
        Check(layout.SlotMode(1) == VertexLayout::Mode::Remap, "offset 2 is Remap");
        Check(layout.HostOffset(0) == 0 && layout.HostOffset(1) == 4, "Remap packs offsets");
        Check(layout.HostOffset(2) == 0 && layout.HostStride(0) == 16, "Direct slot unchanged");
        Check(layout.HostStride(1) == 12, "Remap host stride");
        const auto plan = Verify(layout, attributes, 1, 0, 50, 16, "remap copy");
        Check(plan && plan->map.size() == 3 && plan->view_stride == 12, "remap plan");
        Check(Verify(layout, attributes, 1, 3, 50, 16, "remap at offset 3").has_value(),
              "remap bound unaligned");
        Check(!layout.PlanBind(0, 32, 64, 16), "aligned Direct slot binds in place");
    }
    // Several 8-bit components fetch in parts at any stride, 16-bit ones at even strides.
    {
        const VertexLayout bytes = Build({Attr(0, 1, 4, 1)}, {69});
        Check(bytes.RepackMask() == 0, "8-bit split needs no repack");
        Check(!bytes.PlanBind(0, 0, 690, 69), "8-bit split binds in place");
        const VertexLayout halves = Build({Attr(0, 6, 4, 2)}, {70});
        Check(halves.RepackMask() == 0, "16-bit split at even stride needs no repack");
        const std::vector<VertexAttributeDesc> odd{Attr(0, 6, 4, 2)};
        const VertexLayout halves_odd = Build(odd, {69});
        Check(halves_odd.SlotMode(0) == VertexLayout::Mode::Align, "16-bit at stride 69 aligns");
        Check(Verify(halves_odd, odd, 0, 0, 30, 69, "16-bit align copy").has_value(),
              "16-bit align plan");
    }
    // A Direct stream bound at an unaligned offset moves as one block, stride kept.
    {
        const std::vector<VertexAttributeDesc> attributes{Attr(0, 0, 4, 4), Attr(0, 4, 4, 1)};
        const VertexLayout layout = Build(attributes, {8});
        Check(layout.RepackMask() == 0, "aligned stream needs no repack");
        const auto plan = Verify(layout, attributes, 0, 6, 40, 8, "linear copy");
        Check(plan && plan->src_stride == 0 && plan->count == 1 && plan->view_stride == 8,
              "linear plan");
        Check(plan->words_per_vertex == 80, "linear plan copies the range");
    }
    // Stride 0: one vertex for every draw, still repacked.
    {
        const std::vector<VertexAttributeDesc> attributes{Attr(0, 2, 4, 4)};
        const VertexLayout layout = Build(attributes, {0});
        Check(layout.SlotMode(0) == VertexLayout::Mode::Remap, "stride 0 offset 2 is Remap");
        const auto plan = Verify(layout, attributes, 0, 0, 1, 0, "stride 0 copy");
        Check(plan && plan->view_stride == 0 && plan->count == 1, "stride 0 plan");
    }
    // Signatures tell layouts of the same bytes apart.
    {
        const VertexLayout a = Build({Attr(0, 2, 4, 4)}, {16});
        const VertexLayout b = Build({Attr(0, 6, 4, 4)}, {16});
        const VertexLayout c = Build({Attr(0, 0, 4, 4)}, {69});
        const auto pa = a.PlanBind(0, 0, 160, 16);
        const auto pb = b.PlanBind(0, 0, 160, 16);
        const auto pc = c.PlanBind(0, 0, 690, 69);
        const auto pl = VertexLayout::PlanDirectBind(2, 160, 16);
        Check(pa && pb && pc && pl, "signature plans");
        Check(pa->signature != pb->signature, "remap maps differ");
        Check(pa->signature != pc->signature && pc->signature != pl->signature &&
                  pa->signature != pl->signature,
              "modes differ");
    }
    // Per-draw cost of planning a bind.
    {
        const VertexLayout layout = Build({Attr(0, 0, 12, 4), Attr(1, 2, 4, 4)}, {69, 16});
        const auto start = std::chrono::steady_clock::now();
        u64 sum = 0;
        constexpr u32 ITERATIONS = 10'000'000;
        for (u32 i = 0; i < ITERATIONS; ++i) {
            const auto plan = layout.PlanBind(i & 1U, (i >> 1) & 4U, 6900, (i & 1U) ? 16 : 69);
            sum += plan ? plan->signature : 0;
        }
        const double ns = std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - start)
                              .count() /
                          ITERATIONS;
        std::printf("PlanBind: %.2f ns per bind (%llu)\n", ns,
                    static_cast<unsigned long long>(sum & 1));
    }
    std::printf("vertex layout gate: OK\n");
    return 0;
}
