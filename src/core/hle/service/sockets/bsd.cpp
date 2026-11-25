// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/memory.h"
#include "core/internal_network/event_fd.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"
#include <common/settings.h>

namespace Service::Sockets {

// Per-socket datagram counters. Online games end a session when a peer goes quiet; these lines
// show whether packets stopped arriving, stopped leaving, or failed on the host.
struct UdpTraffic {
    using Clock = std::chrono::steady_clock;

    struct Peer {
        u64 sent = 0;
        u64 received = 0;
        Clock::time_point last_received{};
        bool quiet_reported = false;
    };

    static constexpr auto QuietAfter = std::chrono::seconds(2);
    static constexpr auto ReportEvery = std::chrono::seconds(10);
    static constexpr size_t MaxPeers = 32;
    static constexpr u64 ErrorsLoggedInFull = 5;

    explicit UdpTraffic(s32 fd_) : fd{fd_}, opened{Clock::now()}, last_report{opened} {}

    void OnSend(const Network::SockAddrIn* to, s32 ret, Errno err) {
        const auto now = Clock::now();
        std::scoped_lock lk{mutex};
        if (ret < 0) {
            if (err != Errno::AGAIN && ++send_errors <= ErrorsLoggedInFull) {
                LOG_WARNING(Service, "UDP fd={} send to {} failed: errno {}", fd, Name(to),
                            static_cast<u32>(err));
            }
        } else {
            ++sent;
            sent_bytes += static_cast<u64>(ret);
            if (Peer* peer = Find(to)) {
                ++peer->sent;
            }
        }
        // The game keeps sending while it waits, so quiet peers are found here.
        for (auto& [key, peer] : peers) {
            if (peer.received == 0 || peer.quiet_reported ||
                now - peer.last_received < QuietAfter) {
                continue;
            }
            peer.quiet_reported = true;
            LOG_WARNING(Service, "UDP fd={} peer {} quiet for {:.1f} s, {} packets sent to it",
                        fd, Name(key), Seconds(now - peer.last_received), peer.sent);
        }
        Report(now);
    }

    void OnReceive(const Network::SockAddrIn* from, s32 ret, Errno err) {
        const auto now = Clock::now();
        std::scoped_lock lk{mutex};
        if (ret < 0) {
            // EAGAIN is a non-blocking poll with nothing queued.
            if (err != Errno::AGAIN && ++receive_errors <= ErrorsLoggedInFull) {
                LOG_WARNING(Service, "UDP fd={} receive failed: errno {}", fd,
                            static_cast<u32>(err));
            }
            return;
        }
        ++received;
        received_bytes += static_cast<u64>(ret);
        if (Peer* peer = Find(from)) {
            if (peer->quiet_reported) {
                LOG_INFO(Service, "UDP fd={} peer {} back after {:.1f} s", fd, Name(from),
                         Seconds(now - peer->last_received));
                peer->quiet_reported = false;
            }
            ++peer->received;
            peer->last_received = now;
        }
        Report(now);
    }

    void OnClose() {
        std::scoped_lock lk{mutex};
        LOG_INFO(Service,
                 "UDP fd={} closed after {:.1f} s: sent {} packets ({} B), received {} ({} B), "
                 "errors send {} receive {}",
                 fd, Seconds(Clock::now() - opened), sent, sent_bytes, received, received_bytes,
                 send_errors, receive_errors);
        for (const auto& [key, peer] : peers) {
            LOG_INFO(Service, "UDP fd={}   peer {}: sent {}, received {}", fd, Name(key),
                     peer.sent, peer.received);
        }
    }

private:
    static u64 Key(const Network::SockAddrIn* addr) {
        if (addr == nullptr) {
            return 0; // connected socket: one implicit peer
        }
        u64 ip = 0;
        for (const u8 b : addr->ip) {
            ip = (ip << 8) | b;
        }
        return (ip << 16) | addr->portno;
    }

    static std::string Name(u64 key) {
        if (key == 0) {
            return "(connected)";
        }
        return fmt::format("{}.{}.{}.{}:{}", (key >> 40) & 0xff, (key >> 32) & 0xff,
                           (key >> 24) & 0xff, (key >> 16) & 0xff, key & 0xffff);
    }

