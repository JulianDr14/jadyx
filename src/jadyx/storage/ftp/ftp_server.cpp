// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from NXbox src/eden_uwp/ftp (GPL-3.0-or-later).
#include "jadyx/storage/ftp/ftp_protocol.h"
#include "jadyx/storage/ftp/ftp_safety.h"
#include "jadyx/storage/ftp/ftp_server.h"
#include "jadyx/storage/ftp/ftp_transfer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/logging.h"

namespace Jadyx::Ftp {
namespace {
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template <typename Socket> void CloseSocket(const Socket& socket) noexcept {
    try {
        if (socket)
            socket.Close();
    } catch (...) {
        // Closing is also the cancellation path during suspend and network loss.
    }
}

std::tm Modified(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    SYSTEMTIME time{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        !FileTimeToSystemTime(&attributes.ftLastWriteTime, &time))
        throw std::runtime_error("cannot read modification time");
    std::tm utc{};
    utc.tm_year = time.wYear - 1900;
    utc.tm_mon = time.wMonth - 1;
    utc.tm_mday = time.wDay;
    utc.tm_hour = time.wHour;
    utc.tm_min = time.wMinute;
    utc.tm_sec = time.wSecond;
    return utc;
}
std::string Name(const fs::path& path) { return winrt::to_string(path.filename().wstring()); }
void Write(const StreamSocket& socket, std::string_view text) {
    DataWriter writer(socket.OutputStream());
    writer.WriteBytes(winrt::array_view<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        reinterpret_cast<const std::uint8_t*>(text.data() + text.size())));
    writer.StoreAsync().get();
    writer.DetachStream();
}
bool Publish(const fs::path& source, const fs::path& destination, bool replace = true) {
    return replace && Common::FS::Exists(destination)
               ? ReplaceFileFromAppW(destination.c_str(), source.c_str(), nullptr, 0, nullptr,
                                     nullptr) != 0
               : MoveFileFromAppW(source.c_str(), destination.c_str()) != 0;
}
// All threads keep their owning Session alive. Closing the sockets cancels blocked WinRT I/O.
struct Session : std::enable_shared_from_this<Session> {
    StreamSocket control{nullptr};
    StreamSocket data{nullptr};
    StreamSocketListener passive{nullptr};
    Roots roots;
    std::string password;
    std::string ip;
    std::string peer;
    std::string cwd;
    std::optional<std::string> rename_from;
    std::shared_ptr<std::atomic<std::uint64_t>> changes;
    std::atomic<bool> stopped{false};
    std::atomic<bool> transferring{false};
    std::atomic<bool> done{false};
    std::mutex sockets_mutex;
    std::mutex reply_mutex;
    std::mutex finish_mutex;
    std::condition_variable ready;
    std::condition_variable finished;
    std::thread transfer;
    bool authenticated = false;
    bool username_ok = false;
    int failures = 0;
    SessionDeadline deadline;
    std::shared_ptr<LoginBackoff> login_backoff;
    std::optional<std::uint64_t> allocation;

