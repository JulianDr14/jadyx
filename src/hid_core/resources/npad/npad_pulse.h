// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/hle/service/pulse_service.h"
#include "hid_core/resources/npad/npad_data.h"

namespace Service::HID {

inline void SaveNpadPulseState(Pulse::StateWriter& out, NpadRevision revision,
                              const NPadData& data) {
    out.U32(static_cast<u32>(revision));
    data.PulseSave(out);
}

inline NpadRevision LoadNpadPulseState(Pulse::StateReader& in, NPadData& data) {
    // ActivateNpadWithRevision accepts a u32, including revisions not named by our enum.
    // Preserve that value: the live HID code already handles it through its default masks.
    // Restricting only restore to Revision3 rejects states this same build can save.
    const auto revision = static_cast<NpadRevision>(in.U32());
    data.PulseLoad(in);
    return revision;
}

} // namespace Service::HID
