// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
struct ID3D12Device;
namespace D3D12 {
void RunLocalMemoryMatrix(ID3D12Device* device, const std::filesystem::path& shaders,
                          const std::filesystem::path& report) noexcept;
}
