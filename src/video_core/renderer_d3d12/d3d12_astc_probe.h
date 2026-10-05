// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace D3D12 {
// Runs without a guest, closes automatically, and never changes renderer texture interfaces.
// False means the isolated gate failed; absence of this opt-in does not create these resources.
bool RunAstcSamplingProbe(bool compiler_pressure_only = false);
} // namespace D3D12