    static std::string Name(const Network::SockAddrIn* addr) {
        return Name(Key(addr));
    }

    static double Seconds(Clock::duration d) {
        return std::chrono::duration<double>(d).count();
    }

    Peer* Find(const Network::SockAddrIn* addr) {
        const u64 key = Key(addr);
        if (const auto it = peers.find(key); it != peers.end()) {
            return &it->second;
        }
        if (peers.size() >= MaxPeers) {
            return nullptr;
        }
        return &peers[key];
    }

    void Report(Clock::time_point now) {
        if (now - last_report < ReportEvery) {
            return;
        }
        LOG_INFO(Service, "UDP fd={} last {:.0f} s: sent {} packets, received {}, {} peers", fd,
                 Seconds(now - last_report), sent - sent_at_report, received - received_at_report,
                 peers.size());
        last_report = now;
        sent_at_report = sent;
        received_at_report = received;
    }

    s32 fd;
    std::mutex mutex;
    std::map<u64, Peer> peers;
    Clock::time_point opened;
    Clock::time_point last_report;
    u64 sent = 0;
    u64 sent_bytes = 0;
    u64 received = 0;
    u64 received_bytes = 0;
    u64 sent_at_report = 0;
    u64 received_at_report = 0;
    u64 send_errors = 0;
    u64 receive_errors = 0;
};

namespace {

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

// Size of a real BSD sockaddr_in. The guest SockAddrIn struct is padded to 0x100 so that longer
// input addresses can be read, but an address handed back to the game is always this long.
// Reporting 0x100 lets a game that trusts the length copy 240 bytes past its 16-byte sockaddr.
constexpr size_t GuestSockAddrInSize = 16;

// IPPROTO_TCP option number of TCP_NODELAY.
constexpr u32 TcpNoDelay = 1;

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), (std::min)(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, (std::min)(sizeof(T), buffer.size()));
}

// One message of the buffer SendMMsg/RecvMMsg exchange (7.0.0+). After a leading 0x08 byte each
// message is: u32 namelen, name, s32 iovlen, iovlen x (u64 len, data), u32 controllen, control,
// s32 flags, s32 msg_len. The offsets point into that buffer.
struct MMsgIov {
    size_t data_offset;
    size_t length;
};

struct MMsgEntry {
    size_t namelen_offset;
    size_t name_offset;
    size_t namelen;
    std::vector<MMsgIov> iovs;
    size_t msg_len_offset;
};

