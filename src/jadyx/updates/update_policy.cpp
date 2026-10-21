// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include "jadyx/updates/update_policy.h"
namespace Jadyx::Updates {
std::optional<Version> ParseVersion(std::string_view text) {
    Version version{};
    for (unsigned i = 0; i < 4; ++i) {
        auto dot = text.find('.');
        auto part = text.substr(0, dot);
        unsigned value{};
        if (part.empty() || (part.size() > 1 && part.front() == '0'))
            return {};
        auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || end != part.data() + part.size() || value > 65535)
            return {};
        version[i] = static_cast<std::uint16_t>(value);
        if (i == 3) {
            if (dot != text.npos)
                return {};
        } else {
            if (dot == text.npos)
                return {};
            text.remove_prefix(dot + 1);
        }
    }
    return version;
}
std::string FormatVersion(const Version& v) {
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." +
           std::to_string(v[3]);
}
std::string ReleaseNotesText(std::string_view markdown) {
    const auto inline_text = [](std::string_view text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text.substr(i, 2) == "**" || text.substr(i, 2) == "__") {
                ++i;
                continue;
            }
            if (text[i] == '`')
                continue;
            // [label](target) keeps the label; a console cannot follow the link.
            if (text[i] == '[') {
                const auto close = text.find("](", i);
                const auto end = close == text.npos ? text.npos : text.find(')', close);
                if (end != text.npos && text.find('\n', i) > end) {
                    out += text.substr(i + 1, close - i - 1);
                    i = end;
                    continue;
                }
            }
            out += text[i];
        }
        return out;
    };
    std::vector<std::string> lines;
    bool joinable = false; // The previous line is a bullet or paragraph a wrapped line continues.
    while (!markdown.empty()) {
        const auto newline = markdown.find('\n');
        auto line = markdown.substr(0, newline);
        markdown.remove_prefix(newline == markdown.npos ? markdown.size() : newline + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto start = line.find_first_not_of(" \t");
        if (start == line.npos) {
            if (!lines.empty() && !lines.back().empty())
                lines.emplace_back();
            joinable = false;
            continue;
        }
        line.remove_prefix(start);
        if (line.front() == '#') {
            line.remove_prefix(std::min(line.find_first_not_of('#'), line.size()));
            line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
            lines.push_back(inline_text(line));
            joinable = false;
        } else if (line.starts_with("- ") || line.starts_with("* ")) {
            lines.push_back("\xE2\x80\xA2 " + inline_text(line.substr(2)));
            joinable = true;
        } else if (joinable) {
            lines.back() += " " + inline_text(line);
        } else {
            lines.push_back(inline_text(line));
            joinable = true;
        }
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    std::string out;
    for (const auto& line : lines)
        out += (out.empty() ? "" : "\n") + line;
    if (out.size() > 16384) {
        out.resize(16384);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0)
            out.pop_back();
    }
    return out;
}
std::optional<Release> VerifyManifest(std::string_view body, std::string_view signature,
                                      std::string_view public_pem, const Version& installed,
                                      Channel channel) {
    if (body.empty() || body.size() > 65536 || signature.size() > 8192 || public_pem.empty())
        return {};
    std::unique_ptr<BIO, decltype(&BIO_free)> bio{
        BIO_new_mem_buf(public_pem.data(), static_cast<int>(public_pem.size())), BIO_free};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
        bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr, EVP_PKEY_free};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free};
    if (!key || !context || EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA ||
        EVP_PKEY_bits(key.get()) < 2048 ||
        EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()),
                         signature.size(), reinterpret_cast<const unsigned char*>(body.data()),
                         body.size()) != 1)
        return {};
    try {
        const auto j = nlohmann::json::parse(body);
        auto version = ParseVersion(j.at("version").get<std::string>());
        const std::string ch = j.at("channel");
        if (!j.at("schema").is_number_integer() || j.at("schema") != 1 ||
            !j.at("size").is_number_unsigned() || !version || *version <= installed ||
            (ch != "stable" && ch != "preview") || (channel == Channel::Stable && ch != "stable") ||
            j.at("identity").at("name") != "Jadyx" ||
            j.at("identity").at("publisher") != "CN=JadyxDev" ||
            j.at("identity").at("architecture") != "x64" || j.at("diagnostic") != false ||
            j.at("min_os") != "10.0.17763.0" || j.at("vclibs_min") != "14.0.33519.0")
            return {};
        Release release;
        release.version = *version;
        release.channel = ch == "stable" ? Channel::Stable : Channel::Preview;
        release.tag = j.at("tag");
        release.url = j.at("url");
        release.sha256 = j.at("sha256");
        release.size = j.at("size").get<std::uint64_t>();
        release.notes = j.at("notes");
        release.published = j.at("published");
        // Exact asset binding prevents cross-repository URLs, path/query tricks and asset
        // substitution.
        if (release.tag != "v" + FormatVersion(*version) ||
            release.url != "https://github.com/JulianDr14/jadyx/releases/download/" + release.tag +
                               "/Jadyx_" + FormatVersion(*version) + "_x64.appx" ||
            release.size == 0 || release.size > (4ull << 30) || release.notes.size() > 16384 ||
            release.published.size() > 64 || release.sha256.size() != 64 ||
            !std::all_of(release.sha256.begin(), release.sha256.end(),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return {};
        return release;
    } catch (...) {
        return {};
    }
}
} // namespace Jadyx::Updates
