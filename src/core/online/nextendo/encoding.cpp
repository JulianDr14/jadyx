// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/encoding.h"

#include <array>
#include <cctype>

#include <openssl/rand.h>
#include <openssl/sha.h>

namespace Core::Online::Nextendo {
namespace {

constexpr std::string_view kUrl =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string Base64UrlEncode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kUrl[(v >> 18) & 63];
        out += kUrl[(v >> 12) & 63];
        out += kUrl[(v >> 6) & 63];
        out += kUrl[v & 63];
    }
    if (const std::size_t rest = data.size() - i; rest > 0) {
        std::uint32_t v = data[i] << 16;
        if (rest == 2) {
            v |= data[i + 1] << 8;
        }
        out += kUrl[(v >> 18) & 63];
        out += kUrl[(v >> 12) & 63];
        if (rest == 2) {
            out += kUrl[(v >> 6) & 63];
        }
    }
    return out;
}

std::string Base64UrlEncode(std::string_view text) {
    return Base64UrlEncode(std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        const int value = DecodeChar(c);
        if (value < 0 || padding > 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (padding > 2 || bits >= 6) {
        return std::nullopt;
    }
    return out;
}

std::string HexEncode(std::span<const std::uint8_t> data) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const auto byte : data) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

std::vector<std::uint8_t> Sha256(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> digest(SHA256_DIGEST_LENGTH);
    SHA256(data.data(), data.size(), digest.data());
    return digest;
}

std::vector<std::uint8_t> RandomBytes(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (count > 0 && RAND_bytes(out.data(), static_cast<int>(count)) != 1) {
        return {};
    }
    return out;
}

} // namespace Core::Online::Nextendo