    void Reply(std::string_view text) {
        std::lock_guard lock(reply_mutex);
        Write(control, text);
    }
    void CloseData() {
        std::lock_guard lock(sockets_mutex);
        if (passive) {
            CloseSocket(passive);
            passive = nullptr;
        }
        if (data) {
            CloseSocket(data);
            data = nullptr;
        }
        ready.notify_all();
    }
    void Stop() {
        stopped = true;
        CloseSocket(control);
        CloseData();
    }
    void Wait() {
        std::unique_lock lock(finish_mutex);
        finished.wait(lock, [&] { return done.load(); });
    }
    fs::path Resolve(const std::string& relative) const {
        const auto mapped = MapPath(roots, relative);
        if (!mapped)
            throw std::runtime_error("root unavailable");
        fs::path result = mapped->root;
        const auto& tail = mapped->relative;
        for (const auto& component : fs::path(winrt::to_hstring(tail).c_str())) {
            result /= component;
            // Reject every reparse point, including directory junctions, not just symlinks.
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExFromAppW(result.c_str(), GetFileExInfoStandard, &attributes) &&
                (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                throw std::runtime_error("reparse point");
        }
        return result;
    }
    void Changed() { ++*changes; }
    void Passive(bool extended) {
        CloseData();
        for (unsigned port = 50000; port <= 50009; ++port) {
            StreamSocketListener candidate;
            const std::weak_ptr<Session> weak = shared_from_this();
            candidate.ConnectionReceived([weak](const auto& sender, const auto& args) {
                const auto owner = weak.lock();
                if (!owner) {
                    CloseSocket(args.Socket());
                    return;
                }
                std::lock_guard lock(owner->sockets_mutex);
                const auto incoming = args.Socket();
                if (owner->stopped || owner->passive != sender || owner->data ||
                    winrt::to_string(incoming.Information().RemoteAddress().CanonicalName()) !=
                        owner->peer) {
                    CloseSocket(incoming);
                    return;
                }
                owner->data = incoming;
                owner->ready.notify_all();
            });
            try {
                // Any interface, like the control port; PASV still advertises `ip`.
                candidate.BindServiceNameAsync(winrt::to_hstring(port)).get();
            } catch (...) {
                CloseSocket(candidate);
                continue;
            }
            {
                std::lock_guard lock(sockets_mutex);
                if (stopped) {
                    CloseSocket(candidate);
                    return;
                }
                passive = candidate;
            }
            Reply(extended ? EpsvReply(port) : PasvReply(*PrivateIPv4(ip), port));
            return;
        }
        Reply("425 No passive port available.\r\n");
    }
    StreamSocket AwaitData() {
        std::unique_lock lock(sockets_mutex);
        if (!ready.wait_for(lock, 30s, [&] { return data || stopped || !passive; }) || stopped ||
            !data)
            throw std::runtime_error("data connection unavailable");
        return data;
    }
    void Transfer(std::string verb, std::string relative, std::optional<std::uint64_t> expected) {
        const bool upload = verb == "STOR" || verb == "APPE";
        const bool listing = verb == "LIST" || verb == "NLST" || verb == "MLSD";
        std::uint64_t bytes = 0;
        bool success = false;
        bool connected = false;
        fs::path partial;
        try {
            const auto target = Resolve(relative);
            if (upload) {
                partial = target;
                partial += L".part";
                // Do not destroy a pre-existing temporary file from another producer.
                if (Common::FS::Exists(partial)) {
                    partial.clear();
                    throw std::runtime_error("partial exists");
                }
                if (verb == "APPE" && Common::FS::Exists(target)) {
                    Common::FS::IOFile original(target, Common::FS::FileAccessMode::Read);
                    Common::FS::IOFile copy(partial, Common::FS::FileAccessMode::Write);
                    if (!original.IsOpen() || !copy.IsOpen())
                        throw std::runtime_error("cannot prepare append");
                    CopyStream(
                        [&](std::span<std::uint8_t> chunk) {
                            return original.ReadSpan<std::uint8_t>(chunk);
                        },
                        [&](std::span<const std::uint8_t> chunk) {
                            return copy.WriteSpan<std::uint8_t>(chunk);
                        },
                        [&] { return !stopped && transferring; }, original.GetSize());
                    if (!copy.Flush() || !copy.Commit())
                        throw std::runtime_error("append copy failed");
                    copy.Close();
                }
            }
            const auto socket = AwaitData();
            connected = true;
            if (upload) {
                Common::FS::IOFile output(partial, Common::FS::FileAccessMode::Append);
                if (!output.IsOpen())
                    throw std::runtime_error("cannot open upload");
                DataReader reader(socket.InputStream());
                reader.InputStreamOptions(InputStreamOptions::Partial);
                bytes = CopyStream(
                    [&](std::span<std::uint8_t> buffer) {
                        const auto count =
                            reader.LoadAsync(static_cast<std::uint32_t>(buffer.size())).get();
                        if (count != 0)
                            reader.ReadBytes(winrt::array_view<std::uint8_t>(
                                buffer.data(), buffer.data() + count));
                        if (count != 0)
                            deadline.Activity();
                        return static_cast<std::size_t>(count);
                    },
                    [&](std::span<const std::uint8_t> buffer) {
                        return output.WriteSpan<std::uint8_t>(buffer);
                    },
                    [&] { return !stopped && transferring; });
                reader.DetachStream();
                CompleteUpload(
                    expected, bytes, [&] { return !stopped && transferring; },
                    [&] {
                        if (!output.Flush() || !output.Commit())
                            throw std::runtime_error("upload flush failed");
                        output.Close();
                        if (stopped || !transferring || !Publish(partial, target))
                            throw std::runtime_error("upload cancelled or rename failed");
                    },
                    [&] {
                        output.Close();
                        DeleteFileFromAppW(partial.c_str());
                    });
                partial.clear();
                Changed();
            } else if (listing) {
                auto entry = [&](const fs::path& item) {
                    const auto filename = Name(item);
                    if (!NormalizePath("", filename))
                        return;
                    const auto checked =
                        Resolve(relative.empty() ? filename : relative + "/" + filename);
                    const bool directory = Common::FS::IsDir(checked);
                    const auto size = directory ? 0 : Common::FS::GetSize(checked);
                    const auto utc = Modified(checked);
                    std::string line;
                    if (verb == "NLST")
                        line = filename + "\r\n";
                    else if (verb == "MLSD")
                        line = std::string("type=") + (directory ? "dir" : "file") +
                               ";size=" + std::to_string(size) + ";modify=" + Timestamp(utc) +
                               "; " + filename + "\r\n";
                    else
                        line = ListLine(filename, directory, size, utc);
                    Write(socket, line);
                    deadline.Activity();
                    bytes += line.size();
                };
                if (relative.empty()) {
                    const auto& aliases = RootNames;
                    for (const auto& alias_name : aliases) {
                        const char* alias = alias_name.data();
                        std::string line;
                        if (verb == "NLST")
                            line = std::string(alias) + "\r\n";
                        else if (verb == "MLSD")
                            line = std::string("type=dir; ") + alias + "\r\n";
                        else {
                            std::tm date{};
                            date.tm_year = 126;
                            date.tm_mday = 1;
                            line = ListLine(alias, true, 0, date);
                        }
                        Write(socket, line);
                        bytes += line.size();
                    }
                } else {
                    for (const auto& item : fs::directory_iterator(target)) {
                        if (stopped || !transferring)
                            throw std::runtime_error("listing cancelled");
                        entry(item.path());
                    }
                }
            } else {
                Common::FS::IOFile input(target, Common::FS::FileAccessMode::Read);
                if (!input.IsOpen())
                    throw std::runtime_error("cannot open download");
                bytes = CopyStream(
                    [&](std::span<std::uint8_t> buffer) {
                        return input.ReadSpan<std::uint8_t>(buffer);
                    },
                    [&](std::span<const std::uint8_t> buffer) {
                        Write(socket, std::string_view(reinterpret_cast<const char*>(buffer.data()),
                                                       buffer.size()));
                        deadline.Activity();
                        return buffer.size();
                    },
                    [&] { return !stopped && transferring; }, input.GetSize());
            }
            success = true;
        } catch (...) {
            // A failed transfer must leave the old destination intact.
        }
        if (!partial.empty()) {
            DeleteFileFromAppW(partial.c_str());
        }
        CloseData();
        const bool aborted = !transferring;
        transferring = false;
        if (!stopped && !aborted) {
            try {
                Reply(success     ? "226 Transfer complete.\r\n"
                      : connected ? "426 Transfer failed.\r\n"
                                  : "425 Cannot open data connection.\r\n");
            } catch (...) {
                Stop();
            }
        }
        LOG_INFO(Frontend, "FTP {} bytes={} success={}", verb, bytes, success);
    }
    void Abort() {
        const bool active = transferring.exchange(false);
        CloseData();
        if (transfer.joinable())
            transfer.join();
        Reply(active ? "426 Transfer aborted.\r\n226 Abort successful.\r\n"
                     : "225 No transfer active.\r\n");
    }
    bool Handle(const Command& command) {
        const auto& verb = command.verb;
        const auto& argument = command.argument;
        if (verb == "QUIT") {
            Reply("221 Goodbye.\r\n");
            return false;
        }
        if (verb == "USER" && !transferring) {
            if (authenticated)
                deadline.Login(false);
            authenticated = false;
            allocation.reset();
            username_ok = argument == "jadyx";
            rename_from.reset();
            CloseData();
            Reply("331 Password required.\r\n");
        } else if (verb == "PASS" && !transferring) {
            if (username_ok && argument == password) {
                authenticated = true;
                deadline.Login(true);
                Reply("230 Logged in.\r\n");
                LOG_INFO(Frontend, "FTP login accepted");
            } else {
                if (authenticated)
                    deadline.Login(false);
                authenticated = false;
                allocation.reset();
                ++failures;
                std::unique_lock lock(sockets_mutex);
                const auto delay = login_backoff->Failure(peer);
                ready.wait_for(
                    lock,
                    std::min(std::chrono::steady_clock::duration(delay), deadline.Remaining()),
                    [&] { return stopped.load(); });
                lock.unlock();
                if (stopped || deadline.Expired())
                    return false;
                Reply("530 Login incorrect.\r\n");
                if (failures >= 5)
                    return false;
            }
        } else if (verb == "SYST") {
            Reply("215 UNIX Type: L8\r\n");
        } else if (verb == "FEAT") {
            Reply("211-Features\r\n UTF8\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n"
                  " MLST type*;size*;modify*;\r\n211 End\r\n");
        } else if (verb == "OPTS") {
            Reply(argument == "UTF8 ON" ? "200 UTF8 enabled.\r\n" : "501 Unsupported option.\r\n");
        } else if (!authenticated) {
            Reply("530 Log in first.\r\n");
        } else if (verb == "ABOR") {
            Abort();
        } else if (verb == "NOOP") {
            Reply("200 OK.\r\n");
        } else if (transferring) {
            Reply("450 Transfer in progress.\r\n");
        } else if (verb == "PASV" || verb == "EPSV") {
            if (verb == "EPSV" && !argument.empty() && argument != "1")
                Reply("522 Use IPv4.\r\n");
            else
                Passive(verb == "EPSV");
        } else if (verb == "ALLO") {
            allocation = ParseAllocation(argument);
            Reply(allocation ? "200 Upload size accepted for next upload.\r\n"
                             : "501 Expected an unsigned byte count.\r\n");
        } else if (verb == "PWD") {
            std::string quoted;
            for (const char letter : cwd) {
                quoted += letter;
                if (letter == '"')
                    quoted += '"';
            }
            Reply("257 \"/" + quoted + "\"\r\n");
        } else if (verb == "TYPE") {
            Reply(argument == "A" || argument == "I" ? "200 Binary transfers.\r\n"
                                                     : "504 Unsupported type.\r\n");
        } else if (verb == "REST") {
            Reply(argument == "0" ? "350 Restart at zero.\r\n" : "504 Only REST 0 supported.\r\n");
        } else if (verb == "CWD" || verb == "CDUP" || verb == "SIZE" || verb == "MDTM" ||
                   verb == "STOR" || verb == "APPE" || verb == "RETR" || verb == "LIST" ||
                   verb == "NLST" || verb == "MLSD" || verb == "DELE" || verb == "MKD" ||
                   verb == "RMD" || verb == "RNFR" || verb == "RNTO") {
            const auto source = rename_from;
            rename_from.reset();
            const auto relative = NormalizePath(cwd, verb == "CDUP" ? ".." : argument);
            if (!relative) {
                Reply("550 Invalid path.\r\n");
                return true;
            }
            try {
                const bool virtual_directory = relative->find('/') == std::string::npos;
                if (virtual_directory && verb != "CWD" && verb != "CDUP" && verb != "LIST" &&
                    verb != "NLST" && verb != "MLSD")
                    throw std::runtime_error("virtual root is read-only");
                const auto target = Resolve(*relative);
                if (verb == "CWD" || verb == "CDUP") {
                    if (!Common::FS::IsDir(target))
                        throw std::runtime_error("not a directory");
                    cwd = *relative;
                    Reply("250 Directory changed.\r\n");
                } else if (verb == "SIZE") {
                    if (!Common::FS::IsFile(target))
                        throw std::runtime_error("not a file");
                    Reply("213 " + std::to_string(Common::FS::GetSize(target)) + "\r\n");
                } else if (verb == "MDTM") {
                    Reply("213 " + Timestamp(Modified(target)) + "\r\n");
                } else if (verb == "RNFR") {
                    if (relative->empty() || !Common::FS::Exists(target))
                        throw std::runtime_error("missing source");
                    rename_from = *relative;
                    Reply("350 Send RNTO.\r\n");
                } else if (verb == "RNTO") {
                    if (!source) {
                        Reply("503 Send RNFR first.\r\n");
                    } else {
                        if (relative->empty() ||
                            !CanRenameTo(*relative, Common::FS::Exists(target)) ||
                            !Publish(Resolve(*source), target, InGames(*relative)))
                            throw std::runtime_error("rename failed");
                        Changed();
                        Reply("250 Renamed.\r\n");
                    }
                } else if (verb == "MKD") {
                    if (!CreateDirectoryFromAppW(target.c_str(), nullptr))
                        throw std::runtime_error("cannot create directory");
                    Changed();
                    Reply("257 Directory created.\r\n");
                } else if (verb == "DELE" || verb == "RMD") {
                    if (relative->empty() || (verb == "RMD") != Common::FS::IsDir(target) ||
                        !(verb == "RMD" ? RemoveDirectoryFromAppW(target.c_str())
                                        : DeleteFileFromAppW(target.c_str())))
                        throw std::runtime_error("cannot remove");
                    Changed();
                    Reply("250 Removed.\r\n");
                } else {
                    const bool listing = verb == "LIST" || verb == "NLST" || verb == "MLSD";
                    if (listing ? !Common::FS::IsDir(target) : relative->empty())
                        throw std::runtime_error("invalid target");
                    if (verb == "RETR" && !Common::FS::IsFile(target))
                        throw std::runtime_error("not a file");
                    if ((verb == "STOR" || verb == "APPE") &&
                        (!Common::FS::IsDir(target.parent_path()) ||
                         (Common::FS::Exists(target) && !Common::FS::IsFile(target))))
                        throw std::runtime_error("invalid upload target");
                    const bool upload = verb == "STOR" || verb == "APPE";
                    const auto expected = upload ? std::exchange(allocation, std::nullopt)
                                                 : std::optional<std::uint64_t>{};
                    if (upload && !expected) {
                        Reply("503 Send ALLO with exact upload byte count first.\r\n");
                        return true;
                    }
                    bool has_passive;
                    {
                        std::lock_guard lock(sockets_mutex);
                        has_passive = static_cast<bool>(passive);
                    }
                    if (!has_passive) {
                        Reply("425 Use PASV or EPSV first.\r\n");
                    } else {
                        if (transfer.joinable())
                            transfer.join();
                        transferring = true;
                        Reply("150 Opening data connection.\r\n");
                        try {
                            transfer = std::thread([self = shared_from_this(), verb,
                                                    path = *relative, expected] {
                                bool initialized = false;
                                try {
                                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                                    initialized = true;
                                    self->Transfer(verb, path, expected);
                                } catch (...) {
                                    self->transferring = false;
                                    self->Stop();
                                }
                                if (initialized)
                                    winrt::uninit_apartment();
                            });
                        } catch (...) {
                            transferring = false;
                            throw;
                        }
                    }
                }
            } catch (...) {
                Reply("550 File action unavailable.\r\n");
            }
        } else {
            rename_from.reset();
            Reply("502 Command not implemented.\r\n");
        }
        return !stopped;
    }
    void Run() {
        bool initialized = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
            Reply("220 Jadyx FTP ready.\r\n");
            DataReader reader(control.InputStream());
            reader.InputStreamOptions(InputStreamOptions::Partial);
            std::string pending;
            bool keep = true;
            while (keep && !stopped) {
                if (deadline.Expired())
                    break;
                const auto load = reader.LoadAsync(1024);
                // A WinRT operation accepts only one Completed delegate. Poll a future,
                // rather than repeatedly installing a delegate through load.wait_for().
                const auto completion = std::make_shared<std::promise<void>>();
                auto completed = completion->get_future();
                load.Completed([completion](const auto&, const auto) { completion->set_value(); });
                while (completed.wait_for(250ms) != std::future_status::ready) {
                    if (stopped || deadline.Expired()) {
                        load.Cancel();
                        throw std::runtime_error("control session timed out");
                    }
                }
                if (load.get() == 0 || deadline.Expired())
                    break;
                while (reader.UnconsumedBufferLength() != 0) {
                    const auto byte = reader.ReadByte();
                    if (byte == 0 || pending.size() >= 8192)
                        throw std::runtime_error("invalid command");
                    pending += static_cast<char>(byte);
                    if (pending.ends_with("\r\n")) {
                        if (deadline.Expired()) {
                            keep = false;
                        } else {
                            deadline.Activity();
                            keep = Handle(ParseCommand(pending));
                        }
                        pending.clear();
                        if (!keep)
                            break;
                    }
                }
            }
            reader.DetachStream();
        } catch (...) {
        }
        Stop();
        if (transfer.joinable())
            transfer.join();
        LOG_INFO(Frontend, "FTP session closed");
        {
            std::lock_guard lock(finish_mutex);
            done = true;
        }
        finished.notify_all();
        if (initialized)
            winrt::uninit_apartment();
    }
};
} // namespace

