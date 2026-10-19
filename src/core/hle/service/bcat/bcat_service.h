// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/bcat/bcat_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::BCAT {
class BcatBackend;
class IDeliveryCacheStorageService;
class IDeliveryCacheProgressService;

class IBcatService final : public ServiceFramework<IBcatService> {
public:
    explicit IBcatService(Core::System& system_, BcatBackend& backend_, u64 program_id_);
    ~IBcatService() override;

    // Pulse: the backend is the creator's and keeps nothing; the progress of each sync type and
    // its update event are this interface's.
    static constexpr std::string_view PulseInterfaceKind = "bcat:IBcatService";
    std::string_view PulseKind() const override {
        return PulseInterfaceKind;
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    Kernel::KEvent* PulseEvent(u32 slot) override;
    /// Rebuilds an interface of the creator's backend from a captured state.
    static std::shared_ptr<IBcatService> PulseRestore(Core::System& system, BcatBackend& backend,
                                                      Pulse::StateReader& in);

    ProgressServiceBackend& GetProgressBackend(SyncType type);
    /// An interface over the progress of a sync type; it keeps this interface alive.
    std::shared_ptr<IDeliveryCacheProgressService> CreateProgressService(SyncType type);

private:
    Result RequestSyncDeliveryCache(OutInterface<IDeliveryCacheProgressService> out_interface);

    Result RequestSyncDeliveryCacheWithDirectoryName(
        const DirectoryName& name, OutInterface<IDeliveryCacheProgressService> out_interface);

    Result SetPassphrase(u64 application_id, InBuffer<BufferAttr_HipcPointer> passphrase_buffer);

    Result RegisterSystemApplicationDeliveryTasks();

    Result ClearDeliveryCacheStorage(u64 application_id);

private:
    const ProgressServiceBackend& GetProgressBackend(SyncType type) const;

    BcatBackend& backend;
    u64 program_id;
    std::array<ProgressServiceBackend, static_cast<size_t>(SyncType::Count)> progress;
};

} // namespace Service::BCAT
