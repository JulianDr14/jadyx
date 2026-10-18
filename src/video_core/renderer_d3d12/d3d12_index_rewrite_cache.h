// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <span>
#include <unordered_map>
#include <vector>

namespace D3D12 {

/// Index lists D3D12 cannot draw (quads, fans, loops, 8-bit indices) are rewritten on the CPU.
/// Games draw the same geometry every frame, so the rewrite is remembered per guest index range.
///
/// An entry is trusted without reading guest memory while its buffer's write generation is the
/// one it was made from: every GPU or upload write to a cache buffer bumps the generation. When
/// the generation moved (something else in the buffer was written), the guest bytes are read and
/// compared with the ones the rewrite came from; only different bytes are rewritten again.
/// Generated lists (non-indexed draws) depend on nothing but the key: buffer_id 0.
class IndexRewriteCache {
public:
    struct Key {
        std::uint64_t buffer_id{};  ///< 0 for generated lists
        std::uint64_t offset{};     ///< byte offset of the first index (or the first vertex)
        std::uint32_t count{};
        std::uint8_t format{};      ///< guest index size in bytes, 0 for generated lists
        std::uint8_t topology{};
        bool operator==(const Key&) const = default;
    };

    struct Entry {
        std::uint64_t generation{};
        std::vector<std::uint8_t> source; ///< the guest bytes the rewrite came from
        std::vector<std::uint8_t> rewrite; ///< as uploaded: 16-bit or 32-bit indices
        std::uint32_t count{};             ///< indices in rewrite
        bool wide{};                       ///< 32-bit indices
        /// The upload of rewrite in the command list of upload_tick, rebound while that list is
        /// still being recorded (its staging memory cannot be recycled before it retires).
        std::uint64_t upload_tick{~std::uint64_t{}};
        std::uint64_t upload_address{};

        [[nodiscard]] std::size_t Bytes() const noexcept {
            return source.capacity() + rewrite.capacity() + sizeof(Entry);
        }
        /// Stores a new rewrite from 32-bit vertex numbers, narrowed unless wide.
        void SetRewrite(std::span<const std::uint32_t> indices, bool wide_) {
            wide = wide_;
            count = static_cast<std::uint32_t>(indices.size());
            const std::size_t size = wide ? sizeof(std::uint32_t) : sizeof(std::uint16_t);
            rewrite.resize(indices.size() * size);
            std::uint8_t* out = rewrite.data();
            if (wide) {
                std::memcpy(out, indices.data(), rewrite.size());
            } else {
                for (const std::uint32_t index : indices) {
                    const auto narrow = static_cast<std::uint16_t>(index);
                    std::memcpy(out, &narrow, sizeof(narrow));
                    out += sizeof(narrow);
                }
            }
            upload_tick = ~std::uint64_t{};
        }
    };

    explicit IndexRewriteCache(std::size_t budget_bytes = 32u << 20) : budget{budget_bytes} {}
    IndexRewriteCache(const IndexRewriteCache&) = delete;
    IndexRewriteCache& operator=(const IndexRewriteCache&) = delete;

    /// The entry for key, made the most recently used; nullptr when there is none.
    [[nodiscard]] Entry* Find(const Key& key) {
        const auto it = map.find(key);
        if (it == map.end()) {
            return nullptr;
        }
        order.splice(order.begin(), order, it->second);
        return &it->second->entry;
    }

    /// The entry for key, created empty (generation 0, no source) when missing. Call Account()
    /// once it is filled; older entries are evicted then.
    [[nodiscard]] Entry& Obtain(const Key& key) {
        if (Entry* const found = Find(key)) {
            used -= found->Bytes();
            return *found;
        }
        order.push_front(Node{key, {}});
        map.emplace(key, order.begin());
        return order.front().entry;
    }

    /// Charges the most recently obtained entry and evicts the least recently used ones that do
    /// not fit. An entry bigger than the whole budget is kept alone until the next one.
    void Account() {
        used += order.front().entry.Bytes();
        while (order.size() > 1 && (used > budget || order.size() > MAX_ENTRIES)) {
            Node& last = order.back();
            used -= last.entry.Bytes();
            map.erase(last.key);
            order.pop_back();
        }
    }

    [[nodiscard]] std::size_t Size() const noexcept { return order.size(); }
    [[nodiscard]] std::size_t UsedBytes() const noexcept { return used; }

    static constexpr std::size_t MAX_ENTRIES = 8192;

private:
    struct Node {
        Key key;
        Entry entry;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            // Offsets are what usually differ: mix them with a multiply, then fold in the rest.
            std::uint64_t h = key.buffer_id * 0x9E3779B97F4A7C15ull ^ key.offset;
            h ^= (std::uint64_t{key.count} << 16) ^ (std::uint64_t{key.format} << 8) ^ key.topology;
            h *= 0xBF58476D1CE4E5B9ull;
            return static_cast<std::size_t>(h ^ (h >> 31));
        }
    };

    std::size_t budget;
    std::size_t used{};
    std::list<Node> order; ///< most recently used first
    std::unordered_map<Key, std::list<Node>::iterator, KeyHash> map;
};

} // namespace D3D12