std::optional<std::vector<MMsgEntry>> ParseMMsgBuffer(std::span<const u8> buffer, u32 vlen) {
    std::vector<MMsgEntry> entries;
    size_t pos = 1;
    const auto take = [&](size_t size) -> std::optional<size_t> {
        if (size > buffer.size() || pos > buffer.size() - size) {
            return std::nullopt;
        }
        const size_t at = pos;
        pos += size;
        return at;
    };
    for (u32 i = 0; i < vlen; ++i) {
        MMsgEntry entry{};
        const auto namelen_at = take(sizeof(u32));
        if (!namelen_at) {
            return std::nullopt;
        }
        entry.namelen_offset = *namelen_at;
        entry.namelen = GetValue<u32>(buffer.subspan(*namelen_at));
        const auto name_at = take(entry.namelen);
        const auto iovlen_at = name_at ? take(sizeof(s32)) : std::nullopt;
        if (!iovlen_at) {
            return std::nullopt;
        }
        entry.name_offset = *name_at;
        const s32 iovlen = GetValue<s32>(buffer.subspan(*iovlen_at));
        if (iovlen < 0 || iovlen > 1024) {
            return std::nullopt;
        }
        for (s32 v = 0; v < iovlen; ++v) {
            const auto len_at = take(sizeof(u64));
            if (!len_at) {
                return std::nullopt;
            }
            const u64 length = GetValue<u64>(buffer.subspan(*len_at));
            const auto data_at = length <= buffer.size() ? take(static_cast<size_t>(length))
                                                         : std::nullopt;
            if (!data_at) {
                return std::nullopt;
            }
            entry.iovs.push_back({*data_at, static_cast<size_t>(length)});
        }
        const auto controllen_at = take(sizeof(u32));
        if (!controllen_at) {
            return std::nullopt;
        }
        const u32 controllen = GetValue<u32>(buffer.subspan(*controllen_at));
        const auto control_at = take(controllen);
        const auto flags_at = control_at ? take(sizeof(s32)) : std::nullopt;
        const auto msg_len_at = flags_at ? take(sizeof(s32)) : std::nullopt;
        if (!msg_len_at) {
            return std::nullopt;
        }
        entry.msg_len_offset = *msg_len_at;
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // Anonymous namespace

void BSD_USA::PollWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
}

void BSD_USA::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::AcceptWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD_USA::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::ConnectWork::Execute(BSD_USA* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD_USA::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD_USA::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvFromWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD_USA::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD_USA::SendWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD_USA::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SendToWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD_USA::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::PulseCapture(Pulse::HandlerCapture&) const {
    const auto open = std::count_if(file_descriptors.begin(), file_descriptors.end(),
                                    [](const auto& fd) { return fd.has_value(); });
    if (open > 0) {
        throw Pulse::ServiceRejected(true, fmt::format("{} sockets are open", open));
    }
}

void BSD_USA::RegisterClient(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD_USA::StartMonitoring(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};

    rb.Push(ResultSuccess);
}

void BSD_USA::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));
    if (bsd_errno == Errno::SUCCESS) {
        bsd_errno = ShutdownImpl(fd, 0);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Select(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD_USA::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = ctx.ReadBuffer(),
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD_USA::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD_USA::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} len={:#x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD_USA::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD_USA::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 cmd = rp.Pop<s32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    BuildErrnoResponse(ctx, SetSockOptImpl(fd, level, optname, optval));
}

void BSD_USA::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD_USA::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD_USA::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD_USA::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();

    const auto respond = [&ctx](s32 ret, Errno bsd_errno) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
    };

    if (ctx.BufferDescriptorB().empty() || vlen == 0 || vlen > 0x20) {
        LOG_WARNING(Service, "SendMMsg fd={} vlen={} has no usable buffer", fd, vlen);
        respond(-1, Errno::INVAL);
        return;
    }
    const auto& descriptor = ctx.BufferDescriptorB()[0];
    std::vector<u8> buffer(descriptor.Size());
    ctx.GetMemory().ReadBlock(descriptor.Address(), buffer.data(), buffer.size());

    const auto entries = ParseMMsgBuffer(buffer, vlen);
    if (!entries) {
        LOG_WARNING(Service, "SendMMsg fd={} vlen={} buffer of {} bytes does not parse", fd, vlen,
                    buffer.size());
        respond(-1, Errno::FAULT);
        return;
    }

    s32 sent_messages = 0;
    Errno first_errno = Errno::SUCCESS;
    size_t sent_bytes = 0;
    for (const MMsgEntry& entry : *entries) {
        std::vector<u8> message;
        for (const MMsgIov& iov : entry.iovs) {
            message.insert(message.end(), buffer.begin() + iov.data_offset,
                           buffer.begin() + iov.data_offset + iov.length);
        }
        const std::span<const u8> name{buffer.data() + entry.name_offset, entry.namelen};
        const auto [ret, bsd_errno] = entry.namelen != 0
                                          ? SendToImpl(fd, flags, message, name)
                                          : SendImpl(fd, flags, message);
        if (ret < 0) {
            first_errno = bsd_errno;
            break;
        }
        PutValue(std::span<u8>{buffer}.subspan(entry.msg_len_offset), ret);
        ++sent_messages;
        sent_bytes += static_cast<size_t>(ret);
        if (static_cast<size_t>(ret) < message.size()) {
            break;
        }
    }

    LOG_INFO(Service, "SendMMsg fd={} vlen={} sent {} messages, {} bytes, errno={}", fd, vlen,
             sent_messages, sent_bytes, static_cast<u32>(first_errno));

    if (sent_messages == 0) {
        respond(-1, first_errno);
        return;
    }
    ctx.WriteBuffer(buffer);
    respond(sent_messages, Errno::SUCCESS);
}

