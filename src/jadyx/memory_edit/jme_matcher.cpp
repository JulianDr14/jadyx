// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_matcher.h"

#include <algorithm>
#include <bit>
#include <cstring>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace Jadyx::MemoryEdit {

namespace {

template <typename T, typename Test>
u32 ScanScalar(const u8* page, u64* bits, Test test) {
    u32 hits = 0;
    for (size_t i = 0; i < MATCH_PAGE_SIZE / sizeof(T); ++i) {
        T value;
        std::memcpy(&value, page + i * sizeof(T), sizeof(T));
        if (test(value)) {
            bits[i / 64] |= u64{1} << (i % 64);
            ++hits;
        }
    }
    return hits;
}

template <typename T>
u32 ScanBits(const u8* page, u64* bits, u64 wanted) {
    return ScanScalar<T>(page, bits, [wanted = static_cast<T>(wanted)](T value) { return value == wanted; });
}

template <typename T>
u32 ScanRange(const u8* page, u64* bits, T low, T high) {
    return ScanScalar<T>(page, bits, [low, high](T value) { return value >= low && value < high; });
}

#ifdef __AVX2__

// `Compare(block)` gives a byte mask with all the bytes of a hit set: mostly zero, so a hit may be slow.
template <size_t WIDTH, typename Compare>
u32 ScanBlocks(const u8* page, u64* bits, Compare compare) {
    constexpr u32 ELEMENT = WIDTH == 8 ? 0xFFu : (u32{1} << WIDTH) - 1;
    u32 hits = 0;
    for (size_t offset = 0; offset < MATCH_PAGE_SIZE; offset += 32) {
        const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(page + offset));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(compare(block)));
        while (mask != 0) {
            const u32 low = static_cast<u32>(std::countr_zero(mask));
            const size_t index = (offset + low) / WIDTH;
            bits[index / 64] |= u64{1} << (index % 64);
            ++hits;
            mask &= ~(ELEMENT << low);
        }
    }
    return hits;
}

#endif

} // namespace

u32 MatchPageScalar(const Query& query, const u8* page, u64* bits) {
    std::fill_n(bits, MatchWords(query.kind), u64{0});
    switch (query.kind) {
    case ValueKind::I8:
        return ScanBits<u8>(page, bits, query.bits);
    case ValueKind::I16:
        return ScanBits<u16>(page, bits, query.bits);
    case ValueKind::I32:
        return ScanBits<u32>(page, bits, query.bits);
    case ValueKind::I64:
        return ScanBits<u64>(page, bits, query.bits);
    case ValueKind::F32:
        return ScanRange<float>(page, bits, query.low32, query.high32);
    case ValueKind::F64:
        return ScanRange<double>(page, bits, query.low64, query.high64);
    }
    return 0;
}

u32 MatchPage(const Query& query, const u8* page, u64* bits) {
#ifdef __AVX2__
    std::fill_n(bits, MatchWords(query.kind), u64{0});
    switch (query.kind) {
    case ValueKind::I8: {
        const __m256i wanted = _mm256_set1_epi8(static_cast<char>(query.bits));
        return ScanBlocks<1>(page, bits, [wanted](__m256i block) { return _mm256_cmpeq_epi8(block, wanted); });
    }
    case ValueKind::I16: {
        const __m256i wanted = _mm256_set1_epi16(static_cast<short>(query.bits));
        return ScanBlocks<2>(page, bits, [wanted](__m256i block) { return _mm256_cmpeq_epi16(block, wanted); });
    }
    case ValueKind::I32: {
        const __m256i wanted = _mm256_set1_epi32(static_cast<int>(query.bits));
        return ScanBlocks<4>(page, bits, [wanted](__m256i block) { return _mm256_cmpeq_epi32(block, wanted); });
    }
    case ValueKind::I64: {
        const __m256i wanted = _mm256_set1_epi64x(static_cast<long long>(query.bits));
        return ScanBlocks<8>(page, bits, [wanted](__m256i block) { return _mm256_cmpeq_epi64(block, wanted); });
    }
    case ValueKind::F32: {
        const __m256 low = _mm256_set1_ps(query.low32);
        const __m256 high = _mm256_set1_ps(query.high32);
        return ScanBlocks<4>(page, bits, [low, high](__m256i block) {
            const __m256 value = _mm256_castsi256_ps(block);
            return _mm256_castps_si256(
                _mm256_and_ps(_mm256_cmp_ps(value, low, _CMP_GE_OQ), _mm256_cmp_ps(value, high, _CMP_LT_OQ)));
        });
    }
    case ValueKind::F64: {
        const __m256d low = _mm256_set1_pd(query.low64);
        const __m256d high = _mm256_set1_pd(query.high64);
        return ScanBlocks<8>(page, bits, [low, high](__m256i block) {
            const __m256d value = _mm256_castsi256_pd(block);
            return _mm256_castpd_si256(
                _mm256_and_pd(_mm256_cmp_pd(value, low, _CMP_GE_OQ), _mm256_cmp_pd(value, high, _CMP_LT_OQ)));
        });
    }
    }
    return 0;
#else
    return MatchPageScalar(query, page, bits);
#endif
}

} // namespace Jadyx::MemoryEdit
