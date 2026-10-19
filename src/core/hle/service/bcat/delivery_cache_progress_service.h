// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/bcat/bcat_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::BCAT {
struct DeliveryCacheProgressImpl;
class IBcatService;

class IDeliveryCacheProgressService final : public ServiceFramework<IDeliveryCacheProgressService> {
public:
    explicit IDeliveryCacheProgressService(Core::System& system_,
                                           std::shared_ptr<IBcatService> parent_, SyncType type_,
                                           Kernel::KReadableEvent& event_,
                                           const DeliveryCacheProgressImpl& impl_);
    ~IDeliveryCacheProgressService() override;

    // Pulse: a view of its IBcatService's progress, rebuilt from that interface.
    static constexpr std::string_view PulseInterfaceKind = "bcat:IDeliveryCacheProgressService";
    std::string_view PulseKind() const override {
        return PulseInterfaceKind;
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    static std::shared_ptr<IDeliveryCacheProgressService> PulseRestore(
        Pulse::HandlerRestore& restore);

private:
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetImpl(OutLargeData<DeliveryCacheProgressImpl, BufferAttr_HipcPointer> out_impl);

    std::shared_ptr<IBcatService> parent; ///< Owns the event and the progress.
    SyncType type;
    Kernel::KReadableEvent& event;
    const DeliveryCacheProgressImpl& impl;
};

} // namespace Service::BCAT
