// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/acc/nextendo_acc.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/hle_ipc.h"
#include "core/online/nextendo/config.h"
#include "core/online/nextendo/id_token.h"

namespace Service::Account::NextendoAcc {
namespace {

namespace Nx = Core::Online::Nextendo;

constexpr const char* KeyId = "jadyx-baas-key-1";
constexpr const char* KeyFile = "nextendo_baas.pem";
constexpr auto TokenReuse = std::chrono::hours{2}; // the token itself lives three hours

// The signing key stays the same across launches so the identity the servers see is stable.
// It only signs tokens for the redirected account host; it is not a secret of the user.
Nx::RsaSigner* Signer() {
    static const std::unique_ptr<Nx::RsaSigner> signer = [] {
        const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir) / KeyFile;
        const std::string pem = Common::FS::ReadStringFromFile(path, Common::FS::FileType::TextFile);
        if (!pem.empty()) {
            if (auto loaded = Nx::RsaSigner::FromPem(KeyId, pem)) {
                return loaded;
            }
            LOG_WARNING(Service_ACC, "[Nextendo] stored signing key unreadable, making a new one");
        }
        auto generated = Nx::RsaSigner::Generate(KeyId);
        if (generated) {
            void(Common::FS::CreateParentDirs(path));
            if (Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile,
                                              generated->PrivatePem()) == 0) {
                LOG_WARNING(Service_ACC, "[Nextendo] could not keep the signing key");
            }
        } else {
            LOG_ERROR(Service_ACC, "[Nextendo] could not make a signing key");
        }
        return generated;
    }();
    return signer.get();
}

std::string InstalledVersion(Core::System& system, u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    const FileSys::PatchManager patches{program_id, system.GetFileSystemController(),
                                        system.GetContentProvider()};
    const auto metadata = patches.GetControlMetadata();
    return metadata.first != nullptr ? metadata.first->GetVersionString() : std::string{};
}

struct Cache {
    std::mutex mutex;
    std::vector<u8> token;
    std::chrono::steady_clock::time_point expires;
    u64 program_id = 0;
    std::string version;
    std::string nex_token;
};

Cache& TokenCache() {
    static Cache cache;
    return cache;
}

} // namespace

std::optional<u64> AccountId() {
    const auto config = Nx::GlobalConfig().Snapshot();
    if (!config->enabled || config->pid == 0) {
        return std::nullopt;
    }
    return config->pid;
}

bool Licensed() {
    const auto config = Nx::GlobalConfig().Snapshot();
    return config->enabled && config->pid != 0;
}

std::optional<std::vector<u8>> IdToken(Core::System& system) {
    const auto config = Nx::GlobalConfig().Snapshot();
    if (!config->enabled) {
        return std::nullopt;
    }
    const u64 program_id = system.GetApplicationProcessProgramID();
    const std::string version = InstalledVersion(system, program_id);
    const auto now = std::chrono::steady_clock::now();

    auto& cache = TokenCache();
    std::scoped_lock lock{cache.mutex};
    if (!cache.token.empty() && now < cache.expires && cache.program_id == program_id &&
        cache.version == version && cache.nex_token == config->nex_token) {
        return cache.token;
    }
    Nx::RsaSigner* signer = Signer();
    if (signer == nullptr) {
        return std::nullopt;
    }
    const auto issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto token = Nx::BuildIdToken({.title_id = program_id,
                                         .title_version = version,
                                         .nex_token = config->nex_token,
                                         .issued_at = issued_at},
                                        *signer);
    if (!token) {
        LOG_ERROR(Service_ACC, "[Nextendo] could not sign the id_token");
        return std::nullopt;
    }
    cache.token.assign(token->begin(), token->end());
    cache.expires = now + TokenReuse;
    cache.program_id = program_id;
    cache.version = version;
    cache.nex_token = config->nex_token;
    // Length only: the token carries the game token and must never reach the log.
    LOG_INFO(Service_ACC, "[Nextendo] id_token issued ({} bytes, game token {})", cache.token.size(),
             config->nex_token.empty() ? "missing" : "present");
    return cache.token;
}

u32 WriteIdToken(HLERequestContext& ctx, const std::vector<u8>& token) {
    const std::size_t capacity = ctx.GetWriteBufferSize();
    if (capacity == 0) {
        return 0;
    }
    if (token.size() >= capacity) {
        LOG_WARNING(Service_ACC, "[Nextendo] id_token ({} bytes) cut to the guest buffer ({})",
                    token.size(), capacity);
    }
    std::vector<u8> out(capacity, 0);
    const std::size_t length = std::min(token.size(), capacity - 1);
    std::copy_n(token.begin(), length, out.begin());
    ctx.WriteBuffer(out);
    return static_cast<u32>(length);
}

} // namespace Service::Account::NextendoAcc