void BSD_USA::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();
    rp.Skip(1, false);
    const s64 timeout_sec = rp.Pop<s64>();

    const auto respond = [&ctx](s32 ret, Errno bsd_errno) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
    };

    if (ctx.BufferDescriptorB().empty() || vlen == 0 || vlen > 0x20) {
        LOG_WARNING(Service, "RecvMMsg fd={} vlen={} has no usable buffer", fd, vlen);
        respond(-1, Errno::INVAL);
        return;
    }
    const auto& descriptor = ctx.BufferDescriptorB()[0];
    std::vector<u8> buffer(descriptor.Size());
    ctx.GetMemory().ReadBlock(descriptor.Address(), buffer.data(), buffer.size());

    const auto entries = ParseMMsgBuffer(buffer, vlen);
    if (!entries) {
        LOG_WARNING(Service, "RecvMMsg fd={} vlen={} buffer of {} bytes does not parse", fd, vlen,
                    buffer.size());
        respond(-1, Errno::FAULT);
        return;
    }

    // Receive one message, which recvmmsg allows; the guest calls again for more.
    const MMsgEntry& entry = entries->front();
    size_t capacity = 0;
    for (const MMsgIov& iov : entry.iovs) {
        capacity += iov.length;
    }
    std::vector<u8> message(capacity);
    s32 ret;
    Errno bsd_errno;
    if (entry.namelen != 0) {
        std::vector<u8> addr(entry.namelen);
        std::tie(ret, bsd_errno) = RecvFromImpl(fd, flags, message, addr);
        if (ret >= 0) {
            const size_t name_size = (std::min)(addr.size(), entry.namelen);
            std::fill_n(buffer.begin() + entry.name_offset, entry.namelen, u8{0});
            std::copy_n(addr.begin(), name_size, buffer.begin() + entry.name_offset);
            PutValue(std::span<u8>{buffer}.subspan(entry.namelen_offset),
                     static_cast<u32>(name_size));
        }
    } else {
        std::tie(ret, bsd_errno) = RecvImpl(fd, flags, message);
    }

    LOG_INFO(Service, "RecvMMsg fd={} vlen={} timeout={}s got {} bytes, errno={}", fd, vlen,
             timeout_sec, ret, static_cast<u32>(bsd_errno));

    if (ret < 0) {
        respond(-1, bsd_errno);
        return;
    }

    size_t copied = 0;
    for (const MMsgIov& iov : entry.iovs) {
        const size_t chunk = (std::min)(iov.length, static_cast<size_t>(ret) - copied);
        std::copy_n(message.begin() + copied, chunk, buffer.begin() + iov.data_offset);
        copied += chunk;
    }
    PutValue(std::span<u8>{buffer}.subspan(entry.msg_len_offset), ret);
    ctx.WriteBuffer(buffer);
    respond(1, Errno::SUCCESS);
}

void BSD_USA::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    // read() on a socket is recv() without flags (an eventfd takes its counter this way).
    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = 0,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

/// @brief Only bsd:s is able to dup()
void BSD_USA::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    if (is_user) {
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = Errno::INVAL,
        });
        return;
    }

    auto const res_v = DuplicateSocketImpl(input.fd);
    if (auto* res = std::get_if<s32>(&res_v)) {
        rb.PushRaw(OutputParameters{
            .ret = *res,
            .bsd_errno = Errno::SUCCESS,
        });
    } else {
        auto* err = std::get_if<Errno>(&res_v);
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = *err,
        });
    }
}

