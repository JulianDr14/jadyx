// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <istream>
#include <string>
#include <string_view>

namespace Jadyx::Ftp {
// The caller supplies only the installed package's boot.cfg, never user storage.
inline std::string ReadTestPassword(std::istream& input) {
    constexpr std::string_view key = "ftp_test_password=";
    std::string password;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.starts_with(key))
            continue;
        const auto value = std::string_view{line}.substr(key.size());
        if (value.size() >= 12 && std::none_of(value.begin(), value.end(), [](unsigned char byte) {
                return byte < 0x20 || byte == 0x7f;
            }))
            password = value;
    }
    return password;
}
} // namespace Jadyx::Ftp
