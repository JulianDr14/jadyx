// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/network_portal.h"
#include "jadyx/storage/log_export.h"
#include "jadyx/storage/network_portal_logo.h"
#include "jadyx/storage/network_portal_page.h"

// Match the httplib configuration used by the rest of common (including SSL feature macros).
#include "common/httplib.h"
#include <iphlpapi.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <atomic>
#include <mutex>
#include <set>
#include <fstream>
#include <fileapifromapp.h>
#include <map>
#include <thread>
#include <tuple>
#include <cctype>
#include <format>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>

namespace Jadyx {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

// Keep short OS polling intervals for cancellation, but tolerate 30s idle LAN retransmits.
// Windows WSAPoll may not wake on local shutdown, so a 30s kernel timeout is not suitable.
class PortalStream final : public httplib::Stream {
public:
    PortalStream(httplib::Stream& source_, const std::atomic<bool>& stopped_) : source{source_}, stopped{stopped_} {}
    bool is_readable() const override { return !stopped && !read_closed && source.is_readable(); }
    bool wait_readable() const override { return Wait(false); }
    bool wait_writable() const override { return Wait(true); }
    bool is_peer_alive() const override { return !stopped && source.is_peer_alive(); }
    ssize_t read(char* bytes, size_t size) override {
        // SocketStream::read already polls before recv. Polling here too doubles the
        // readiness syscalls on every unbuffered body read (16 KiB in httplib 0.46).
        const auto until=Clock::now()+std::chrono::seconds{30};
        while (!stopped && !read_closed) {
            const auto count=source.read(bytes,size);
            if (count >= 0 || source.get_error() != httplib::Error::Timeout) {
                error_=count>0 ? httplib::Error::Success : source.get_error();
                return stopped ? -1 : count;
            }
            if (Clock::now() >= until) { error_=httplib::Error::Timeout; return -1; }
        }
        error_=httplib::Error::ConnectionClosed;
        return -1;
    }
    ssize_t write(const char* bytes, size_t size) override {
        if (!Wait(true)) return -1;
        return stopped ? -1 : source.write(bytes, size);
    }
    void get_remote_ip_and_port(std::string& ip, int& port) const override { source.get_remote_ip_and_port(ip, port); }
    void get_local_ip_and_port(std::string& ip, int& port) const override { source.get_local_ip_and_port(ip, port); }
    socket_t socket() const override { return source.socket(); }
    time_t duration() const override { return source.duration(); }
    void set_read_timeout(time_t, time_t = 0) override {} // Only our bounded, cancellable waits.
    void CloseAfterResponse() { read_closed=true; }
    bool Closing() const { return read_closed; }
private:
    bool Wait(bool write) const {
        if (!write && read_closed) return false;
        const auto until=Clock::now()+std::chrono::seconds{30};
        while (!stopped) {
            if (write ? source.wait_writable() : (source.is_readable() || source.wait_readable())) return !stopped;
            if (Clock::now() >= until) return false;
        }
        return false;
    }
    httplib::Stream& source;
    const std::atomic<bool>& stopped;
    bool read_closed{};
};

// Request handlers execute synchronously on one worker. This scoped slot lets the response
// hook disable httplib 0.46.0's drain after committing Connection: close on an early rejection.
thread_local PortalStream* current_portal_stream{};
struct PortalStreamScope {
    PortalStream* previous{current_portal_stream};
    explicit PortalStreamScope(PortalStream& stream) { current_portal_stream=&stream; }
    ~PortalStreamScope() { current_portal_stream=previous; }
};

// httplib's destructor does not close a socket bound before listen_after_bind starts. Keep
// ownership even if creating the listener/thread pool throws after a successful bind.
class PortalServer final : public httplib::Server {
public:
    void CancelClients() {
        std::lock_guard lock{clients_mutex};
        stopping = true;
        for (const auto socket : clients) ::shutdown(socket, SD_BOTH);
    }

    ~PortalServer() override {
        const auto socket = svr_sock_.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) { ::shutdown(socket, SD_BOTH); ::closesocket(socket); }
    }

protected:
    bool process_and_close_socket(socket_t socket) override {
        {
            std::lock_guard lock{clients_mutex};
            if (stopping) { ::closesocket(socket); return false; }
            clients.insert(socket);
        }
        bool result = false, rejected = false;
        try {
            std::string remote, local;
            int remote_port{}, local_port{};
            httplib::detail::get_remote_ip_and_port(socket, remote, remote_port);
            httplib::detail::get_local_ip_and_port(socket, local, local_port);
            // Same plain-HTTP processing path as httplib 0.46.0. Own the final close so the
            // cancellation registry cannot refer to a recycled descriptor (including on error).
            result = httplib::detail::process_server_socket(svr_sock_, socket,
                keep_alive_max_count_, keep_alive_timeout_sec_, read_timeout_sec_, read_timeout_usec_,
                write_timeout_sec_, write_timeout_usec_,
                [&](httplib::Stream& stream, bool close, bool& closed) {
                    PortalStream cancellable{stream, stopping};
                    PortalStreamScope scope{cancellable};
                    const bool handled=process_request(cancellable, remote, remote_port, local, local_port, close, closed, nullptr);
                    closed |= cancellable.Closing();
                    rejected |= cancellable.Closing();
                    return handled;
                });
        } catch (...) {}
        if (rejected && !stopping) {
            // Finish the response before discarding a bounded amount of already-sent
            // input. Closing immediately with unread TCP data can replace 413/409
            // with a reset on Windows. Never wait for the whole advertised body.
            ::shutdown(socket,SD_SEND);
            u_long nonblocking=1;
            if (::ioctlsocket(socket,FIONBIO,&nonblocking)==0) {
                const auto until=Clock::now()+std::chrono::milliseconds{100};
                std::array<char,4096> discarded{};
                std::size_t drained{};
                while (!stopping && drained<(1U << 20) && Clock::now()<until) {
                    if (httplib::detail::select_read(socket,0,10000)<=0) continue;
                    const auto count=::recv(socket,discarded.data(),static_cast<int>(discarded.size()),0);
                    if (count<=0) break;
                    drained+=static_cast<std::size_t>(count);
                }
            }
        }
        {
            std::lock_guard lock{clients_mutex};
            clients.erase(socket);
            ::shutdown(socket, SD_BOTH);
            ::closesocket(socket);
        }
        return result;
    }

private:
    std::mutex clients_mutex;
    std::set<socket_t> clients;
    std::atomic<bool> stopping{};
};



bool EqualSecret(std::string_view a, std::string_view b) {
    return a.size() == b.size() && !a.empty() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string RandomSession() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}

std::string RandomCode() {
    // Rejection sampling avoids modulo bias in the four decimal digits. Four are enough on a LAN:
    // five tries a minute and five minutes per code leave a guess at 25 in 10000.
    std::string result;
    while (result.size() < 4) {
        unsigned char byte{};
        if (RAND_bytes(&byte, 1) != 1) return {};
        if (byte < 250) result += static_cast<char>('0' + byte % 10);
    }
    return result;
}