void BSD_USA::EventFd(HLERequestContext& ctx) {
    // The guest sends the flags in the first word and the initial value after them: the online
    // library's eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) arrives as {6, 0}. Read the other way
    // round it became a blocking eventfd that already held 6.
    IPC::RequestParser rp{ctx};
    const u64 word0 = rp.Pop<u64>();
    const u32 word1 = rp.Pop<u32>();
    const u32 flags = static_cast<u32>(word0);
    const u64 initval = word1;

    // Horizon's eventfd flags follow FreeBSD: EFD_SEMAPHORE 1, EFD_NONBLOCK is O_NONBLOCK.
    constexpr u32 Semaphore = 0x1;
    constexpr u32 NonBlock = 0x4;
    const bool non_blocking = (flags & (NonBlock | Network::FLAG_O_NONBLOCK)) != 0;

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available for an eventfd");
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::MFILE);
        return;
    }
    auto socket = std::make_shared<Network::EventFdSocket>();
    Network::Errno host_err = socket->Open(initval, (flags & Semaphore) != 0);
    if (host_err == Network::Errno::SUCCESS && non_blocking) host_err = socket->SetNonBlock(true);
    if (host_err != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "eventfd could not open its loopback socket");
        rb.Push<s32>(-1);
        rb.PushEnum(Translate(host_err));
        return;
    }
    LOG_INFO(Service, "New eventfd fd={} initval={} flags={:#x} (raw {:#x} {:#x})", fd, initval,
             flags, word0, word1);
    {
        std::scoped_lock lk{file_descriptors_mutex};
        file_descriptors[fd] = FileDescriptor{
            .socket = std::move(socket),
            .flags = non_blocking ? static_cast<s32>(Network::FLAG_O_NONBLOCK) : 0,
            .is_connection_based = false,
        };
    }
    rb.Push<s32>(fd);
    rb.PushEnum(Errno::SUCCESS);
}

template <typename Work>
void BSD_USA::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD_USA::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // The type carries the BSD creation flags: SOCK_NONBLOCK (0x20000000) and SOCK_CLOEXEC
    // (0x10000000, meaningless here). Split them off before the type is checked.
    constexpr u32 SOCK_FLAG_CLOEXEC = 0x10000000;
    constexpr u32 SOCK_FLAG_NONBLOCK = 0x20000000;
    const bool non_blocking = (static_cast<u32>(type) & SOCK_FLAG_NONBLOCK) != 0;
    type = static_cast<Type>(static_cast<u32>(type) & ~(SOCK_FLAG_CLOEXEC | SOCK_FLAG_NONBLOCK));

    // user bsd:u has restrictions on SOCK_SEQPACKET and SOCK_RAW
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW)) {
        if (type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP) {
            // fine, can use on bsd:s and bsd:u
        } else {
            return {-1, Errno::INVAL};
        }
    }

    // Only IPv4 sockets exist on the host side. Refuse other families up front: a socket that
    // looks created but has no host socket behind it makes dual-stack clients (gRPC probes
    // AF_INET6 first) take the IPv6 path and fail every later call. With EAFNOSUPPORT they fall
    // back to AF_INET.
    if (domain != Domain::INET && domain != Domain::Unspecified) {
        LOG_INFO(Service, "socket family {} is not supported, answering EAFNOSUPPORT",
                 static_cast<u32>(domain));
        return {-1, Errno::AFNOSUPPORT};
    }

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    // Built whole before it is published: the room's network thread walks the descriptors and
    // must never see one without its socket.
    FileDescriptor descriptor{};
    // ENONMEM might be thrown here

    LOG_INFO(Service, "New socket fd={}", fd);

    auto room_member = Network::GetRoomMember().lock();
    if (room_member && room_member->IsConnected()) {
        descriptor.socket = std::make_shared<Network::ProxySocket>();
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
    }

    descriptor.socket->Initialize(Translate(domain), Translate(type), Translate(protocol));
    if (non_blocking) {
        const Network::Errno nb_err = descriptor.socket->SetNonBlock(true);
        if (nb_err == Network::Errno::SUCCESS) {
            descriptor.flags = static_cast<s32>(Network::FLAG_O_NONBLOCK);
        } else {
            LOG_WARNING(Service, "socket fd={} could not be made non-blocking at creation", fd);
        }
    }
    descriptor.is_connection_based = IsConnectionBased(type);
    const bool is_connection_based = descriptor.is_connection_based;
    if (type == Type::DGRAM) {
        descriptor.udp = std::make_shared<UdpTraffic>(fd);
    }
    {
        std::scoped_lock lk{file_descriptors_mutex};
        file_descriptors[fd] = std::move(descriptor);
    }

    if (Settings::values.airplane_mode.GetValue() && is_connection_based) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }

    return {fd, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD_USA::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // When no entries are provided, -1 is returned with errno zero
        return {-1, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    for (PollFD& pollfd : fds) {
        // revents is output only; games can pass it dirty (reused pollfd arrays).
        pollfd.revents = PollEvents{};

        if (pollfd.fd > static_cast<s32>(MAX_FD) || pollfd.fd < 0) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            pollfd.revents = PollEvents{};
            return {0, Errno::SUCCESS};
        }

        const std::optional<FileDescriptor>& descriptor = file_descriptors[pollfd.fd];
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            pollfd.revents = PollEvents::Nval;
            return {0, Errno::SUCCESS};
        }
    }

    std::vector<Network::PollFD> host_pollfds(fds.size());
    std::transform(fds.begin(), fds.end(), host_pollfds.begin(), [](PollFD pollfd) {
        Network::PollFD result;
        result.socket = file_descriptors[pollfd.fd]->socket.get();
        result.events = Translate(pollfd.events);
        result.revents = Network::PollEvents{};
        return result;
    });

    const auto result = Network::Poll(host_pollfds, timeout);

    const size_t num = host_pollfds.size();
    for (size_t i = 0; i < num; ++i) {
        fds[i].revents = Translate(host_pollfds[i].revents);
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    return Translate(result);
}

