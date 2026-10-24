// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
constexpr u32 ResultSuccess = 0;
enum class Errno : u32 { SUCCESS, BADF, AGAIN, INTR };
namespace Network {
constexpr u32 FLAG_MSG_DONTWAIT = 0x80;
constexpr u32 FLAG_O_NONBLOCK = 0x800;
} // namespace Network
constexpr int Service = 0;
template <typename... T>
void Log(const T&...) {}
#define LOG_DEBUG(...) Log(__VA_ARGS__)
#define LOG_WARNING(...) Log(__VA_ARGS__)
struct HLERequestContext {
    s32 fd{};
    std::size_t size{};
    std::vector<u8> output;
    std::vector<u32> response;
    bool queued{};
    std::size_t GetWriteBufferSize() const {
        return size;
    }
    void WriteBuffer(const std::vector<u8>& bytes) {
        assert(bytes.size() <= size);
        output = bytes;
    }
};
namespace IPC {
struct RequestParser {
    HLERequestContext& ctx;
    template <typename T>
    T Pop() {
        return static_cast<T>(ctx.fd);
    }
};
struct ResponseBuilder {
    HLERequestContext& ctx;
    ResponseBuilder(HLERequestContext& context, unsigned) : ctx{context} {}
    template <typename T>
    void Push(T value) {
        ctx.response.push_back(static_cast<u32>(value));
    }
    void PushEnum(Errno value) {
        Push(value);
    }
};
} // namespace IPC
struct Socket {
    s32 result{4};
    Errno error{};
    unsigned calls{};
    bool nonblocking{};
    std::vector<bool> nonblock_changes;
    std::size_t requested{};
    void SetNonBlock(bool enabled) {
        nonblocking = enabled;
        nonblock_changes.push_back(enabled);
    }
    std::pair<s32, Errno> Recv(u32 flags, std::span<u8> output) {
        assert(flags == 0);
        requested = output.size();
        ++calls;
        if (result >= 0) {
            const auto count = std::min(output.size(), static_cast<std::size_t>(result));
            std::fill_n(output.begin(), count, 0x5a);
            return {static_cast<s32>(count), error};
        }
        return {result, error};
    }
};
std::pair<s32, Errno> Translate(std::pair<s32, Errno> value) {
    return value;
}
struct BSD_USA {
    struct FileDescriptor {
        u32 flags{};
        Socket* socket{};
    };
    struct RecvWork {
        void Execute(BSD_USA* bsd);
        void Response(HLERequestContext& ctx);
        s32 fd{};
        u32 flags{};
        std::vector<u8> message;
        s32 ret{};
        Errno bsd_errno{};
    };
    std::array<FileDescriptor*, 1> file_descriptors{};
    bool IsFileDescriptorValid(s32 fd) const {
        return fd == 0 && file_descriptors[0] != nullptr;
    }
    void ExecuteWork(HLERequestContext& ctx, RecvWork work) {
        ctx.queued = true;
        work.Execute(this);
        work.Response(ctx);
    }
    std::pair<s32, Errno> RecvImpl(s32 fd, u32 flags, std::vector<u8>& message);
    void Read(HLERequestContext& ctx);
};
// HANDLERS
int main() {
    Socket socket;
    BSD_USA::FileDescriptor descriptor;
    descriptor.socket = &socket;
    BSD_USA bsd;
    bsd.file_descriptors[0] = &descriptor;
    auto read = [&](s32 fd, std::size_t size, s32 expected, Errno error) {
        HLERequestContext ctx;
        ctx.fd = fd;
        ctx.size = size;
        bsd.Read(ctx);
        assert(ctx.response.size() == 3 && ctx.response[0] == ResultSuccess);
        assert(ctx.response[1] == static_cast<u32>(expected));
        assert(ctx.response[2] == static_cast<u32>(error));
        assert(ctx.queued && "Read must use the deferred receive worker");
        assert(ctx.output.size() <= size);
        return ctx;
    };
    auto bytes = read(0, 8, 4, Errno::SUCCESS);
    assert(socket.requested == 8 && socket.calls == 1);
    assert(std::all_of(bytes.output.begin(), bytes.output.begin() + 4,
                       [](u8 value) { return value == 0x5a; }));
    assert(socket.nonblock_changes == std::vector<bool>{false});
    read(0, 2, 2, Errno::SUCCESS); // Partial receive, bounded by the IPC buffer.
    read(0, 0, 0, Errno::SUCCESS);
    socket.result = 0;
    read(0, 8, 0, Errno::SUCCESS); // Actual EOF, not a fabricated one.
    const auto calls = socket.calls;
    read(-1, 8, -1, Errno::BADF);
    read(1, 8, -1, Errno::BADF);
    assert(socket.calls == calls);
    descriptor.flags = Network::FLAG_O_NONBLOCK;
    socket.nonblocking = true;
    socket.nonblock_changes.clear();
    socket.result = -1;
    socket.error = Errno::AGAIN;
    read(0, 8, -1, Errno::AGAIN);
    assert(socket.nonblocking && socket.nonblock_changes.empty());
    descriptor.flags = 0;
    socket.nonblocking = false;
    socket.error = Errno::INTR;
    read(0, 8, -1, Errno::INTR);
    assert(!socket.nonblocking && socket.nonblock_changes == std::vector<bool>{false});
}
