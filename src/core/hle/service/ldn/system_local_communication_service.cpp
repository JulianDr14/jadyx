// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ldn/system_local_communication_service.h"

namespace Service::LDN {

ISystemLocalCommunicationService::ISystemLocalCommunicationService(Core::System& system_)
    : ServiceFramework{system_, "ISystemLocalCommunicationService"},
      service_context{system_, "ISystemLocalCommunicationService"} {
    // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetState"},
            {1, nullptr, "GetNetworkInfo"},
            {2, nullptr, "GetIpv4Address"},
            {3, nullptr, "GetDisconnectReason"},
            {4, nullptr, "GetSecurityParameter"},
            {5, nullptr, "GetNetworkConfig"},
            {100, D<&ISystemLocalCommunicationService::AttachStateChangeEvent>, "AttachStateChangeEvent"},
            {101, nullptr, "GetNetworkInfoLatestUpdate"},
            {102, nullptr, "Scan"},
            {103, nullptr, "ScanPrivate"},
            {104, nullptr, "SetWirelessControllerRestriction"},
            {200, nullptr, "OpenAccessPoint"},
            {201, nullptr, "CloseAccessPoint"},
            {202, nullptr, "CreateNetwork"},
            {203, nullptr, "CreateNetworkPrivate"},
            {204, nullptr, "DestroyNetwork"},
            {205, nullptr, "Reject"},
            {206, nullptr, "SetAdvertiseData"},
            {207, nullptr, "SetStationAcceptPolicy"},
            {208, nullptr, "AddAcceptFilterEntry"},
            {209, nullptr, "ClearAcceptFilter"},
            {300, nullptr, "OpenStation"},
            {301, nullptr, "CloseStation"},
            {302, nullptr, "Connect"},
            {303, nullptr, "ConnectPrivate"},
            {304, nullptr, "Disconnect"},
            {400, nullptr, "InitializeSystem"},
            {401, nullptr, "FinalizeSystem"},
            {402, nullptr, "SetOperationMode"},
            {403, C<&ISystemLocalCommunicationService::InitializeSystem2>, "InitializeSystem2"},
        };
    // clang-format on

    RegisterHandlers(functions);

    state_change_event =
        service_context.CreateEvent("ISystemLocalCommunicationService:StateChangeEvent");
}

ISystemLocalCommunicationService::~ISystemLocalCommunicationService() {
    service_context.CloseEvent(state_change_event);
}

// switchbrew (GetStateChangeEvent): no input, one event handle; sdknso aborts when it fails. The
// system interface has no network of its own here, so the state never changes and the event is
// never signaled.
Result ISystemLocalCommunicationService::AttachStateChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_LDN, "called");
    *out_event = &state_change_event->GetReadableEvent();
    R_SUCCEED();
}

Result ISystemLocalCommunicationService::InitializeSystem2() {
    LOG_WARNING(Service_LDN, "(STUBBED) called");
    R_SUCCEED();
}

} // namespace Service::LDN
