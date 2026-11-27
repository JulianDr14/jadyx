// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Service::LDN {

class ISystemLocalCommunicationService final
    : public ServiceFramework<ISystemLocalCommunicationService> {
public:
    explicit ISystemLocalCommunicationService(Core::System& system_);
    ~ISystemLocalCommunicationService() override;

private:
    Result AttachStateChangeEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result InitializeSystem2();

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* state_change_event;
};

} // namespace Service::LDN
