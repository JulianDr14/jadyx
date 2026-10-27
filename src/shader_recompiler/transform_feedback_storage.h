// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// Transform feedback written by the vertex shader into storage buffers, for hosts without stream
/// output the backend can use (RuntimeInfo::xfb_storage). At its epilogue, before any position
/// transform, each vertex stores the captured components of its outputs at
///   (vertex - first vertex + instance * vertex_count) * stride + offset
/// of their buffer, skipping words past the buffer's size. Like Maxwell, vertex N of a
/// non-indexed list draw lands at slot N; indexed and strip draws do not keep that order.
///
/// When the guest disables the rasterizer, the vertex is then moved behind the viewer, so the
/// draw only writes the buffers.
namespace Shader::TransformFeedbackStorage {

/// Descriptor set (register space in D3D12): buffer N is storage buffer binding N (u<N>), the
/// constants a uniform buffer at CONSTANTS_BINDING (b<CONSTANTS_BINDING>).
constexpr u32 DESCRIPTOR_SET = 27;
constexpr u32 NUM_BUFFERS = 4;
constexpr u32 CONSTANTS_BINDING = NUM_BUFFERS;

/// Constants, in 32-bit words.
constexpr u32 VERTEX_COUNT_WORD = 0;
/// Non-zero: the guest rasterizer is disabled.
constexpr u32 DISCARD_WORD = 1;
/// Words each buffer holds, from its first byte.
constexpr u32 SIZE_WORD = 4;
constexpr u32 CONSTANT_WORDS = SIZE_WORD + NUM_BUFFERS;

} // namespace Shader::TransformFeedbackStorage
