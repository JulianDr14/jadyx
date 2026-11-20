// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/internal_network/event_fd.h"

#include <array>
#include <cstring>
#include <limits>

#include "common/logging.h"

namespace Network {
namespace {

constexpr u64 MaxCount = (std::numeric_limits<u64>::max)() - 1;

} // namespace

Errno EventFdSocket::Open(u64 initval, bool semaphore_) {
    semaphore = semaphore_;
    if (const Errno err = Initialize(Domain::INET, Type::DGRAM, Protocol::UDP); err != Errno::SUCCESS)
        return err;
    // Bound to the loopback on any port, then connected to that same address: only its own
    // datagrams reach it.
    if (const Errno err = Socket::Bind(SockAddrIn{Domain::INET, {127, 0, 0, 1}, 0});
        err != Errno::SUCCESS) {
        Close();
        return err;
    }
    const auto [self, name_err] = GetSockName();
    if (name_err != Errno::SUCCESS) {
        Close();
        return name_err;
    }
    if (const Errno err = Socket::Connect(self); err != Errno::SUCCESS) {
        Close();
        return err;
    }
    if (const Errno err = Socket::SetNonBlock(false); err != Errno::SUCCESS) {
        Close();
        return err;
    }
    if (initval != 0) {
        if (const auto [ret, err] = Push(initval); ret < 0) {
            Close();
            return err;
        }
    }
    return Errno::SUCCESS;
}

std::pair<s32, Errno> EventFdSocket::Push(u64 value) {
    std::array<u8, sizeof(u64)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(value));
    const auto [ret, err] = Socket::Send(bytes, 0);
    if (ret != static_cast<s32>(sizeof(value))) return {-1, ret < 0 ? err : Errno::OTHER};
    return {static_cast<s32>(sizeof(value)), Errno::SUCCESS};
}

std::pair<s32, Errno> EventFdSocket::Recv(int flags, std::span<u8> message) {
    if (flags != 0) LOG_DEBUG(Network, "eventfd read with flags {:#x} (ignored)", flags);
    if (message.size() < sizeof(u64)) return {-1, Errno::INVAL};

    // The first datagram: waits for it unless non-blocking (AGAIN then, like an empty eventfd).
    std::array<u8, sizeof(u64)> bytes{};
    auto [ret, err] = Socket::Recv(0, bytes);
    if (ret < 0) return {-1, err};
    u64 total = 0;
    std::memcpy(&total, bytes.data(), sizeof(total));

    if (semaphore) {
        // One unit per read; the rest goes back in the queue.
        if (total > 1) Push(total - 1);
        total = 1;
    } else {
        // The rest that is already queued, without waiting.
        Socket::SetNonBlock(true);
        while (true) {
            const auto [more, more_err] = Socket::Recv(0, bytes);
            if (more != static_cast<s32>(sizeof(u64))) break;
            u64 value = 0;
            std::memcpy(&value, bytes.data(), sizeof(value));
            total = value > MaxCount - total ? MaxCount : total + value;
        }
        Socket::SetNonBlock(non_blocking);
    }
    std::memcpy(message.data(), &total, sizeof(total));
    return {static_cast<s32>(sizeof(total)), Errno::SUCCESS};
}

std::pair<s32, Errno> EventFdSocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    if (addr) *addr = SockAddrIn{};
    return Recv(flags, message);
}

std::pair<s32, Errno> EventFdSocket::Send(std::span<const u8> message, int flags) {
    if (flags != 0) LOG_DEBUG(Network, "eventfd write with flags {:#x} (ignored)", flags);
    if (message.size() < sizeof(u64)) return {-1, Errno::INVAL};
    u64 value = 0;
    std::memcpy(&value, message.data(), sizeof(value));
    if (value == (std::numeric_limits<u64>::max)()) return {-1, Errno::INVAL};
    if (value == 0) return {static_cast<s32>(sizeof(value)), Errno::SUCCESS};
    return Push(value);
}

std::pair<s32, Errno> EventFdSocket::SendTo(u32 flags, std::span<const u8> message,
                                            const SockAddrIn*) {
    return Send(message, static_cast<int>(flags));
}

Errno EventFdSocket::SetNonBlock(bool enable) {
    const Errno err = Socket::SetNonBlock(enable);
    if (err == Errno::SUCCESS) non_blocking = enable;
    return err;
}

} // namespace Network