void Reply(httplib::Response& response, int status, const Json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

bool Unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto result=std::from_chars(text.data(), text.data()+text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data()+text.size();
}

bool Decode64(const std::string& encoded, std::string& decoded) {
    if (encoded.empty() || encoded.size() > 1024 || encoded.size() % 4) return false;
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned accumulator{}, bits{};
    decoded.clear();
    bool padding=false;
    for (const char c : encoded) {
        if (c == '=') { padding=true; continue; }
        const auto digit=alphabet.find(c);
        if (padding || digit == std::string_view::npos) return false;
        accumulator=(accumulator << 6) | static_cast<unsigned>(digit);
        bits+=6;
        if (bits >= 8) { bits-=8; decoded+=static_cast<char>((accumulator >> bits) & 255); }
    }
    return httplib::detail::base64_encode(decoded) == encoded;
}

std::string Hex16(std::uint64_t value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4) result[static_cast<std::size_t>(i)] = hex[value & 15];
    return result;
}

bool ParseHex16(std::string_view text, std::uint64_t& value) {
    if (text.size() != 16 || text.find_first_not_of("0123456789abcdef") != std::string_view::npos) return false;
    return std::from_chars(text.data(), text.data() + text.size(), value, 16).ec == std::errc{};
}

Json UploadJson(const UploadSnapshot& info) {
    const auto& p=info.performance;
    return {{"id",info.id}, {"name",info.name}, {"fingerprint",info.fingerprint}, {"tag",info.tag},
        {"phase",info.phase}, {"error",info.error}, {"length",info.length}, {"offset",info.offset},
        {"received",info.received}, {"free_bytes",info.free_bytes}, {"busy",info.busy}, {"available",info.available},
        {"performance",{{"confirmed_offset",p.confirmed_offset},{"bytes",p.bytes},{"total_us",p.total_us},
            {"write_us",p.write_us},{"flush_us",p.flush_us},{"checkpoint_us",p.checkpoint_us},{"reader_calls",p.reader_calls}}}};
}
Json RemovalJson(const PortalRemoval& info) {
    return {{"serial", info.serial}, {"game", info.application ? Hex16(info.application) : ""},
        {"content", info.content}, {"phase", info.phase}, {"error", info.error}};
}

Json SystemJson(const SystemUploadStatus& info){
    const auto& r=info.review;
    auto files=Json::array();for(const auto& f:info.files)files.push_back({{"name",f.name},{"length",f.length},{"fingerprint",f.fingerprint}});
    return {{"id",info.id},{"kind",info.kind},{"format",info.format},{"phase",info.phase},{"error",info.error},
        {"count",info.count},{"completed",info.completed},{"processed",info.processed},{"bytes",info.bytes},{"busy",info.busy},{"available",info.available},
        {"file",UploadJson(info.file)},{"files",files},{"review",{{"installed_firmware",r.installed_firmware},{"incoming_firmware",r.incoming_firmware},
            {"installed_generation",r.installed_generation},{"incoming_generation",r.incoming_generation},{"required_generation",r.required_generation},{"warnings",r.warnings}}}};
}

bool LocalIpv4(const std::string& address) {
    in_addr ip{};
    if (InetPtonA(AF_INET, address.c_str(), &ip) != 1) return false;
    const auto v = ntohl(ip.s_addr);
    return (v >> 24) == 10 || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8 ||
           (v >> 16) == 0xa9fe || (v >> 24) == 127; // loopback used only by the native harness
}

/// httplib does not say why a bind failed. Binding the same way again tells: the port is
/// taken, the system refuses it, or the address no longer belongs to the console.
int BindError(const std::string& ipv4, unsigned short port) {
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return ::WSAGetLastError();
    const BOOL exclusive = TRUE;
    int error = ::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0 ? 0 : ::WSAGetLastError();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (!error && InetPtonA(AF_INET, ipv4.c_str(), &address.sin_addr) != 1) error = WSAEADDRNOTAVAIL;
    if (!error && ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        error = ::WSAGetLastError();
    ::closesocket(socket);
    return error;
}

PortalError BindFailure(int error) {
    switch (error) {
    case WSAEADDRINUSE: return PortalError::PortInUse;
    case WSAEACCES: return PortalError::PortBlocked;
    case WSAEADDRNOTAVAIL: return PortalError::AddressLost;
    default: return PortalError::Bind;
    }
}
} // namespace

namespace {
std::vector<unsigned char> AdapterTable() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> data(size);
    ULONG result{};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(data.data()), &size);
        if (result != ERROR_BUFFER_OVERFLOW) break;
        if (size > 1024 * 1024) return {};
        data.resize(size);
    }
    if (result != NO_ERROR) data.clear();
    return data;
}

std::string LowerGuid(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '{' || c == '}'; }), text.end());
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// What Windows' connection manager knows of each adapter, by adapter GUID. The console can
/// show its wireless link as an Ethernet interface, so the interface type alone is not enough.
struct AdapterProfile { bool wlan{}, internet{}; std::uint32_t iana{}; };
std::map<std::string, AdapterProfile> AdapterProfiles() {
    using namespace winrt::Windows::Networking::Connectivity;
    std::map<std::string, AdapterProfile> result;
    try {
        for (const auto& profile : NetworkInformation::GetConnectionProfiles()) {
            const auto adapter = profile.NetworkAdapter();
            if (!adapter) continue;
            auto& entry = result[LowerGuid(winrt::to_string(winrt::to_hstring(adapter.NetworkAdapterId())))];
            entry.wlan |= profile.IsWlanConnectionProfile();
            entry.iana = adapter.IanaInterfaceType();
        }
        if (const auto internet = NetworkInformation::GetInternetConnectionProfile())
            if (const auto adapter = internet.NetworkAdapter())
                result[LowerGuid(winrt::to_string(winrt::to_hstring(adapter.NetworkAdapterId())))].internet = true;
    } catch (...) {} // Without the connection manager the interface type still decides.
    return result;
}

bool Wireless(const IP_ADAPTER_ADDRESSES& adapter, const AdapterProfile* profile) {
    return adapter.IfType == IF_TYPE_IEEE80211 ||
        (profile && (profile->wlan || profile->iana == IF_TYPE_IEEE80211));
}

std::string Narrow(const wchar_t* text) {
    return text ? winrt::to_string(text) : std::string{};
}
} // namespace