std::pair<s32, Errno> BSD_USA::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    auto [result, bsd_errno] = descriptor.socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    {
        std::scoped_lock lk{file_descriptors_mutex};
        file_descriptors[new_fd] = FileDescriptor{
            .socket = std::move(result.socket),
            .is_connection_based = descriptor.is_connection_based,
        };
    }

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    if (write_buffer.size() > GuestSockAddrInSize) {
        write_buffer.resize(GuestSockAddrInSize);
    }
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD_USA::BindImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    ASSERT(addr.size() >= 16);
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    return Translate(file_descriptors[fd]->socket->Bind(Translate(addr_in)));
}

Errno BSD_USA::ConnectImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    ASSERT(addr.size() >= 16);
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    const Errno result = Translate(file_descriptors[fd]->socket->Connect(Translate(addr_in)));

    if (result == Errno::ISCONN) {
        LOG_DEBUG(Service, "returned ISCONN - socket already connected");
        return Errno::SUCCESS;
    }

    return result;
}

Errno BSD_USA::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    // The reported length must be that of a real sockaddr_in, not of the padded guest struct.
    write_buffer.resize(GuestSockAddrInSize);
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    // The reported length must be that of a real sockaddr_in, not of the padded guest struct.
    write_buffer.resize(GuestSockAddrInSize);
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::ListenImpl(s32 fd, s32 backlog) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    return Translate(file_descriptors[fd]->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD_USA::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        return {descriptor.flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor.socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        descriptor.flags = arg;
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD_USA::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    // gRPC sets TCP_NODELAY and reads it back; a zero answer makes it drop the socket before
    // connecting.
    if (level == static_cast<u32>(SocketLevel::TCP) && static_cast<u32>(optname) == TcpNoDelay) {
        const u32 value = file_descriptors[fd]->tcp_nodelay ? 1 : 0;
        optval.resize(sizeof(u32));
        PutValue(optval, value);
        return Errno::SUCCESS;
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        UNIMPLEMENTED_MSG("Unknown getsockopt level");
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    switch (optname) {
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Errno), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);
        }
        return Translate(getsockopt_err);
    }
    default: {
        // Clients that verify an option after setting it close the socket on a zero answer, so
        // echo what the guest set.
        const auto& options = file_descriptors[fd]->socket_options;
        const auto it = options.find(static_cast<u32>(optname));
        if (it != options.end()) {
            optval.resize(sizeof(u32));
            PutValue(optval, it->second);
            return Errno::SUCCESS;
        }
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
    }
}