struct Server::State {
    mutable std::mutex mutex;
    StreamSocketListener listener{nullptr};
    std::shared_ptr<Session> session;
    std::shared_ptr<std::atomic<std::uint64_t>> changes =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<LoginBackoff> login_backoff = std::make_shared<LoginBackoff>();
    std::string ip;
    std::string password;
    bool running = false;
};
Server::Server() : state_(std::make_shared<State>()) {}
Server::~Server() { Stop(); }
bool Server::Start(const Roots& roots, const std::string& ipv4, std::string_view test_password) {
    Stop();
    std::lock_guard lock(state_->mutex);
    try {
        if (!PrivateIPv4(ipv4))
            return false;
        state_->ip = ipv4;
        using winrt::Windows::Security::Cryptography::CryptographicBuffer;
        std::uint32_t random;
        do {
            random = CryptographicBuffer::GenerateRandomNumber();
        } while (random >= 4294800000u);
        state_->password = test_password.empty() ? std::to_string(100000u + random % 900000u)
                                                 : std::string{test_password};
        for (const auto& directory : {roots.games, roots.keys, roots.firmware, roots.saves}) {
            if (directory.empty())
                throw std::runtime_error("missing root");
            fs::create_directories(directory);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExFromAppW(directory.c_str(), GetFileExInfoStandard,
                                             &attributes) ||
                (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                throw std::runtime_error("invalid root");
        }
        StreamSocketListener listener;
        const std::weak_ptr<State> weak = state_;
        listener.ConnectionReceived([weak, roots](const auto&, const auto& args) {
            const auto owner = weak.lock();
            const auto socket = args.Socket();
            if (!owner) {
                CloseSocket(socket);
                return;
            }
            std::lock_guard session_lock(owner->mutex);
            const auto remote =
                winrt::to_string(socket.Information().RemoteAddress().CanonicalName());
            if (!PrivateIPv4(remote)) {
                CloseSocket(socket);
                return;
            }
            if (!owner->running || (owner->session && !owner->session->done)) {
                try {
                    std::thread([socket] {
                        bool initialized = false;
                        try {
                            winrt::init_apartment(winrt::apartment_type::multi_threaded);
                            initialized = true;
                            Write(socket, "421 Server busy.\r\n");
                        } catch (...) {
                        }
                        CloseSocket(socket);
                        if (initialized)
                            winrt::uninit_apartment();
                    }).detach();
                } catch (...) {
                    CloseSocket(socket);
                }
                return;
            }
            auto session = std::make_shared<Session>();
            session->control = socket;
            session->roots = roots;
            session->ip = owner->ip;
            session->password = owner->password;
            session->changes = owner->changes;
            session->login_backoff = owner->login_backoff;
            session->peer = winrt::to_string(socket.Information().RemoteAddress().CanonicalName());
            try {
                std::thread worker([session] { session->Run(); });
                owner->session = session;
                worker.detach();
            } catch (...) {
                CloseSocket(socket);
            }
        });
        // Listen on every interface: bound to the LAN address alone the port stayed closed
        // to the LAN on the console; state_->ip is only what the UI and PASV show.
        listener.BindServiceNameAsync(L"2121").get();
        state_->listener = listener;
        state_->running = true;
        LOG_INFO(Frontend, "FTP listening on port 2121");
        return true;
    } catch (...) {
        state_->password.clear();
        LOG_WARNING(Frontend, "FTP start failed");
        return false;
    }
}
void Server::Stop() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        if (state_->listener) {
            LOG_INFO(Frontend, "FTP stopped");
            CloseSocket(state_->listener);
            state_->listener = nullptr;
        }
        session = std::move(state_->session);
        state_->password.clear();
    }
    if (session) {
        session->Stop();
        session->Wait();
    }
}
bool Server::Running() const {
    std::lock_guard lock(state_->mutex);
    return state_->running;
}
std::wstring Server::Address() const {
    std::lock_guard lock(state_->mutex);
    return L"ftp://" + std::wstring(winrt::to_hstring(state_->ip)) + L":2121";
}
std::wstring Server::Password() const {
    std::lock_guard lock(state_->mutex);
    return std::wstring(winrt::to_hstring(state_->password));
}
std::uint64_t Server::Changes() const { return state_->changes->load(); }
} // namespace Jadyx::Ftp