std::vector<PortalAddress> PortalAddresses() {
    const auto data = AdapterTable();
    if (data.empty()) return {};
    const auto profiles = AdapterProfiles();
    struct Ranked { PortalAddress address; bool internet{}, deprecated{}; ULONG metric{}; };
    std::vector<Ranked> found;
    for (auto* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(data.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp ||
            (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211)) continue;
        const auto profile_it = profiles.find(LowerGuid(a->AdapterName ? a->AdapterName : ""));
        const auto* profile = profile_it != profiles.end() ? &profile_it->second : nullptr;
        const auto link = Wireless(*a, profile) ? PortalLink::WiFi : PortalLink::Ethernet;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            // Tentative, duplicate or invalid addresses are not the console's yet (or anymore).
            if (u->DadState != IpDadStatePreferred && u->DadState != IpDadStateDeprecated) continue;
            char address[INET_ADDRSTRLEN]{};
            auto* ip = &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            if (!InetNtopA(AF_INET, ip, address, sizeof(address)) || !LocalIpv4(address)) continue;
            if ((ntohl(ip->s_addr) >> 24) == 127) continue; // the native harness passes loopback itself
            if (std::any_of(found.begin(), found.end(), [&](const Ranked& r) { return r.address.ipv4 == address; }))
                continue;
            // Friendly adapter names never become HTML or a command; the local UI renders text.
            found.push_back({{link == PortalLink::WiFi ? "Wi-Fi" : "Ethernet", address, link,
                              a->FirstGatewayAddress != nullptr, (ntohl(ip->s_addr) >> 16) == 0xa9fe},
                             profile && profile->internet, u->DadState == IpDadStateDeprecated, a->Ipv4Metric});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Ranked& l, const Ranked& r) {
        return std::tuple{l.address.link_local, !l.address.gateway, !l.internet, l.deprecated, l.metric} <
               std::tuple{r.address.link_local, !r.address.gateway, !r.internet, r.deprecated, r.metric};
    });
    std::vector<PortalAddress> result;
    for (auto& r : found) result.push_back(std::move(r.address));
    return result;
}

bool PortalAddressOwned(const std::string& ipv4) {
    const auto data = AdapterTable();
    for (auto* a = data.empty() ? nullptr : reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(data.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char address[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, address,
                          sizeof(address)) && ipv4 == address)
                return true;
        }
    }
    return false;
}

std::string PortalAdapterReport() {
    const auto data = AdapterTable();
    if (data.empty()) return "adapters: GetAdaptersAddresses failed\n";
    const auto profiles = AdapterProfiles();
    std::string report;
    for (auto* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(data.data()); a; a = a->Next) {
        const auto profile_it = profiles.find(LowerGuid(a->AdapterName ? a->AdapterName : ""));
        const auto* profile = profile_it != profiles.end() ? &profile_it->second : nullptr;
        const bool kept_type = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
        report += std::format("adapter '{}' ({}) type={} up={} metric={} gateway={} profile={}{}\n",
            Narrow(a->FriendlyName), Narrow(a->Description), a->IfType, a->OperStatus == IfOperStatusUp,
            a->Ipv4Metric, a->FirstGatewayAddress != nullptr,
            profile ? std::format("wlan={} iana={} internet={}", profile->wlan, profile->iana, profile->internet) : "none",
            !kept_type ? " [skipped: type]" : a->OperStatus != IfOperStatusUp ? " [skipped: down]" :
                Wireless(*a, profile) ? " [Wi-Fi]" : " [Ethernet]");
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char address[INET_ADDRSTRLEN]{};
            InetNtopA(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, address, sizeof(address));
            report += std::format("  ipv4 {} dad={} prefix={} lan={}\n", address, static_cast<int>(u->DadState),
                                  static_cast<int>(u->OnLinkPrefixLength), LocalIpv4(address));
        }
    }
    return report;
}

struct NetworkPortal::Impl {
    std::unique_ptr<LogExport> logs;
    std::string logs_ticket;
    std::uint64_t logs_ticket_run{};
    Clock::time_point logs_ticket_until{};
    std::mutex admission;
    std::function<std::optional<Updates::UpdatePackage>()> update_package;
    std::atomic<bool> update_offered{};
    std::optional<Updates::UpdatePackage> ticket_package;
    std::string update_ticket;
    Clock::time_point update_ticket_until{};
    bool imports_available{},system_supported{};
    std::mutex lifecycle;
    mutable std::mutex mutex;
    PortalStatus status;
    Clock::time_point expires{}, attempts_window{};
    unsigned attempts{};
    std::string session;
    std::string authority;
    std::unique_ptr<PortalServer> server;
    bool exclusive_socket{};
    std::jthread listener;
    std::unique_ptr<NspUpload> upload;
    std::unique_ptr<NspUpload> content; ///< Update/DLC packages, consumed into internal storage.
    std::unique_ptr<SystemUpload> system;
    PortalLibrary library;
    bool storage_failed{};

    /// Excludes both resumable uploads, or neither. Caller holds `admission`.
    bool BlockUploads() {
        if (upload && !upload->BlockForLocalOperation()) return false;
        if (content && !content->BlockForLocalOperation()) {
            if (upload) upload->SetAvailable(imports_available);
            return false;
        }
        return true;
    }
    void RestoreUploads() {
        if (upload) upload->SetAvailable(imports_available);
        if (content) content->SetAvailable(imports_available);
    }
    bool RemovalActive() const {
        std::lock_guard lock{mutex};
        return status.removal.phase == "pending" || status.removal.phase == "running";
    }

    void Shutdown() {
        if (logs) logs->Stop();
        if (upload) upload->Cancel();
        if (content) content->Cancel();
        if (system) system->Cancel();
        {
            std::lock_guard lock{mutex};
            status.running = status.paired = false;
            update_ticket.clear(); ticket_package.reset();
            logs_ticket.clear();
            status.code.clear(); status.url.clear(); session.clear();
        }
        if (server) { server->stop(); server->CancelClients(); server->decommission(); }
        if (listener.joinable()) listener.join();
        server.reset();
        if (upload) upload->Suspend();
        if (content) content->Suspend();
        if (system) system->Suspend();
    }