Errno BSD_USA::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    if (level == static_cast<u32>(SocketLevel::TCP) && static_cast<u32>(optname) == TcpNoDelay &&
        optval.size() >= sizeof(u32)) {
        file_descriptors[fd]->tcp_nodelay = GetValue<u32>(optval) != 0;
        return Errno::SUCCESS;
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) setsockopt with level={}, optname={}", level, optname);
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    if (optname == OptName::LINGER) {
        // Some titles pass only the on/off word; a missing timeout reads as zero.
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        const auto linger = GetValue<Linger>(optval);

        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    if (optval.size() != sizeof(u32)) {
        LOG_WARNING(Service, "(STUBBED) setsockopt optname={:#x} with a {}-byte value",
                    static_cast<u32>(optname), optval.size());
        return Errno::SUCCESS;
    }
    auto value = GetValue<u32>(optval);
    file_descriptors[fd]->socket_options[static_cast<u32>(optname)] = value;

    switch (optname) {
    case OptName::REUSEADDR:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::BROADCAST:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetBroadcast(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(value));
    case OptName::RCVTIMEO:
        return Translate(socket->SetRcvTimeo(value));
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default:
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
}

Errno BSD_USA::ShutdownImpl(s32 fd, s32 how) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(file_descriptors[fd]->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD_USA::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->Recv(flags, message));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    if (descriptor.udp) {
        descriptor.udp->OnReceive(nullptr, ret, bsd_errno);
    }
    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor.is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        p_addr_in = &addr_in;
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    if (descriptor.udp) {
        descriptor.udp->OnReceive(p_addr_in, ret, bsd_errno);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            ASSERT(addr.size() >= GuestSockAddrInSize);
            const SockAddrIn result = Translate(addr_in);
            addr.resize(GuestSockAddrInSize);
            PutValue(addr, result);
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }
    const auto [ret, bsd_errno] = Translate(file_descriptors[fd]->socket->Send(message, flags));
    if (const auto& udp = file_descriptors[fd]->udp) {
        udp->OnSend(nullptr, ret, bsd_errno);
    }
    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() >= 16);
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        p_addr_in = &addr_in;
    }

    const auto [ret, bsd_errno] =
        Translate(file_descriptors[fd]->socket->SendTo(flags, message, p_addr_in));
    if (const auto& udp = file_descriptors[fd]->udp) {
        udp->OnSend(p_addr_in, ret, bsd_errno);
    }
    return {ret, bsd_errno};
}

Errno BSD_USA::CloseImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const Errno bsd_errno = Translate(file_descriptors[fd]->socket->Close());
    if (bsd_errno != Errno::SUCCESS) {
        return bsd_errno;
    }

    LOG_INFO(Service, "Close socket fd={}", fd);
    if (file_descriptors[fd]->udp) {
        file_descriptors[fd]->udp->OnClose();
    }

    std::scoped_lock lk{file_descriptors_mutex};
    file_descriptors[fd].reset();
    return bsd_errno;
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Errno::MFILE;
    }

    std::scoped_lock lk{file_descriptors_mutex};
    file_descriptors[new_fd] = FileDescriptor{
        .socket = file_descriptors[fd]->socket,
        .flags = file_descriptors[fd]->flags,
        .is_connection_based = file_descriptors[fd]->is_connection_based,
        .udp = file_descriptors[fd]->udp,
    };
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return std::nullopt;
    }
    return file_descriptors[fd]->socket;
}

s32 BSD_USA::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

bool BSD_USA::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd > static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD_USA::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Runs on the room's network thread while the guest opens and closes sockets: take the
    // sockets under the lock and keep them alive while the packet is handed over.
    std::vector<std::shared_ptr<Network::SocketBase>> sockets;
    {
        std::scoped_lock lk{file_descriptors_mutex};
        for (const auto& optional_descriptor : file_descriptors) {
            if (optional_descriptor.has_value() && optional_descriptor->socket) {
                sockets.push_back(optional_descriptor->socket);
            }
        }
    }
    for (const auto& socket : sockets) {
        socket->HandleProxyPacket(packet);
    }
}

