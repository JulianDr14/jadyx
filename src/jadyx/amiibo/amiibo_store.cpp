// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/amiibo/amiibo_store.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>
#include <utility>

#include "common/fs/path_util.h"
#include "core/hle/service/nfc/common/amiibo_crypto.h"
#include "jadyx/storage/atomic_file.h"

namespace Jadyx {

namespace {

namespace AmiiboCrypto = Service::NFP::AmiiboCrypto;
namespace fs = std::filesystem;

constexpr std::size_t MaxNameBytes = 100;
constexpr std::size_t FullImage = sizeof(Service::NFP::EncryptedNTAG215File);
static_assert(FullImage == 540);

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(std::string_view text) {
    std::string lower{text};
    std::ranges::transform(lower, lower.begin(), Lower);
    return lower;
}

bool IsDeviceName(std::string_view name) {
    const auto stem = LowerAscii(name.substr(0, name.find('.')));
    for (const auto* device : {"con", "prn", "aux", "nul"}) {
        if (stem == device) return true;
    }
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem[3] >= '0' && stem[3] <= '9';
}

/// A path from a validated name, or empty when the bytes are not valid UTF-8.
fs::path Child(const fs::path& folder, std::string_view name) {
    try {
        const std::u8string text{reinterpret_cast<const char8_t*>(name.data()), name.size()};
        return folder / fs::path{text};
    } catch (...) {
        return {};
    }
}

bool KeyPartIs(const AmiiboCrypto::InternalKey& key, std::string_view type) {
    // Both the name and its padding: a file of another format would not have them by chance.
    std::array<char, 0xE> expected{};
    std::ranges::copy(type, expected.begin());
    return key.type_string == expected && key.magic_length <= key.magic_bytes.size();
}

} // namespace

std::string AmiiboFileName(std::string_view uploaded) {
    if (const auto slash = uploaded.find_last_of("/\\"); slash != std::string_view::npos) {
        uploaded.remove_prefix(slash + 1);
    }
    if (uploaded.empty() || uploaded.size() > MaxNameBytes || uploaded.back() == ' ' ||
        uploaded.back() == '.' || !LowerAscii(uploaded).ends_with(".bin") || uploaded.size() == 4) {
        return {};
    }
    for (const char c : uploaded) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view{":*?\"<>|"}.find(c) != std::string_view::npos) {
            return {};
        }
    }
    if (IsDeviceName(uploaded)) return {};
    return std::string{uploaded};
}

bool IsAmiiboImage(std::string_view bytes) {
    if (bytes.size() != FullImage - 8 && bytes.size() != FullImage && bytes.size() != FullImage + 0x20) {
        return false;
    }
    // Like VirtualAmiibo: a dump without the password is padded, the signature is not read.
    std::array<char, FullImage> image{};
    std::memcpy(image.data(), bytes.data(), (std::min)(bytes.size(), image.size()));
    Service::NFP::EncryptedNTAG215File encrypted;
    Service::NFP::NTAG215File plain;
    std::memcpy(&encrypted, image.data(), image.size());
    std::memcpy(&plain, image.data(), image.size());
    return AmiiboCrypto::IsAmiiboValid(encrypted) || AmiiboCrypto::IsAmiiboValid(plain);
}

bool IsAmiiboKey(std::string_view bytes) {
    if (bytes.size() != 2 * sizeof(AmiiboCrypto::InternalKey)) return false;
    AmiiboCrypto::InternalKey unfixed_info, locked_secret;
    std::memcpy(&unfixed_info, bytes.data(), sizeof(unfixed_info));
    std::memcpy(&locked_secret, bytes.data() + sizeof(unfixed_info), sizeof(locked_secret));
    return KeyPartIs(unfixed_info, "unfixed infos") && KeyPartIs(locked_secret, "locked secret");
}

AmiiboStore::AmiiboStore()
    : AmiiboStore{Common::FS::GetEdenPath(Common::FS::EdenPath::AmiiboDir) / "files",
                  Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir) / "key_retail.bin"} {}

AmiiboStore::AmiiboStore(fs::path files_, fs::path key_)
    : files{std::move(files_)}, key{std::move(key_)} {}

std::vector<AmiiboFile> AmiiboStore::List() const {
    const std::scoped_lock lock{mutex};
    std::vector<AmiiboFile> list;
    std::error_code ec;
    for (fs::directory_iterator it{files, ec}, end; !ec && it != end; it.increment(ec)) {
        try {
            const auto text = it->path().filename().u8string();
            std::string name{reinterpret_cast<const char*>(text.data()), text.size()};
            std::error_code size_error;
            const auto bytes = it->file_size(size_error);
            if (!size_error && it->is_regular_file(size_error) && AmiiboFileName(name) == name) {
                list.push_back({std::move(name), bytes});
            }
        } catch (...) {
            // A name that isn't valid Unicode was not stored by us; leave it out.
        }
    }
    std::ranges::sort(list, {}, [](const AmiiboFile& file) { return LowerAscii(file.name); });
    return list;
}

std::string AmiiboStore::Add(std::string_view uploaded, std::string_view bytes, bool replace) {
    const auto name = AmiiboFileName(uploaded);
    const auto path = name.empty() ? fs::path{} : Child(files, name);
    if (path.empty()) return "name";
    if (!IsAmiiboImage(bytes)) return "not_amiibo";
    const std::scoped_lock lock{mutex};
    std::error_code ec;
    if (!replace && fs::exists(path, ec)) return "exists";
    return WriteFileAtomically(path, bytes) ? "" : "disk";
}

std::string AmiiboStore::Remove(std::string_view uploaded) {
    const auto name = AmiiboFileName(uploaded);
    const auto path = name.empty() ? fs::path{} : Child(files, name);
    if (path.empty()) return "name";
    const std::scoped_lock lock{mutex};
    std::error_code ec;
    const bool removed = fs::remove(path, ec);
    return ec ? "disk" : removed ? "" : "missing";
}

bool AmiiboStore::HasKey() const {
    const std::scoped_lock lock{mutex};
    std::error_code ec;
    return fs::is_regular_file(key, ec);
}

std::string AmiiboStore::SetKey(std::string_view bytes) {
    if (!IsAmiiboKey(bytes)) return "not_key";
    const std::scoped_lock lock{mutex};
    return WriteFileAtomically(key, bytes) ? "" : "disk";
}

std::string AmiiboStore::RemoveKey() {
    const std::scoped_lock lock{mutex};
    std::error_code ec;
    fs::remove(key, ec);
    return ec ? "disk" : "";
}

std::string AmiiboStore::PathOf(std::string_view uploaded) const {
    const auto name = AmiiboFileName(uploaded);
    const auto path = name.empty() ? fs::path{} : Child(files, name);
    if (path.empty()) return {};
    try {
        const auto text = path.u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    } catch (...) {
        return {};
    }
}

} // namespace Jadyx
