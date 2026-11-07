// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The player's own amiibo files and optional key_retail.bin, as uploaded from Jadyx Portal. Files
// are checked before they are kept, so a wrong file is refused at upload instead of showing up as
// a damaged amiibo inside a game. Jadyx ships no keys or figures.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx {

struct AmiiboFile {
    std::string name; ///< UTF-8 file name, without folders.
    std::uint64_t bytes{};
    bool operator==(const AmiiboFile&) const = default;
};

/// The file name inside an uploaded name, or empty when it cannot be a plain .bin file on the
/// console: reserved characters or device names, a trailing dot or space, too long.
[[nodiscard]] std::string AmiiboFileName(std::string_view uploaded);
/// A 532, 540 or 572 byte tag dump, encrypted or decrypted.
[[nodiscard]] bool IsAmiiboImage(std::string_view bytes);
/// A 160 byte key_retail.bin with its two keys in the order the crypto code reads them.
[[nodiscard]] bool IsAmiiboKey(std::string_view bytes);

class AmiiboStore {
public:
    /// The folders the emulated core reads from.
    AmiiboStore();
    AmiiboStore(std::filesystem::path files, std::filesystem::path key);

    /// By name, ignoring case. Only .bin files.
    [[nodiscard]] std::vector<AmiiboFile> List() const;
    /// Returns an error word, empty on success: "name", "not_amiibo", "exists" (unless `replace`)
    /// or "disk". Replacing overwrites the data games saved in the file, so callers ask first.
    [[nodiscard]] std::string Add(std::string_view name, std::string_view bytes, bool replace);
    /// Returns an error word, empty on success: "name", "missing" or "disk".
    [[nodiscard]] std::string Remove(std::string_view name);
    [[nodiscard]] bool HasKey() const;
    /// Returns an error word, empty on success: "not_key" or "disk".
    [[nodiscard]] std::string SetKey(std::string_view bytes);
    /// Returns an error word, empty on success: "disk".
    [[nodiscard]] std::string RemoveKey();
    /// UTF-8 path of a stored file, as VirtualAmiibo opens it; empty for an invalid name.
    [[nodiscard]] std::string PathOf(std::string_view name) const;

private:
    std::filesystem::path files;
    std::filesystem::path key;
    mutable std::mutex mutex;
};

} // namespace Jadyx
