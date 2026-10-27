// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

void IStorage::PulseCapture(Pulse::HandlerCapture& capture) const {
    if (pulse_origin == PulseOrigin::None || !pulse_opener) {
        throw Pulse::ServiceRejected(false, "IStorage opened by a call Pulse cannot replay");
    }
    capture.state.U64(capture.Reference(pulse_opener));
    capture.state.U32(static_cast<u32>(pulse_origin));
    capture.state.U32(static_cast<u32>(pulse_args.size()));
    capture.state.Bytes(pulse_args);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // Read pacing from Eden #4525: caps each storage near Switch media speed (40 MB/s, at most
    // 64 KiB charged per read, so ~1.6 ms per read at worst). Guests that stream movies can run
    // out of memory or freeze when replies outpace them. It replaced our NVDEC-only 8 ms sleep
    // per read. The backend read counts toward the budget, so a slow disk adds no extra wait,
    // but loads made of many small reads may take longer: if a load time regression shows up
    // on the Series, compare against a build without this block first.
    const auto now = std::chrono::steady_clock::now();
    if (now < next_read_tick) {
        std::this_thread::sleep_for(next_read_tick - now);
    }
    next_read_tick = std::chrono::steady_clock::now() +
                     std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                              1'000'000'000 / ReadBytesPerSecond};

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
