// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/network_import_gate.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "common/httplib.h"
#include <nlohmann/json.hpp>
#include "jadyx/storage/network_portal.h"
#include "jadyx/storage/nsp_import.h"

namespace Jadyx {
bool RunNetworkImportGate(const std::filesystem::path& local,
                          const std::function<void(std::string)>& diagnostic) {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    const auto require = [](bool ok, const std::string& message) {
        if (!ok) throw std::runtime_error(message);
    };
    const auto status = [](const httplib::Result& result) { return result ? result->status : 0; };
    fs::path fixture;
    try {
        // Check every real internal NSP with precisely the production validator before copying.
        fs::path source;
        std::uint64_t total{};
        unsigned checked{};
        for (const auto& item : fs::directory_iterator{local / "games"}) {
            if (!item.is_regular_file() || item.path().extension() != ".nsp") continue;
            const auto result = ValidateUploadedNsp(item.path(), {});
            diagnostic("NSP gate: source " + std::to_string(++checked) + " bytes=" +
                       std::to_string(item.file_size()) + " validation=" + (result.empty() ? "ok" : result));
            const auto length = item.file_size();
            if (result.empty() && length > (4ULL << 30) && (source.empty() || length < total)) {
                source = item.path(); total = length;
            }
        }
        require(!source.empty(), "no valid base NSP above 4 GiB in LocalState/games");
        const auto original_time = fs::last_write_time(source);
        fixture = local / ("network-import-gate-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(fixture), "create private fixture");
        const auto games = fixture / "games";
        constexpr unsigned short port = 18080;
        const std::string host = "127.0.0.1";
        const auto origin = "http://" + host + ":" + std::to_string(port);
        auto portal = std::make_unique<NetworkPortal>(games);
        std::unique_ptr<httplib::Client> client;
        httplib::Headers headers;
        const auto connect = [&] {
            require(portal->Start(host, port), "bind diagnostic loopback port 18080");
            portal->SetUploadsAvailable(true);
            client = std::make_unique<httplib::Client>(host, port);
            client->set_keep_alive(true);
            client->set_read_timeout(60);
            const auto paired = client->Post("/api/v1/pair", {{"Origin", origin}},
                Json{{"code", portal->Status().code}}.dump(), "application/json");
            require(status(paired) == 200, "AppContainer loopback pairing HTTP=" + std::to_string(status(paired)));
            // Pairing secrets remain only in these transient objects, never diagnostics.
            headers = {{"Authorization", "Bearer " + Json::parse(paired->body).at("session").get<std::string>()},
                       {"Origin", origin}, {"Tus-Resumable", "1.0.0"}};
        };
        connect();
        auto creation = headers;
        creation.emplace("Upload-Length", std::to_string(total));
        creation.emplace("Upload-Metadata", "filename " + httplib::detail::base64_encode("real-nsp-gate.nsp") +
            ",fingerprint " + httplib::detail::base64_encode(std::string(64, 'a')));
        const auto created = client->Post("/api/v1/uploads", creation);
        require(status(created) == 201, "create upload HTTP=" + std::to_string(status(created)));
        const auto url = created->get_header_value("Location");
        std::ifstream input{source, std::ios::binary};
        require(input.good(), "open source read-only");
        std::vector<char> buffer(1 << 20);
        bool restarted = false;
        unsigned progress{};
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t offset = 0; offset < total;) {
            const auto count = (std::min)(total - offset, 16ULL << 20);
            auto patch = headers;
            patch.emplace("Upload-Offset", std::to_string(offset));
            const auto result = client->Patch(url, patch, static_cast<size_t>(count),
                [&](size_t position, size_t length, httplib::DataSink& sink) {
                    input.clear();
                    input.seekg(static_cast<std::streamoff>(offset + position));
                    while (length) {
                        const auto n = (std::min)(length, buffer.size());
                        input.read(buffer.data(), static_cast<std::streamsize>(n));
                        if (input.gcount() != static_cast<std::streamsize>(n) || !sink.write(buffer.data(), n)) return false;
                        length -= n;
                    }
                    return true;
                }, "application/offset+octet-stream");
            require(status(result) == 204, "PATCH HTTP=" + std::to_string(status(result)));
            offset += count;
            require(result->get_header_value("Upload-Offset") == std::to_string(offset), "confirmed offset");
            if (offset * 10 / total > progress) {
                progress = static_cast<unsigned>(offset * 10 / total);
                diagnostic("NSP gate: confirmed=" + std::to_string(offset) + "/" + std::to_string(total));
            }
            if (!restarted && offset > (4ULL << 30)) {
                client.reset(); portal->Stop(); portal.reset();
                portal = std::make_unique<NetworkPortal>(games);
                connect();
                const auto head = client->Head(url, headers);
                require(status(head) == 200 && head->get_header_value("Upload-Offset") == std::to_string(offset),
                        "durable resume above 4 GiB");
                restarted = true;
                diagnostic("NSP gate: new server instance recovered offset=" + std::to_string(offset));
            }
        }
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        require(status(client->Post(url + "/finish", headers)) == 204, "finish HTTP");
        const auto validation = portal->FinalizeGame(ValidateUploadedNsp);
        require(validation.empty(), "production validation/publication: " + validation);
        require(portal->Status().transfer.phase == "complete", "published phase");
        portal->Stop(); client.reset(); portal.reset();
        const auto installed = games / "real-nsp-gate.nsp";
        require(fs::file_size(installed) == total, "published size");
        std::ifstream copy{installed, std::ios::binary};
        input.clear(); input.seekg(0);
        std::vector<char> compare(1 << 20);
        for (std::uint64_t offset = 0; offset < total;) {
            const auto n = static_cast<size_t>((std::min)(total - offset, std::uint64_t{buffer.size()}));
            input.read(buffer.data(), n); copy.read(compare.data(), n);
            require(input.gcount() == static_cast<std::streamsize>(n) &&
                    copy.gcount() == static_cast<std::streamsize>(n) &&
                    std::equal(buffer.begin(), buffer.begin() + n, compare.begin()), "full byte comparison");
            offset += n;
        }
        input.close(); copy.close();
        require(fs::file_size(source) == total && fs::last_write_time(source) == original_time, "source unchanged");
        require(fixture.parent_path() == local && fixture.filename().wstring().starts_with(L"network-import-gate-"), "confined cleanup");
        fs::remove_all(fixture);
        diagnostic("NSP gate PASS: bytes=" + std::to_string(total) + " chunks=16MiB receive_seconds=" +
            std::to_string(seconds) + " receive_MiBs=" + std::to_string(total / 1048576.0 / seconds) +
            " restart_above_4GiB=1 production_validator=ok byte_compare=equal original=unchanged; loopback only, not Series LAN performance");
        return true;
    } catch (const std::exception& error) {
        diagnostic(std::string{"NSP gate FAIL: "} + error.what());
        // Retain failed fixtures for inspection, never remove any source data.
        return false;
    }
}
}
