// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

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

    R_UNLESS(static_cast<u64>(length) <= out_bytes.size(), FileSys::ResultInvalidSize);

    const u64 storage_size = backend->GetSize();
    R_UNLESS(static_cast<u64>(offset) <= storage_size &&
                 static_cast<u64>(length) <= storage_size - static_cast<u64>(offset),
             FileSys::ResultOutOfRange);

    if (length == 0) {
        R_SUCCEED();
    }

    // IStorage has no bytes-read output: success promises the entire requested range.
    const auto read = backend->Read(out_bytes.data(), static_cast<std::size_t>(length),
                                    static_cast<std::size_t>(offset));
    if (read != static_cast<u64>(length)) {
        LOG_ERROR(Service_FS, "Storage read failed, offset={:#x}, requested={}, actual={}", offset,
                  length, read);
        R_THROW(ResultUnknown);
    }

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
