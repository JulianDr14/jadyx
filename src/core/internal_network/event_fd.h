// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The guest's eventfd: a 64-bit counter that write() adds to and read() takes (all of it, or one
// in semaphore mode). Online libraries use it to wake their polling thread, so it must work with
// poll() next to real sockets. The host has no eventfd, so the counter rides on a UDP socket
// connected to itself on the loopback: every write is one datagram carrying the value, a read
// drains them and adds them up, and the host's poll sees it readable while one is queued.

#include "core/internal_network/sockets.h"

namespace Network {

class EventFdSocket final : public Socket {
public:
    /// Opens the loopback socket and queues `initval` (when non-zero).
    Errno Open(u64 initval, bool semaphore);

    /// Takes the counter: 8 bytes, native endian. AGAIN while it is zero and non-blocking;
    /// blocks until a write otherwise.
    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;
    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) override;
    /// Adds 8 bytes (native endian) to the counter.
    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;
    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                 const SockAddrIn* addr) override;
    Errno SetNonBlock(bool enable) override;

private:
    std::pair<s32, Errno> Push(u64 value);

    bool semaphore = false;
    bool non_blocking = false;
};

} // namespace Network
