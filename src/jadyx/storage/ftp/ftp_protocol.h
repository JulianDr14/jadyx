// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from NXbox src/eden_uwp/ftp (GPL-3.0-or-later).
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Ftp {
struct Command {
    std::string verb;
    std::string argument;
};
inline Command ParseCommand(std::string_view line) {
    if (line.ends_with("\r\n"))
        line.remove_suffix(2);
    const auto split = line.find(' ');
    Command result;
    result.verb = line.substr(0, split);
    for (char& letter : result.verb)
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');
    if (split != line.npos)
        result.argument = line.substr(split + 1);
    return result;
}

// Paths are FTP-root-relative. Reject Windows aliases, streams and traversal above root.
inline std::optional<std::string> NormalizePath(std::string_view cwd, std::string_view input) {
    std::vector<std::string> parts;
    auto consume = [&](std::string_view path) {
        while (!path.empty()) {
            const auto end = path.find_first_of("/\\");
            const auto part = path.substr(0, end);
            if (part == "..") {
                if (parts.empty())
                    return false;
                parts.pop_back();
            } else if (!part.empty() && part != ".") {
                if (part.back() == '.' || part.back() == ' ' ||
                    part.find_first_of(":*?\"<>|") != part.npos ||
                    std::any_of(part.begin(), part.end(),
                                [](unsigned char byte) { return byte < 32; }))
                    return false;
                std::string upper(part);
                for (char& letter : upper)
                    if (letter >= 'a' && letter <= 'z')
                        letter = static_cast<char>(letter - 'a' + 'A');
                const auto base = upper.substr(0, upper.find('.'));
                if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                    (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                     base[3] >= '0' && base[3] <= '9') ||
                    upper.ends_with(".PART"))
                    return false;
                parts.emplace_back(part);
            }
            if (end == path.npos)
                break;
            path.remove_prefix(end + 1);
        }
        return true;
    };
    if ((input.empty() || (input.front() != '/' && input.front() != '\\')) && !consume(cwd))
        return std::nullopt;
    if (!consume(input))
        return std::nullopt;
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty())
            result += '/';
        result += part;
    }
    return result;
}
inline std::optional<std::array<unsigned, 4>> PrivateIPv4(std::string_view address) {
    std::array<unsigned, 4> octets{};
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const auto end = address.find('.');
        const auto field = address.substr(0, end);
        const auto parsed =
            std::from_chars(field.data(), field.data() + field.size(), octets[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
            octets[index] > 255 || (index == 3) != (end == address.npos))
            return std::nullopt;
        if (end != address.npos)
            address.remove_prefix(end + 1);
    }
    if (octets[0] == 10 || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] == 192 && octets[1] == 168))
        return octets;
    return std::nullopt;
}
inline std::string PasvReply(const std::array<unsigned, 4>& ip, unsigned port) {
    return "227 Entering Passive Mode (" + std::to_string(ip[0]) + "," + std::to_string(ip[1]) +
           "," + std::to_string(ip[2]) + "," + std::to_string(ip[3]) + "," +
           std::to_string(port / 256) + "," + std::to_string(port % 256) + ").\r\n";
}
inline std::string EpsvReply(unsigned port) {
    return "229 Entering Extended Passive Mode (|||" + std::to_string(port) + "|).\r\n";
}
inline std::string Timestamp(const std::tm& utc) {
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &utc);
    return buffer;
}
inline std::string ListLine(std::string_view name, bool directory, std::uint64_t size,
                            const std::tm& utc) {
    constexpr std::array<const char*, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[40]{};
    std::snprintf(date, sizeof(date), "%s %2d %04d",
                  months.at(static_cast<std::size_t>(utc.tm_mon)), utc.tm_mday, utc.tm_year + 1900);
    return std::string(directory ? "drwxr-xr-x" : "-rw-r--r--") + " 1 jadyx jadyx " +
           std::to_string(size) + " " + date + " " + std::string(name) + "\r\n";
}
inline bool InGames(std::string_view path) {
    std::string folded(path);
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char letter) {
        return static_cast<char>(letter >= 'A' && letter <= 'Z' ? letter + ('a' - 'A') : letter);
    });
    return folded == "games" || folded.starts_with("games/");
}
} // namespace Jadyx::Ftp
