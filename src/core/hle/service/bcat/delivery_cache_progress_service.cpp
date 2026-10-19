// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/bcat_service.h"
#include "core/hle/service/bcat/bcat_types.h"
#include "core/hle/service/bcat/delivery_cache_progress_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheProgressService::IDeliveryCacheProgressService(
    Core::System& system_, std::shared_ptr<IBcatService> parent_, SyncType type_,
    Kernel::KReadableEvent& event_, const DeliveryCacheProgressImpl& impl_)
    : ServiceFramework{system_, "IDeliveryCacheProgressService"}, parent{std::move(parent_)},
      type{type_}, event{event_}, impl{impl_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryCacheProgressService::GetEvent>, "Get"},
        {1, D<&IDeliveryCacheProgressService::GetImpl>, "Get"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDeliveryCacheProgressService::~IDeliveryCacheProgressService() = default;

void IDeliveryCacheProgressService::PulseCapture(Pulse::HandlerCapture& capture) const {
    capture.state.U64(capture.Reference(parent));
    capture.state.U8(static_cast<u8>(type));
}

std::shared_ptr<IDeliveryCacheProgressService> IDeliveryCacheProgressService::PulseRestore(
    Pulse::HandlerRestore& restore) {
    auto& in = restore.state;
    auto parent = restore.Resolve(in.U64());
    const auto type = in.U8();
    if (!parent || parent->PulseKind() != IBcatService::PulseInterfaceKind ||
        type >= static_cast<u8>(SyncType::Count)) {
        throw std::invalid_argument("Pulse: bcat progress without its IBcatService");
    }
    return std::static_pointer_cast<IBcatService>(parent)->CreateProgressService(
        static_cast<SyncType>(type));
}

Result IDeliveryCacheProgressService::GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_BCAT, "called");

    *out_event = &event;
    R_SUCCEED();
}

Result IDeliveryCacheProgressService::GetImpl(
    OutLargeData<DeliveryCacheProgressImpl, BufferAttr_HipcPointer> out_impl) {
    LOG_DEBUG(Service_BCAT, "called");

    *out_impl = impl;
    R_SUCCEED();
}

} // namespace Service::BCAT