BSD_USA::BSD_USA(Core::System& system_, const char* name, bool is_user_)
    : ServiceFramework{system_, name}
    , is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD_USA::RegisterClient, "RegisterClient"},
        {1, &BSD_USA::StartMonitoring, "StartMonitoring"},
        {2, &BSD_USA::Socket, "Socket"},
        {3, &BSD_USA::SocketExempt, "SocketExempt"},
        {4, nullptr, "Open"},
        {5, &BSD_USA::Select, "Select"},
        {6, &BSD_USA::Poll, "Poll"},
        {7, nullptr, "Sysctl"},
        {8, &BSD_USA::Recv, "Recv"},
        {9, &BSD_USA::RecvFrom, "RecvFrom"},
        {10, &BSD_USA::Send, "Send"},
        {11, &BSD_USA::SendTo, "SendTo"},
        {12, &BSD_USA::Accept, "Accept"},
        {13, &BSD_USA::Bind, "Bind"},
        {14, &BSD_USA::Connect, "Connect"},
        {15, &BSD_USA::GetPeerName, "GetPeerName"},
        {16, &BSD_USA::GetSockName, "GetSockName"},
        {17, &BSD_USA::GetSockOpt, "GetSockOpt"},
        {18, &BSD_USA::Listen, "Listen"},
        {19, nullptr, "Ioctl"},
        {20, &BSD_USA::Fcntl, "Fcntl"},
        {21, &BSD_USA::SetSockOpt, "SetSockOpt"},
        {22, &BSD_USA::Shutdown, "Shutdown"},
        {23, nullptr, "ShutdownAllSockets"},
        {24, &BSD_USA::Write, "Write"},
        {25, &BSD_USA::Read, "Read"},
        {26, &BSD_USA::Close, "Close"},
        {27, &BSD_USA::DuplicateSocket, "DuplicateSocket"},
        {28, nullptr, "GetResourceStatistics"},
        {29, &BSD_USA::RecvMMsg, "RecvMMsg"}, //3.0.0+
        {30, &BSD_USA::SendMMsg, "SendMMsg"}, //3.0.0+
        {31, &BSD_USA::EventFd, "EventFd"}, //7.0.0+
        {32, nullptr, "RegisterResourceStatisticsName"}, //7.0.0+
        {33, nullptr, "RegisterClientShared"}, //10.0.0+
        {34, nullptr, "GetSocketStatistics"}, //15.0.0+
        {35, nullptr, "NifIoctl"}, //17.0.0+
        {36, nullptr, "Unknown36"}, //18.0.0+
        {37, nullptr, "Unknown37"}, //18.0.0+
        {38, nullptr, "Unknown38"}, //18.0.0+
        {39, nullptr, "Unknown39"}, //20.0.0+
        {40, nullptr, "Unknown40"}, //20.0.0+
        {41, nullptr, "Unknown41"}, //21.0.0+
        {42, nullptr, "Unknown42"}, //21.0.0+
        {43, nullptr, "Unknown43"}, //21.0.0+
        {200, nullptr, "SetThreadCoreMask"}, //15.0.0+
        {201, nullptr, "GetThreadCoreMask"}, //15.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    if (auto room_member = Network::GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD_USA::~BSD_USA() {
    if (auto room_member = Network::GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
}

std::unique_lock<std::mutex> BSD_USA::LockService() noexcept {
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char *name)
    : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetIfUp"},
        {1, nullptr, "SetIfUpWithEvent"},
        {2, nullptr, "CancelIf"},
        {3, nullptr, "SetIfDown"},
        {4, nullptr, "GetIfState"},
        {5, nullptr, "DhcpRenew"},
        {6, nullptr, "AddStaticArpEntry"},
        {7, nullptr, "RemoveArpEntry"},
        {8, nullptr, "LookupArpEntry"},
        {9, nullptr, "LookupArpEntry2"},
        {10, nullptr, "ClearArpEntries"},
        {11, nullptr, "ClearArpEntries2"},
        {12, nullptr, "PrintArpEntries"},
        {13, nullptr, "Unknown13"},
        {14, nullptr, "Unknown14"},
        {15, nullptr, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

BSD_NU::BSD_NU(Core::System& system_)
    : ServiceFramework{system_, "bsd:nu"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateUserService"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

BSD_NU::~BSD_NU() = default;

} // namespace Service::Sockets
