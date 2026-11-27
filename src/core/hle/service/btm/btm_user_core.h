// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Core {
class System;
}

namespace Service::BTM {

/// btdrv BleAdvertisePacketParameter (libnx BtdrvBleAdvertisePacketParameter).
struct BleAdvertisePacketParameter {
    u16 company_id;
    std::array<u8, 6> pattern_data;
};
static_assert(sizeof(BleAdvertisePacketParameter) == 0x8);

/// btdrv GattAttributeUuid (libnx BtdrvGattAttributeUuid): byte length of the UUID, then the UUID.
struct GattAttributeUuid {
    u32 size;
    std::array<u8, 0x10> uuid;
};
static_assert(sizeof(GattAttributeUuid) == 0x14);

class IBtmUserCore final : public ServiceFramework<IBtmUserCore> {
public:
    explicit IBtmUserCore(Core::System& system_);
    ~IBtmUserCore() override;

private:
    Result AcquireBleScanEvent(Out<bool> out_is_valid,
                               OutCopyHandle<Kernel::KReadableEvent> out_event);

    Result GetBleScanFilterParameter(Out<BleAdvertisePacketParameter> out_parameter,
                                     u16 parameter_id);
    Result GetBleScanFilterParameter2(Out<GattAttributeUuid> out_uuid, u16 parameter_id);

    Result AcquireBleConnectionEvent(Out<bool> out_is_valid,
                                     OutCopyHandle<Kernel::KReadableEvent> out_event);

    Result AcquireBleServiceDiscoveryEvent(Out<bool> out_is_valid,
                                           OutCopyHandle<Kernel::KReadableEvent> out_event);

    Result RegisterBleGattDataPath();
    Result UnregisterBleGattDataPath();

    Result AcquireBleMtuConfigEvent(Out<bool> out_is_valid,
                                    OutCopyHandle<Kernel::KReadableEvent> out_event);

    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* scan_event;
    Kernel::KEvent* connection_event;
    Kernel::KEvent* service_discovery_event;
    Kernel::KEvent* config_event;
};

} // namespace Service::BTM
