// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"

namespace Service::FileSystem {

IFile::IFile(Core::System& system_, FileSys::VirtualFile file_)
    : ServiceFramework{system_, "IFile"}, backend{std::make_unique<FileSys::Fsa::IFile>(file_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IFile::Read>, "Read"},
        {1, D<&IFile::Write>, "Write"},
        {2, D<&IFile::Flush>, "Flush"},
        {3, D<&IFile::SetSize>, "SetSize"},
        {4, D<&IFile::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
        {6, nullptr, "OperateRangeWithBuffer"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

void IFile::PulseCapture(Pulse::HandlerCapture& capture) const {
    if (!pulse_parent) throw Pulse::ServiceRejected(false, "IFile without the file system that opened it");
    // Writes the guest already saw succeed must be on disk before the checkpoint is published.
    if (!CommitForCheckpoint()) throw Pulse::ServiceRejected(true, "IFile could not commit its writes");
    capture.state.U64(capture.Reference(pulse_parent));
    capture.state.String(pulse_path);
    capture.state.U32(pulse_mode);
}

Result IFile::Read(
    FileSys::ReadOption option, Out<s64> out_size, s64 offset,
    const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer,
    s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    R_UNLESS(size >= 0, FileSys::ResultInvalidSize);

    // The guest's size may exceed the buffer it mapped. The console's fs server reads at most the
    // buffer's size, so an empty buffer reads 0 bytes instead of failing with a null buffer, and a
    // short one is never written past its end.
    const size_t read_size = std::min(out_buffer.size(), static_cast<size_t>(size));
    if (read_size == 0) {
        *out_size = 0;
        R_SUCCEED();
    }

    // Read the data from the Storage backend
    R_RETURN(backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(),
                           read_size));
}

Result IFile::Write(
    const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer,
    FileSys::WriteOption option, s64 offset, s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    // Never read past the buffer the guest mapped.
    R_UNLESS(size >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(static_cast<size_t>(size) <= buffer.size(), FileSys::ResultInvalidSize);

    R_RETURN(backend->Write(offset, buffer.data(), size, option));
}

Result IFile::Flush() {
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->Flush());
}

Result IFile::SetSize(s64 size) {
    LOG_DEBUG(Service_FS, "called, size={}", size);

    R_RETURN(backend->SetSize(size));
}

Result IFile::GetSize(Out<s64> out_size) {
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->GetSize(out_size));
}

} // namespace Service::FileSystem
