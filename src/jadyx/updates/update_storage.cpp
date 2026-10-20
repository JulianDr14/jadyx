// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <fileapifromapp.h>

#include <chrono>
#include <fstream>
#include <thread>
#include "jadyx/updates/update_storage.h"
namespace Jadyx::Updates::Storage {
std::optional<std::string> ReadBounded(const std::filesystem::path& path, std::size_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit)
        return {};
    std::ifstream input{path, std::ios::binary};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input || !input.read(bytes.data(), bytes.size()) ||
        input.peek() != std::char_traits<char>::eof())
        return {};
    return bytes;
}
// Unlike the older frontend helper, preserve the original replacement error on cleanup failure.
bool Persist(const std::filesystem::path& path, std::string_view body) {
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(body.data(), body.size()).flush())
            return false;
    }
    DWORD failure{};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool saved = exists ? ReplaceFileFromAppW(path.c_str(), temporary.c_str(), nullptr, 0,
                                                        nullptr, nullptr) != 0
                                  : MoveFileFromAppW(temporary.c_str(), path.c_str()) != 0;
        if (saved)
            return true;
        failure = GetLastError();
        // Antivirus/indexers can briefly hold the old file without delete sharing. Keep the old
        // journal and retry only these transient replacement failures (at most 280 ms).
        if (failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION &&
            failure != ERROR_ACCESS_DENIED && failure != ERROR_UNABLE_TO_REMOVE_REPLACED &&
            failure != ERROR_UNABLE_TO_MOVE_REPLACEMENT)
            break;
        if (attempt < 7)
            std::this_thread::sleep_for(std::chrono::milliseconds{10 * (attempt + 1)});
    }
    SetLastError(failure);
    return false;
}
} // namespace Jadyx::Updates::Storage