    void Routes() {
        server->new_task_queue = [] { return new httplib::ThreadPool(4, 4, 8); };
        server->set_tcp_nodelay(true);
        server->set_socket_options([this](socket_t socket) {
            const BOOL exclusive = TRUE;
            exclusive_socket = ::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0;
        });
        server->set_payload_max_length(NspUpload::MaxChunk);
        // PortalStream extends these short OS polls into a cancellable 30s idle wait.
        server->set_read_timeout(0, 100000);
        server->set_write_timeout(0, 100000);
        server->set_keep_alive_timeout(2);
        server->set_keep_alive_max_count(10000);
        server->set_post_routing_handler([](const auto&, auto& response) {
            if (response.get_header_value("Connection") == "close" && current_portal_stream)
                current_portal_stream->CloseAfterResponse();
        });
        server->set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
            {"Referrer-Policy", "no-referrer"}, {"X-Frame-Options", "DENY"},
            {"Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' blob:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}});
        server->set_pre_routing_handler([this](const auto& request, auto& response) {
            std::lock_guard lock{mutex};
            if (!status.running) { Reply(response, 503, {{"error", "stopped"}}); return httplib::Server::HandlerResponse::Handled; }
            if (request.get_header_value_count("Host") != 1 || request.get_header_value("Host") != authority ||
                request.get_header_value_count("Origin") > 1 ||
                (request.has_header("Origin") && request.get_header_value("Origin") != "http://" + authority)) {
                Reply(response, 403, {{"error", "origin"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            const bool patch=request.method == "PATCH";
            std::uint64_t length{};
            if (request.has_header("Transfer-Encoding") || request.has_header("Content-Encoding") ||
                (request.has_header("Content-Length") &&
                 (!Unsigned(request.get_header_value("Content-Length"), length) || length > (patch ? NspUpload::MaxChunk :
                     request.path == "/api/v1/pair" ? 1024 : 4096)))) {
                response.set_header("Connection", "close");
                Reply(response, 413, {{"error", "chunk"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            if ((request.path == "/" || request.path == "/logo.png") && request.method == "GET")
                return httplib::Server::HandlerResponse::Unhandled;
            if (request.path == "/api/v1/pair" && request.method == "POST" &&
                request.get_header_value("Origin") == "http://" + authority)
                return httplib::Server::HandlerResponse::Unhandled;
            if (request.method == "GET" && request.path == "/api/v1/update/package" &&
                request.get_param_value_count("ticket")==1 && !update_ticket.empty() && Clock::now() < update_ticket_until &&
                EqualSecret(request.get_param_value("ticket"), update_ticket)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if ((request.method == "GET" || request.method == "HEAD") &&
                request.path == "/api/v1/logs/archive" &&
                request.get_param_value_count("ticket") == 1 && !logs_ticket.empty() &&
                Clock::now() < logs_ticket_until &&
                EqualSecret(request.get_param_value("ticket"), logs_ticket))
                return httplib::Server::HandlerResponse::Unhandled;
            if (request.get_header_value_count("Authorization") != 1 ||
                !EqualSecret(request.get_header_value("Authorization"), "Bearer " + session) || session.empty()) {
                Reply(response, 401, {{"error", "session"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server->Get("/", [](const auto&, auto& response) {
            response.set_content(PortalWeb::Page.data(), PortalWeb::Page.size(), "text/html; charset=utf-8");
        });
        server->Get("/logo.png", [](const auto&, auto& response) {
            response.set_content(reinterpret_cast<const char*>(PortalWeb::Logo), PortalWeb::LogoSize, "image/png");
        });
        server->Post("/api/v1/pair", [this](const auto& request, auto& response) {
            const auto body = Json::parse(request.body, nullptr, false);
            std::lock_guard lock{mutex};
            const auto now = Clock::now();
            if (now - attempts_window >= std::chrono::minutes{1}) { attempts_window = now; attempts = 0; }
            if (attempts >= 5) {
                response.set_header("Retry-After", "60");
                Reply(response, 429, {{"error", "attempts"}}); return;
            }
            ++attempts;
            if (!status.running || body.is_discarded() || !body.is_object() ||
                !body.contains("code") || !body["code"].is_string() ||
                now >= expires || !EqualSecret(body["code"].get<std::string>(), status.code)) {
                Reply(response, 401, {{"error", "code"}}); return;
            }
            auto next = RandomSession();
            if (next.empty()) { Reply(response, 503, {{"error", "random"}}); return; }
            // A code pairs one browser once. Existing sessions are invalidated on re-pairing.
            session = std::move(next);
            logs_ticket.clear();
            update_ticket.clear(); ticket_package.reset();
            status.paired = true;
            expires = now;
            Reply(response, 200, {{"session", session}});
        });
        server->Get("/api/v1/status", [this](const auto&, auto& response) {
            Reply(response, 200, {{"running", true}, {"stage", upload ? "nsp" : "connection-test"},
                {"uploads", bool(upload)}, {"transfer", upload ? UploadJson(upload->Snapshot()) : Json::object()},
                {"library", bool(library.games)}, {"content", content ? UploadJson(content->Snapshot()) : Json::object()},
                {"install", InstallJson()},
                {"removal", [this] { std::lock_guard lock{mutex}; return RemovalJson(status.removal); }()},
                {"system_supported",system_supported},{"system",system ? SystemJson(system->Snapshot()) : Json::object()},
                {"logs", LogsJson()}, {"update", {{"offered", update_offered.load()}}}});
        });
        server->Post("/api/v1/update/download", [this](const auto&, auto& response) {
            std::lock_guard admission_lock{admission};
            if ((system && system->Snapshot().busy) || !BlockUploads()) {
                Reply(response,409,{{"error","busy"}}); return;
            }
            std::lock_guard lock{mutex};
            const auto package=update_package ? update_package() : std::nullopt;
            if (!package) { Reply(response,409,{{"error","verify_in_app"}}); return; }
            update_ticket=RandomSession(); update_ticket_until=Clock::now()+std::chrono::minutes{2}; ticket_package=package;
            if (update_ticket.empty()) { Reply(response,503,{{"error","random"}}); return; }
            Reply(response,200,{{"download","/api/v1/update/package?ticket="+update_ticket},{"filename",package->FileName()},{"version",package->version},{"size",package->size}});
        });
        server->Get("/api/v1/update/package", [this](const auto& request, auto& response) {
            std::optional<Updates::UpdatePackage> package;
            {
                std::lock_guard lock{mutex};
                // Consume in the route as well as checking in pre-routing: parallel GETs cannot replay it.
                if (request.get_param_value_count("ticket")!=1 || update_ticket.empty() || Clock::now()>=update_ticket_until ||
                    !EqualSecret(request.get_param_value("ticket"),update_ticket) || !ticket_package) {
                    Reply(response,401,{{"error","ticket"}}); return;
                }
                const auto current=update_package ? update_package() : std::nullopt;
                update_ticket.clear();
                if (!current || *current!=*ticket_package) { ticket_package.reset(); Reply(response,409,{{"error","verify_in_app"}}); return; }
                package=std::move(ticket_package); ticket_package.reset();
            }
            const auto& path=package->path;
            const auto size=package->size;
            // Hold a read-only handle without FILE_SHARE_WRITE/DELETE for the whole response.
            // Pausing/cancelling or another download cannot change the bytes under the browser.
            struct PackageReader {
                HANDLE handle{INVALID_HANDLE_VALUE}; std::mutex io;
                ~PackageReader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
            };
            const auto reader=std::make_shared<PackageReader>();
            reader->handle=CreateFile2FromAppW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,nullptr);
            if (reader->handle == INVALID_HANDLE_VALUE) { Reply(response,409,{{"error","busy"}}); return; }
            response.set_header("Content-Disposition", "attachment; filename="+package->FileName());
            response.set_content_provider(static_cast<std::size_t>(size), "application/octet-stream",
                [reader](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                    std::lock_guard lock{reader->io};
                    LARGE_INTEGER position{}; position.QuadPart=static_cast<LONGLONG>(offset);
                    if (!SetFilePointerEx(reader->handle,position,nullptr,FILE_BEGIN)) return false;
                    std::array<char, 65536> bytes{};
                    while (length) {
                        const auto take=static_cast<DWORD>(std::min(length,bytes.size())); DWORD received{};
                        if (!ReadFile(reader->handle,bytes.data(),take,&received,nullptr) || received!=take || !sink.write(bytes.data(),take)) return false;
                        length-=take;
                    }
                    return true;
                });
        });
        UploadRoutes("/api/v1/uploads", upload.get(), content.get());
        UploadRoutes("/api/v1/content/uploads", content.get(), upload.get());
        LibraryRoutes();
        SystemRoutes();
        LogRoutes();
    }

    Json InstallJson() const {
        if (!library.progress) return {{"done", 0}, {"total", 0}};
        const auto [done, total] = library.progress();
        return {{"done", done}, {"total", total}};
    }

    Json LogsJson() const {
        if (!logs) return {{"supported", false}};
        const auto value = logs->Status();
        return {{"supported", true}, {"phase", value.phase}, {"busy", value.busy},
            {"error", value.error}, {"total", value.total}, {"processed", value.processed},
            {"compressed", value.compressed}, {"files", value.files},
            {"completed_files", value.completed_files}, {"filename", value.filename}};
    }

    void LogRoutes() {
        server->Post("/api/v1/logs/export", [this](const auto&, auto& response) {
            std::lock_guard admission_lock{admission};
            if (!logs) { Reply(response, 409, {{"error", "unavailable"}}); return; }
            const auto error = logs->Start();
            if (!error.empty()) { Reply(response, error == "busy" ? 409 : 503, {{"error", error}}); return; }
            { std::lock_guard lock{mutex}; logs_ticket.clear(); }
            Reply(response, 202, LogsJson());
        });
        server->Delete("/api/v1/logs/export", [this](const auto&, auto& response) {
            if (logs) logs->Cancel();
            Reply(response, 200, LogsJson());
        });
        server->Post("/api/v1/logs/download", [this](const auto&, auto& response) {
            std::lock_guard admission_lock{admission};
            // Granted while compressing too: the browser starts receiving the archive as
            // it is written instead of waiting for the whole export.
            const auto value = logs ? logs->Status() : LogExportStatus{};
            if (!logs || (!value.busy && value.phase != "ready")) {
                Reply(response, 409, {{"error", "not_ready"}}); return;
            }
            std::lock_guard lock{mutex};
            logs_ticket = RandomSession();
            if (logs_ticket.empty()) { Reply(response, 503, {{"error", "random"}}); return; }
            logs_ticket_run = value.run;
            logs_ticket_until = Clock::now() + std::chrono::minutes{30};
            Reply(response, 200, {{"download", "/api/v1/logs/archive?ticket=" + logs_ticket},
                {"filename", value.filename}, {"streaming", value.busy},
                {"size", value.busy ? Json(nullptr) : Json(value.compressed)}});
        });
        server->Get("/api/v1/logs/archive", [this](const auto& request, auto& response) {
            std::uint64_t run{};
            {
                std::lock_guard lock{mutex};
                if (request.get_param_value_count("ticket") != 1 || logs_ticket.empty() ||
                    Clock::now() >= logs_ticket_until ||
                    !EqualSecret(request.get_param_value("ticket"), logs_ticket)) {
                    Reply(response, 401, {{"error", "ticket"}}); return;
                }
                run = logs_ticket_run;
            }
            auto* const exporter = logs.get();
            const auto value = exporter ? exporter->Status() : LogExportStatus{};
            const bool ready = value.phase == "ready";
            // A range needs the final length; a resumed download asks again once it is ready.
            if (!exporter || value.run != run || (!ready && (!value.busy || !request.ranges.empty()))) {
                Reply(response, 409, {{"error", "not_ready"}}); return;
            }
            struct ArchiveReader {
                HANDLE handle{INVALID_HANDLE_VALUE};
                std::vector<char> bytes;
                ~ArchiveReader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
                // Positioned reads in 1 MiB blocks: no shared file pointer, few send calls.
                bool Send(std::uint64_t offset, std::uint64_t length, httplib::DataSink& sink) {
                    if (bytes.empty()) bytes.resize(1U << 20);
                    while (length) {
                        const auto take = static_cast<DWORD>(std::min<std::uint64_t>(length, bytes.size()));
                        OVERLAPPED at{};
                        at.Offset = static_cast<DWORD>(offset); at.OffsetHigh = static_cast<DWORD>(offset >> 32);
                        DWORD received{};
                        if (!ReadFile(handle, bytes.data(), take, &received, &at) || received != take ||
                            !sink.write(bytes.data(), take)) return false;
                        offset += take; length -= take;
                    }
                    return true;
                }
            };
            const auto reader = std::make_shared<ArchiveReader>();
            reader->handle = static_cast<HANDLE>(exporter->OpenArchive(run, [] { return true; }));
            if (reader->handle == INVALID_HANDLE_VALUE) {
                Reply(response, 409, {{"error", "busy"}}); return;
            }
            // Large send buffer: each 1 MiB block leaves in one call and the window never
            // starves while the next block is read, over Ethernet or Wi-Fi.
            if (current_portal_stream) {
                const int buffer = 4 << 20;
                ::setsockopt(current_portal_stream->socket(), SOL_SOCKET, SO_SNDBUF,
                             reinterpret_cast<const char*>(&buffer), sizeof(buffer));
            }
            response.set_header("Content-Disposition", "attachment; filename=\"" + value.filename + "\"");
            if (!ready) {
                // Follows the archive while it is compressed; chunked, since its size is not
                // known yet. A failed or cancelled export aborts the transfer, never truncates it.
                response.set_chunked_content_provider("application/zstd",
                    [reader, exporter, run](std::size_t offset, httplib::DataSink& sink) {
                        std::uint64_t available{};
                        switch (exporter->WaitFor(run, offset, available, [&sink] { return sink.is_writable(); })) {
                        case LogExport::Follow::End: sink.done(); return true;
                        case LogExport::Follow::Data: return reader->Send(offset, available - offset, sink);
                        default: return false;
                        }
                    });
                return;
            }
            response.set_header("Accept-Ranges", "bytes");
            // A strong validator lets the browser resume an interrupted download with Range.
            response.set_header("ETag", "\"" + std::to_string(run) + "-" + std::to_string(value.compressed) + "\"");
            response.set_content_provider(static_cast<std::size_t>(value.compressed), "application/zstd",
                [reader](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                    return reader->Send(offset, length, sink);
                });
        });
    }

    void LibraryRoutes() {
        // Read-only views. Ids are parsed here; the callbacks never see browser text.
        server->Get("/api/v1/library", [this](const auto&, auto& response) {
            if (!library.games) { Reply(response, 503, {{"error", "unavailable"}}); return; }
            try {
                const auto [revision, games] = library.games();
                auto list = Json::array();
                for (const auto& game : games)
                    list.push_back({{"id", Hex16(game.program_id)}, {"name", game.name},
                        {"developer", game.developer}, {"version", game.version}, {"removable", game.removable}});
                Reply(response, 200, {{"revision", revision}, {"games", std::move(list)}});
            } catch (...) { Reply(response, 500, {{"error", "library"}}); }
        });
        server->Get("/api/v1/library/([0-9a-f]{16})/cover", [this](const auto& request, auto& response) {
            std::uint64_t id{};
            if (!library.cover || !ParseHex16(request.matches[1].str(), id)) { Reply(response, 404, {{"error", "cover"}}); return; }
            try {
                auto bytes = library.cover(id);
                if (!bytes || bytes->empty()) { Reply(response, 404, {{"error", "cover"}}); return; }
                response.set_content(std::move(*bytes), "image/jpeg");
            } catch (...) { Reply(response, 500, {{"error", "cover"}}); }
        });
        server->Get("/api/v1/library/([0-9a-f]{16})/content", [this](const auto& request, auto& response) {
            std::uint64_t id{};
            if (!library.contents || !ParseHex16(request.matches[1].str(), id)) { Reply(response, 404, {{"error", "game"}}); return; }
            try {
                auto list = Json::array();
                for (const auto& item : library.contents(id))
                    list.push_back({{"id", item.id}, {"kind", item.kind}, {"name", item.name}, {"version", item.version},
                        {"title", Hex16(item.title)}, {"bytes", item.bytes}, {"enabled", item.enabled}, {"internal", item.internal}});
                Reply(response, 200, {{"contents", std::move(list)}});
            } catch (...) { Reply(response, 500, {{"error", "library"}}); }
        });
        server->Delete("/api/v1/library/([0-9a-f]{16})", [this](const auto& request, auto& response) {
            RequestRemoval(request.matches[1].str(), {}, response);
        });
        server->Delete("/api/v1/library/([0-9a-f]{16})/content/([0-9a-f]{64})", [this](const auto& request, auto& response) {
            RequestRemoval(request.matches[1].str(), request.matches[2].str(), response);
        });
    }

    /// Queues one deletion. Like a local operation, it closes both uploads until it has run.
    void RequestRemoval(const std::string& game, std::string id, httplib::Response& response) {
        std::uint64_t application{};
        if (!ParseHex16(game, application) || !KnownGame(application)) { Reply(response, 404, {{"error", "game"}}); return; }
        std::lock_guard admission_lock{admission};
        if (!imports_available || RemovalActive() || (system && system->Snapshot().busy) || !BlockUploads()) {
            Reply(response, 409, {{"error", "busy"}}); return;
        }
        if (system) system->SetAvailable(false);
        std::lock_guard lock{mutex};
        status.removal = {.serial = status.removal.serial + 1, .application = application,
                          .content = std::move(id), .phase = "pending"};
        Reply(response, 202, {{"removal", RemovalJson(status.removal)}});
    }

    bool KnownGame(std::uint64_t id) const {
        if (!library.games) return false;
        try {
            const auto games = library.games().second;
            return std::any_of(games.begin(), games.end(), [id](const auto& game) { return game.program_id == id; });
        } catch (...) { return false; }
    }

    /// tus routes for one upload record. `other` is the sibling upload: one file at a time.
    /// A content upload also names its kind and target game in the metadata.
    void UploadRoutes(const std::string& base, NspUpload* target, NspUpload* other) {
        const bool tagged = target && target == content.get();
        // Auth/Host/Origin are checked before these routes. No server-supplied destination path
        // is ever accepted from the browser; ids are matched against the one current record.
        const auto version=[](const auto& request, auto& response) {
            response.set_header("Tus-Resumable", "1.0.0");
            response.set_header("Tus-Version", "1.0.0");
            if (request.get_header_value("Tus-Resumable") == "1.0.0") return true;
            Reply(response, 412, {{"error", "version"}}); return false;
        };
        const auto result=[](auto& response, const UploadResult& value) {
            response.status=value.status;
            if (!value.error.empty()) Reply(response, value.status, {{"error",value.error}});
        };
        server->Options(base, [](const auto&, auto& response) {
            response.status=204;
            response.set_header("Tus-Version", "1.0.0");
            response.set_header("Tus-Extension", "creation,termination");
            response.set_header("Tus-Max-Size",std::to_string(NspUpload::MaxLength));
        });
        server->Post(base, [this,version,result,base,target,other,tagged](const auto& request, auto& response) {
            std::lock_guard admission_lock{admission};
            if((system&&system->Snapshot().busy)||(other&&other->Snapshot().busy)){Reply(response,409,{{"error","busy"}});return;}
            if (!version(request,response)) return;
            if (!target) { Reply(response,503,{{"error","unavailable"}}); return; }
            std::uint64_t length{};
            if (!Unsigned(request.get_header_value("Upload-Length"),length) || request.has_header("Upload-Defer-Length") ||
                (request.has_header("Content-Length") && request.get_header_value("Content-Length") != "0")) { Reply(response,400,{{"error","length"}}); return; }
            if (length > NspUpload::MaxLength) { Reply(response,413,{{"error","length"}}); return; }
            const auto metadata=request.get_header_value("Upload-Metadata");
            std::string name,fingerprint,kind,application;
            std::size_t begin{};
            bool valid=true;
            while (begin < metadata.size()) {
                const auto end=metadata.find(',',begin);
                auto item=metadata.substr(begin,end == std::string::npos ? end : end-begin);
                while (!item.empty() && item.front() == ' ') item.erase(0,1);
                const auto separator=item.find(' ');
                if (separator == std::string::npos) { valid=false; break; }
                std::string decoded;
                if (!Decode64(item.substr(separator+1),decoded)) { valid=false; break; }
                const auto key=item.substr(0,separator);
                if (key == "filename" && name.empty()) name=std::move(decoded);
                else if (key == "fingerprint" && fingerprint.empty()) fingerprint=std::move(decoded);
                else if (tagged && key == "kind" && kind.empty()) kind=std::move(decoded);
                else if (tagged && key == "application" && application.empty()) application=std::move(decoded);
                else { valid=false; break; }
                if (end == std::string::npos) break;
                begin=end+1;
            }
            std::string tag;
            if (valid && tagged) {
                std::uint64_t id{};
                if ((kind != "update" && kind != "dlc") || !ParseHex16(application,id)) valid=false;
                else if (!KnownGame(id)) { Reply(response,404,{{"error","game"}}); return; }
                tag=kind+":"+application;
            }
            if (!valid) { Reply(response,400,{{"error","metadata"}}); return; }
            const auto created=target->Create(std::move(name),std::move(fingerprint),length,std::move(tag));
            result(response,created);
            if (created.status == 201) {
                response.set_header("Location",base+"/"+target->Snapshot().id);
                response.set_header("Upload-Offset","0");
            }
        });
        server->Get(base+"/([0-9a-f]{32})", [target,version](const auto& request, auto& response) {
            // httplib dispatches HEAD through its GET route with the body suppressed.
            if (!version(request,response)) return;
            if (!target) { Reply(response,404,{{"error","upload"}}); return; }
            const auto info=target->Snapshot();
            if (info.id != request.matches[1].str() || info.phase == "complete") { Reply(response,404,{{"error","upload"}}); return; }
            response.set_header("Upload-Offset",std::to_string(info.offset));
            response.set_header("Upload-Length",std::to_string(info.length));
            response.status=200;
        });
        server->Patch(base+"/([0-9a-f]{32})",
            [target,version,result](const httplib::Request& request, httplib::Response& response, const httplib::ContentReader& reader) {
                if (!version(request,response)) return;
                if (!target) { Reply(response,404,{{"error","upload"}}); return; }
                std::uint64_t offset{},length{};
                if (request.get_header_value("Content-Type") != "application/offset+octet-stream") { Reply(response,415,{{"error","type"}}); return; }
                if (!Unsigned(request.get_header_value("Upload-Offset"),offset) ||
                    !Unsigned(request.get_header_value("Content-Length"),length)) { Reply(response,400,{{"error","offset"}}); return; }
                const auto appended=target->Append(request.matches[1].str(),offset,length,
                    [&](const UploadSink& sink) { return reader([&](const char* bytes, std::size_t count) { return sink(bytes,count); }); });
                result(response,appended);
                response.set_header("Upload-Offset",std::to_string(target->Snapshot().offset));
            });
        server->Delete(base+"/([0-9a-f]{32})", [target,version,result](const auto& request, auto& response) {
            if (!version(request,response)) return;
            if (!target) { Reply(response,404,{{"error","upload"}}); return; }
            result(response,target->Discard(request.matches[1].str()));
        });
        server->Post(base+"/([0-9a-f]{32})/(finish|pause|rename)",
            [target,result](const auto& request, auto& response) {
                if (!target) { Reply(response,404,{{"error","upload"}}); return; }
                const auto id=request.matches[1].str(),action=request.matches[2].str();
                if (action == "finish") result(response,target->Ready(id));
                else if (action == "pause") result(response,target->Pause(id));
                else {
                    const auto json=Json::parse(request.body,nullptr,false);
                    if (!json.is_object() || !json.contains("name") || !json["name"].is_string()) { Reply(response,400,{{"error","file"}}); return; }
                    result(response,target->Rename(id,json["name"].template get<std::string>()));
                }
            });
    }
    void SystemRoutes(){
        const auto result=[](auto& response,const UploadResult& value){
            response.status=value.status;if(!value.error.empty()){
                response.set_header("Connection","close");Reply(response,value.status,{{"error",value.error}});
            }
        };
        const auto version=[this](const auto& request,auto& response){
            response.set_header("Tus-Resumable","1.0.0");
            if(!system||!system_supported){Reply(response,503,{{"error","unavailable"}});return false;}
            if(request.get_header_value("Tus-Resumable")!="1.0.0"){Reply(response,412,{{"error","version"}});return false;}
            return true;
        };
        server->Post("/api/v1/system/jobs",[this,result](const auto& request,auto& response){
            std::lock_guard admission_lock{admission};
            if(!system||!system_supported){result(response,{503,"unavailable"});return;}
            const auto body=Json::parse(request.body,nullptr,false);
            if(!body.is_object()||!body.contains("kind")||!body["kind"].is_string()||!body.contains("format")||!body["format"].is_string()||
               !body.contains("count")||!body["count"].is_number_unsigned()||body["count"].template get<std::uint64_t>()>4096){result(response,{400,"file"});return;}
            if(!BlockUploads()){result(response,{409,"busy"});return;}
            const auto created=system->Create(body["kind"].template get<std::string>(),body["format"].template get<std::string>(),body["count"].template get<unsigned>());
            if(created.status==201)Reply(response,201,{{"id",system->Snapshot().id}});
            else {result(response,created);if(!system->Snapshot().busy)RestoreUploads();}
        });
        server->Post("/api/v1/system/jobs/([0-9a-f]{32})/files",[this,result,version](const auto& request,auto& response){
            if(!version(request,response))return;
            const auto body=Json::parse(request.body,nullptr,false);
            if(!body.is_object()||!body.contains("name")||!body["name"].is_string()||!body.contains("fingerprint")||!body["fingerprint"].is_string()||
                !body.contains("length")||!body["length"].is_number_unsigned()){result(response,{400,"file"});return;}
            const auto created=system->BeginFile(request.matches[1].str(),body["name"].template get<std::string>(),body["fingerprint"].template get<std::string>(),body["length"].template get<std::uint64_t>());
            result(response,created);
            if(created.status==201){response.set_header("Location","/api/v1/system/files/"+system->Snapshot().file.id);response.set_header("Upload-Offset","0");}
        });
        server->Get("/api/v1/system/files/([0-9a-f]{32})",[this,result,version](const auto& request,auto& response){
            if(!version(request,response))return;const auto file=system->Snapshot().file;
            if(file.id!=request.matches[1].str()||file.phase=="complete"){result(response,{404,"upload"});return;}
            response.status=200;response.set_header("Upload-Offset",std::to_string(file.offset));response.set_header("Upload-Length",std::to_string(file.length));
        });
        server->Patch("/api/v1/system/files/([0-9a-f]{32})",[this,result,version](const httplib::Request& request,httplib::Response& response,const httplib::ContentReader& reader){
            if(!version(request,response))return;std::uint64_t offset{},length{};
            if(request.get_header_value("Content-Type")!="application/offset+octet-stream"||!Unsigned(request.get_header_value("Upload-Offset"),offset)||
                !Unsigned(request.get_header_value("Content-Length"),length)){result(response,{400,"offset"});return;}
            result(response,system->Append(request.matches[1].str(),offset,length,[&](const UploadSink& sink){return reader([&](const char* data,std::size_t count){return sink(data,count);});}));
            response.set_header("Upload-Offset",std::to_string(system->Snapshot().file.offset));
        });
        server->Post("/api/v1/system/files/([0-9a-f]{32})/finish",[this,result,version](const auto& request,auto& response){
            if(version(request,response))result(response,system->FinishFile(request.matches[1].str()));
        });
        server->Post("/api/v1/system/jobs/([0-9a-f]{32})/(prepare|confirm)",[this,result](const auto& request,auto& response){
            if(!system){result(response,{503,"unavailable"});return;}
            if(request.matches[2].str()=="prepare")result(response,system->Prepare(request.matches[1].str()));
            else {const auto body=Json::parse(request.body,nullptr,false);
                result(response,system->Confirm(request.matches[1].str(),body.is_object()&&body.value("accept_warnings",Json{false})==true));}
        });
        server->Delete("/api/v1/system/jobs/([0-9a-f]{32})",[this,result](const auto& request,auto& response){
            if(!system){result(response,{404,"upload"});return;}result(response,system->Discard(request.matches[1].str()));
        });
    }
};

NetworkPortal::NetworkPortal(std::filesystem::path games,SystemImportBackend backend) : impl{std::make_unique<Impl>()} {
    if (!games.empty()) {
        try {
            impl->system_supported=bool(backend.inspect);
            impl->system=std::make_unique<SystemUpload>(games.parent_path(),std::move(backend));
            const auto transfers=games.parent_path()/"jadyx/transfers/content";
            impl->content=std::make_unique<NspUpload>(games,false,transfers,true);
            impl->upload=std::make_unique<NspUpload>(std::move(games));
        }
        catch (...) { impl->storage_failed=true; }
    }
}
NetworkPortal::~NetworkPortal() { Stop(); }

bool NetworkPortal::Start(const std::string& ipv4, unsigned short port, unsigned spare_ports) {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    impl->Shutdown();
    {
        std::lock_guard lock{impl->mutex};
        impl->status.error = PortalError::None; impl->status.port = 0; impl->status.os_error = 0;
    }
    try {
    if (impl->storage_failed) {
        std::lock_guard lock{impl->mutex}; impl->status.error=PortalError::Storage; return false;
    }
    if (impl->upload) impl->upload->Resume();
    if (impl->content) impl->content->Resume();
    if (impl->system) impl->system->Resume();
    if (impl->logs) impl->logs->Resume();
    if (!LocalIpv4(ipv4) || port == 0) {
        std::lock_guard lock{impl->mutex}; impl->status.error = PortalError::NoAddress; return false;
    }
    auto code = RandomCode();
    if (code.empty()) {
        std::lock_guard lock{impl->mutex}; impl->status.error = PortalError::Random; return false;
    }
    // A taken or refused port moves on to the next one; a lost address or anything else stops.
    unsigned short bound = 0;
    PortalError failure = PortalError::Bind;
    int os_error = 0;
    for (unsigned i = 0; i <= spare_ports && port + i <= 0xFFFF; ++i) {
        const auto candidate = static_cast<unsigned short>(port + i);
        impl->server = std::make_unique<PortalServer>();
        impl->exclusive_socket = false;
        impl->authority = ipv4 + ":" + std::to_string(candidate);
        impl->Routes();
        if (impl->server->bind_to_port(ipv4, candidate) && impl->exclusive_socket) { bound = candidate; break; }
        const bool exclusive = impl->exclusive_socket;
        impl->server.reset();
        const int error = BindError(ipv4, candidate);
        const auto reason = exclusive ? BindFailure(error) : PortalError::Bind;
        // The user is told about the usual port; the spare ones only count if it was refused.
        if (i == 0) { failure = reason; os_error = error; }
        if (reason != PortalError::PortInUse && reason != PortalError::PortBlocked) break;
    }
    if (!bound) {
        std::lock_guard lock{impl->mutex};
        impl->status.error = failure; impl->status.port = port; impl->status.os_error = os_error;
        return false;
    }
    {
        std::lock_guard lock{impl->mutex};
        impl->status = {.running = true, .url = "http://" + impl->authority, .code = std::move(code), .port = bound};
        impl->expires = Clock::now() + std::chrono::minutes{5};
        impl->attempts_window = Clock::now(); impl->attempts = 0;
    }
    // Listener, handlers and their state outlive every request. Stop joins before any reset.
    impl->listener = std::jthread([this] {
        bool ok = false;
        try { ok = impl->server->listen_after_bind(); } catch (...) {}
        // Also release Start's ready wait if listener/thread-pool initialization failed.
        impl->server->decommission();
        std::lock_guard lock{impl->mutex};
        if (!ok && impl->status.running) impl->status.error = PortalError::Listen;
        impl->status.running = false;
    });
    impl->server->wait_until_ready();
    return Status().running;
    } catch (...) {
        impl->Shutdown();
        std::lock_guard lock{impl->mutex};
        impl->status.error = PortalError::Listen;
        return false;
    }
}

void NetworkPortal::Stop() {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    impl->Shutdown();
}

bool NetworkPortal::RenewCode() {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    auto code = RandomCode();
    std::lock_guard lock{impl->mutex};
    if (!impl->status.running || code.empty()) return false;
    impl->status.code = std::move(code);
    impl->expires = Clock::now() + std::chrono::minutes{5};
    // Do not reset the attempt limiter: renewing must not defeat brute-force protection.
    return true;
}

PortalStatus NetworkPortal::Status() const {
    PortalStatus result;
    {
        std::lock_guard lock{impl->mutex};
        result=impl->status;
        result.code_expired=Clock::now() >= impl->expires;
    }
    if (impl->upload) result.transfer=impl->upload->Snapshot();
    if (impl->content) result.content=impl->content->Snapshot();
    if (impl->system) result.system=impl->system->Snapshot();
    return result;
}

void NetworkPortal::SetUpdateOffered(bool offered) { impl->update_offered = offered; }
void NetworkPortal::SetUpdatePackage(std::function<std::optional<Updates::UpdatePackage>()> package) {
    std::lock_guard lock{impl->mutex}; impl->update_package = std::move(package);
}
void NetworkPortal::SetLogDirectory(std::filesystem::path directory) {
    std::lock_guard lock{impl->lifecycle};
    if (impl->server) throw std::logic_error{"SetLogDirectory must precede Start"};
    impl->logs = std::make_unique<LogExport>(std::move(directory));
}
void NetworkPortal::SetLibrary(PortalLibrary library) {
    std::lock_guard lock{impl->lifecycle};
    if (impl->server) throw std::logic_error{"SetLibrary must precede Start"};
    impl->library = std::move(library);
}
void NetworkPortal::SetUploadsAvailable(bool enabled) {
    std::lock_guard lock{impl->admission};impl->imports_available=enabled;
    const bool removing=impl->RemovalActive();
    if(impl->system)impl->system->SetAvailable(enabled&&!removing);
    const bool idle=(!impl->system||!impl->system->Snapshot().busy)&&!removing;
    if(impl->upload)impl->upload->SetAvailable(enabled&&idle);
    if(impl->content)impl->content->SetAvailable(enabled&&idle);
}
bool NetworkPortal::BlockForLocalOperation() {
    std::lock_guard lock{impl->admission};if(impl->RemovalActive()||(impl->system&&impl->system->Snapshot().busy))return false;
    if(!impl->BlockUploads())return false;
    if(impl->system)impl->system->SetAvailable(false);return true;
}
std::string NetworkPortal::FinalizeGame(const NspValidator& validate) {
    if (!impl->upload) return "unavailable";
    const auto error=impl->upload->Finalize(validate);
    if (error == "interrupted" && Status().running) impl->upload->Resume();
    return error;
}
std::string NetworkPortal::FinalizeContent(const ContentValidator& validate) {
    if (!impl->content) return "unavailable";
    const auto tag=impl->content->Snapshot().tag;
    const auto error=impl->content->Finalize([&](const std::filesystem::path& path, std::stop_token stop) {
        return validate(path, tag, stop);
    });
    if (error == "interrupted" && Status().running) impl->content->Resume();
    return error;
}
std::string NetworkPortal::RunRemoval(const RemovalRunner& run) {
    PortalRemoval job;
    {
        std::lock_guard lock{impl->mutex};
        if (impl->status.removal.phase != "pending") return "unavailable";
        impl->status.removal.phase = "running";
        job = impl->status.removal;
    }
    std::string error;
    try { error = run(job); } catch (...) { error = "disk"; }
    {
        std::lock_guard lock{impl->mutex};
        impl->status.removal.phase = error.empty() ? "complete" : "failed";
        impl->status.removal.error = error;
    }
    std::lock_guard lock{impl->admission};
    impl->RestoreUploads();
    if (impl->system) impl->system->SetAvailable(impl->imports_available);
    return error;
}
void NetworkPortal::CancelTransfer() { if (impl->upload) impl->upload->Cancel();if(impl->content)impl->content->Cancel();if(impl->system)impl->system->Cancel(); }
std::string NetworkPortal::RunSystemImport(std::stop_token stop){return impl->system?impl->system->RunPending(stop):"unavailable";}

} // namespace Jadyx
