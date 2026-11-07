// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_guest_memory.h"

#include <cstring>

#include "common/host_memory.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Jadyx::MemoryEdit {

namespace {

constexpr u64 PAGE_BYTES = 4096;
constexpr u64 HOST_CUT = 2 * 1024 * 1024;

bool HoldsGameValues(const Kernel::KMemoryInfo& info) {
    using State = Kernel::KMemoryState;
    using Permission = Kernel::KMemoryPermission;
    const State state = info.GetState();
    return (state == State::Normal || state == State::CodeData || state == State::AliasCodeData) &&
           (info.GetPermission() & Permission::UserReadWrite) == Permission::UserReadWrite;
}

} // namespace

GuestMemoryMap WalkGuestMemory(Core::System& system) {
    GuestMemoryMap map;
    Kernel::KProcess* const process = system.ApplicationProcess();
    if (process == nullptr) return map;
    auto& table = process->GetPageTable();
    auto& memory = process->GetMemory();
    const u64 start = GetInteger(table.GetAddressSpaceStart());
    const u64 end = start + table.GetAddressSpaceSize();
    for (u64 address = start; address < end;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page_info{};
        if (table.QueryInfo(&info, &page_info, address).IsError() || info.GetSize() == 0 ||
            info.GetEndAddress() <= address) {
            break;
        }
        const u64 block_end = info.GetEndAddress();
        if (HoldsGameValues(info)) {
            for (u64 va = address; va < block_end; va += PAGE_BYTES) {
                const u8* const host = memory.GetPointerSilent(va);
                if (host == nullptr) continue;
                GuestSpan* const last = map.spans.empty() ? nullptr : &map.spans.back();
                if (last != nullptr && last->va + last->pages * PAGE_BYTES == va &&
                    last->host + last->pages * PAGE_BYTES == host &&
                    reinterpret_cast<uintptr_t>(host) % HOST_CUT != 0) {
                    ++last->pages;
                } else {
                    map.spans.push_back({va, host, 1, static_cast<u32>(map.pages)});
                }
                ++map.pages;
            }
        }
        address = block_end;
    }
    return map;
}

bool CanSearchGuest(Core::System& system) {
    Kernel::KProcess* const process = system.ApplicationProcess();
    if (process == nullptr) return false;
    const u8* const host = process->GetMemory().GetPointerSilent(process->GetEntryPoint());
    return host != nullptr && Common::QueryDramRegion(host).has_value();
}

std::optional<u64> ReadGuestValue(Core::System& system, u64 va, ValueKind kind) {
    Kernel::KProcess* const process = system.ApplicationProcess();
    if (process == nullptr) return std::nullopt;
    // Not ReadBlock: it looks for the GPU's copy by the core running it, and the menu's thread is no
    // core. This reads what the guest would, and a page on disk comes back as it does for the guest.
    const u8* const host = process->GetMemory().GetPointerSilent(va);
    if (host == nullptr) return std::nullopt;
    u64 bits = 0;
    std::memcpy(&bits, host, ValueWidth(kind));
    return bits;
}

bool WriteGuestValue(Core::System& system, u64 va, u64 bits, ValueKind kind) {
    Kernel::KProcess* const process = system.ApplicationProcess();
    if (process == nullptr) return false;
    auto& memory = process->GetMemory();
    if (!memory.IsValidVirtualAddress(va)) return false;
    // As the cheat engine does: it tells the GPU's caches, and any code compiled from what it changed.
    if (!memory.WriteBlock(va, &bits, ValueWidth(kind))) return false;
    Core::InvalidateInstructionCacheRange(process, va, ValueWidth(kind));
    return true;
}

} // namespace Jadyx::MemoryEdit
