// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace D3D12 {

// Selection hints only: the existing guest cache supplies the environments and regenerates
// shaders. Never load DXIL/driver blobs from this file. Full fixed-state keys avoid confusing
// descriptor indices between different runtime specializations of a guest shader.
template <typename Key>
struct AstcWarmRecord {
    Key key;
    std::vector<std::uint32_t> descriptors;
};

constexpr std::uint32_t AstcWarmLimit = 256;

template <typename Key>
bool WriteAstcWarm(std::ostream& out, std::span<const AstcWarmRecord<Key>> records) {
    static_assert(std::is_trivially_copyable_v<Key>);
    if (records.size() > AstcWarmLimit)
        return false;
    const std::array<std::uint32_t, 3> header{0x31574341, sizeof(Key),
                                              static_cast<std::uint32_t>(records.size())};
    out.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    for (const auto& record : records) {
        const auto& ids = record.descriptors;
        if (ids.empty() || ids.size() > 128 || ids.back() >= 128 ||
            !std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            return false;
        const auto count = static_cast<std::uint32_t>(ids.size());
        out.write(reinterpret_cast<const char*>(&record.key), sizeof(Key));
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(ids.data()), count * sizeof(ids[0]));
    }
    return bool(out);
}

template <typename Key>
std::vector<AstcWarmRecord<Key>> ReadAstcWarm(std::istream& in) {
    static_assert(std::is_trivially_copyable_v<Key>);
    std::array<std::uint32_t, 3> header{};
    in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    if (!in || header[0] != 0x31574341 || header[1] != sizeof(Key) || header[2] > AstcWarmLimit)
        return {};
    std::vector<AstcWarmRecord<Key>> records;
    for (std::uint32_t n = 0; n < header[2]; ++n) {
        AstcWarmRecord<Key> record{};
        std::uint32_t count{};
        in.read(reinterpret_cast<char*>(&record.key), sizeof(Key));
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count == 0 || count > 128)
            return {};
        record.descriptors.resize(count);
        in.read(reinterpret_cast<char*>(record.descriptors.data()), count * sizeof(std::uint32_t));
        const auto& ids = record.descriptors;
        if (!in || ids.back() >= 128 || !std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            return {};
        records.push_back(std::move(record));
    }
    if (in.peek() != std::char_traits<char>::eof())
        return {};
    return records;
}

} // namespace D3D12
