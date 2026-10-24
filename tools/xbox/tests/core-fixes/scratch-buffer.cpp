// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <cstddef>
#include <new>
#include "common/scratch_buffer.h"

struct Element {
    unsigned char value;
    static inline bool fail{};
    static inline unsigned allocations{};
    static void* operator new[](std::size_t size) {
        ++allocations;
        if (fail) {
            throw std::bad_alloc{};
        }
        return ::operator new[](size);
    }
    static void operator delete[](void* pointer) noexcept {
        ::operator delete[](pointer);
    }
};
int main() {
    for (const std::size_t initial_size : {0U, 16U}) {
        Common::ScratchBuffer<Element> buffer;
        buffer.resize_destructive(initial_size);
        auto* original = buffer.data();
        if (initial_size != 0) {
            buffer[0].value = 42;
        }
        Element::fail = true;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto previous_allocations = Element::allocations;
            bool threw{};
            try {
                buffer.resize_destructive(32);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            assert(threw);
            assert(Element::allocations == previous_allocations + 1);
            assert(buffer.capacity() == initial_size && buffer.size() == initial_size);
            assert(buffer.data() == original);
            if (initial_size != 0) {
                assert(buffer[0].value == 42);
            }
        }
        Element::fail = false;
        buffer.resize_destructive(32);
        assert(buffer.data() && buffer.capacity() == 32 && buffer.size() == 32);
        buffer[31].value = 7;
        const auto previous_allocations = Element::allocations;
        auto* grown = buffer.data();
        buffer.resize_destructive(8);
        buffer.resize_destructive(32);
        assert(buffer.data() == grown && buffer[31].value == 7);
        assert(Element::allocations == previous_allocations);
        buffer.resize_destructive(0);
        assert(buffer.size() == 0 && buffer.capacity() == 32);
        Common::ScratchBuffer<Element> moved{std::move(buffer)};
        assert(moved.data() == grown && moved.capacity() == 32);
        assert(buffer.data() == nullptr && buffer.size() == 0 && buffer.capacity() == 0);
    }
}
