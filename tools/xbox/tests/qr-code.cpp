// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Gate for jadyx/ui/qr_code: every code is read back by a decoder written apart from the encoder
// (format BCH, unmasking, Reed-Solomon syndromes from exp/log tables, byte-mode parsing). A logo
// square is checked by counting the codewords it touches in each block against what that block's
// Reed-Solomon code can correct (half its error correction bytes).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "jadyx/ui/qr_code.h"

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct Gf {
    std::array<int, 512> exp{};
    std::array<int, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = x;
            log[x] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
    int Mul(int a, int b) const { return a == 0 || b == 0 ? 0 : exp[log[a] + log[b]]; }
};

using Jadyx::Ui::QrLevel;

// Levels M and H: ecc bytes per block and blocks, versions 1-10 (ISO/IEC 18004 table 9).
constexpr std::array<std::array<int, 11>, 2> Ecc{{{0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
                                                  {0, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28}}};
constexpr std::array<std::array<int, 11>, 2> Blocks{{{0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
                                                     {0, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8}}};
constexpr std::array<int, 2> LevelField{0b00, 0b10}; // Table 12: M is 00, H is 10.
// Total codewords per version (table 1).
constexpr std::array<int, 11> Total{0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
// Alignment centres (annex E).
const std::array<std::vector<int>, 11> Align{{{}, {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34},
                                             {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}}};

bool Reserved(int version, int size, int x, int y) {
    if ((x < 9 && y < 9) || (x >= size - 8 && y < 9) || (x < 9 && y >= size - 8)) return true;
    if (x == 6 || y == 6) return true;
    if (version >= 7 && ((x >= size - 11 && x < size - 8 && y < 6) || (y >= size - 11 && y < size - 8 && x < 6)))
        return true;
    const auto& a = Align[version];
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < a.size(); ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j + 1 == a.size()) || (i + 1 == a.size() && j == 0)) continue;
            if (std::abs(x - a[i]) <= 2 && std::abs(y - a[j]) <= 2) return true;
        }
    }
    return false;
}

bool Mask(int m, int i, int j) { // i row, j column, as the standard writes them
    switch (m) {
    case 0: return (i + j) % 2 == 0;
    case 1: return i % 2 == 0;
    case 2: return j % 3 == 0;
    case 3: return (i + j) % 3 == 0;
    case 4: return (i / 2 + j / 3) % 2 == 0;
    case 5: return (i * j) % 2 + (i * j) % 3 == 0;
    case 6: return ((i * j) % 2 + (i * j) % 3) % 2 == 0;
    default: return ((i * j) % 3 + (i + j) % 2) % 2 == 0;
    }
}

int FormatWord(int data) { // 5 bits -> 15-bit BCH(15,5) word, masked
    int value = data << 10;
    for (int bit = 14; bit >= 10; --bit) {
        if (value & (1 << bit)) value ^= 0x537 << (bit - 10);
    }
    return ((data << 10) | value) ^ 0x5412;
}

/// The data modules in reading order: column pairs from the right, up then down.
std::vector<std::array<int, 2>> DataModules(int version, int size) {
    std::vector<std::array<int, 2>> result;
    bool up = true;
    for (int right = size - 1; right > 0; right -= 2) {
        if (right == 6) --right;
        for (int step = 0; step < size; ++step) {
            const int y = up ? size - 1 - step : step;
            for (int x : {right, right - 1}) {
                if (!Reserved(version, size, x, y)) result.push_back({x, y});
            }
        }
        up = !up;
    }
    return result;
}

/// Which block each interleaved codeword belongs to.
std::vector<int> BlockOf(int l, int version) {
    const int blocks = Blocks[l][version], ecc = Ecc[l][version];
    const int data_total = Total[version] - blocks * ecc;
    const int short_len = data_total / blocks, long_blocks = data_total % blocks;
    std::vector<int> result;
    for (int i = 0; i < short_len + 1; ++i) {
        for (int b = 0; b < blocks; ++b) {
            if (i < short_len + (b >= blocks - long_blocks ? 1 : 0)) result.push_back(b);
        }
    }
    for (int i = 0; i < ecc; ++i) {
        for (int b = 0; b < blocks; ++b) result.push_back(b);
    }
    return result;
}

/// Decodes `code`; "" plus a failed check on any inconsistency.
std::string Decode(const Jadyx::Ui::QrCode& code, const Gf& gf) {
    const int size = code.size;
    const int version = (size - 17) / 4;
    if (version < 1 || version > 10 || code.modules.size() != static_cast<std::size_t>(size * size)) return "";
    // Finders and timing.
    bool finders = true;
    for (const auto [cx, cy] : std::array<std::array<int, 2>, 3>{{{3, 3}, {size - 4, 3}, {3, size - 4}}}) {
        for (int d = -3; d <= 3; ++d) {
            finders = finders && code.Dark(cx + d, cy - 3) && code.Dark(cx + d, cy + 3) && code.Dark(cx - 3, cy + d) &&
                      code.Dark(cx + 3, cy + d);
        }
        finders = finders && code.Dark(cx, cy) && !code.Dark(cx - 2, cy) && !code.Dark(cx + 2, cy);
    }
    Check(finders, "finder patterns");
    bool timing = true;
    for (int i = 8; i < size - 8; ++i) timing = timing && code.Dark(i, 6) == (i % 2 == 0) && code.Dark(6, i) == (i % 2 == 0);
    Check(timing, "timing patterns");
    Check(code.Dark(8, size - 8), "dark module");
    // Format: both copies, read as the standard numbers the bits (bit 14 first).
    int first = 0, second = 0;
    const std::array<std::array<int, 2>, 15> a{{{0, 8}, {1, 8}, {2, 8}, {3, 8}, {4, 8}, {5, 8}, {7, 8}, {8, 8},
                                                {8, 7}, {8, 5}, {8, 4}, {8, 3}, {8, 2}, {8, 1}, {8, 0}}};
    for (int k = 0; k < 15; ++k) first = (first << 1) | (code.Dark(a[k][0], a[k][1]) ? 1 : 0);
    for (int k = 0; k < 7; ++k) second = (second << 1) | (code.Dark(8, size - 1 - k) ? 1 : 0);
    for (int k = 0; k < 8; ++k) second = (second << 1) | (code.Dark(size - 8 + k, 8) ? 1 : 0);
    Check(first == second, "format copies agree");
    int format = -1;
    for (int data = 0; data < 32; ++data) {
        if (FormatWord(data) == first) format = data;
    }
    Check(format >= 0, "format BCH word");
    const int l = code.level == QrLevel::H ? 1 : 0;
    Check(format >> 3 == LevelField[static_cast<std::size_t>(l)], "format carries the level");
    if (format < 0) return "";
    const int mask = format & 7;
    Check(mask == code.mask, "format carries the mask");
    if (version >= 7) {
        int word = 0;
        for (int i = 17; i >= 0; --i) word = (word << 1) | (code.Dark(size - 11 + i % 3, i / 3) ? 1 : 0);
        Check(word >> 12 == version, "version block");
    }
    std::vector<int> bits;
    for (const auto& [x, y] : DataModules(version, size)) bits.push_back((code.Dark(x, y) != Mask(mask, y, x)) ? 1 : 0);
    std::vector<int> stream(static_cast<std::size_t>(Total[version]));
    Check(bits.size() >= stream.size() * 8, "codeword capacity");
    for (std::size_t i = 0; i < stream.size() * 8 && i < bits.size(); ++i) stream[i / 8] = stream[i / 8] << 1 | bits[i];
    // De-interleave and check each block's syndromes.
    const int blocks = Blocks[l][version], ecc = Ecc[l][version];
    const int data_total = Total[version] - blocks * ecc;
    const int short_len = data_total / blocks, long_blocks = data_total % blocks;
    std::vector<std::vector<int>> data(static_cast<std::size_t>(blocks)), check(static_cast<std::size_t>(blocks));
    std::size_t at = 0;
    for (int i = 0; i < short_len + 1; ++i) {
        for (int b = 0; b < blocks; ++b) {
            const int len = short_len + (b >= blocks - long_blocks ? 1 : 0);
            if (i < len) data[static_cast<std::size_t>(b)].push_back(stream[at++]);
        }
    }
    for (int i = 0; i < ecc; ++i) {
        for (int b = 0; b < blocks; ++b) check[static_cast<std::size_t>(b)].push_back(stream[at++]);
    }
    bool syndromes = true;
    std::vector<int> message;
    for (int b = 0; b < blocks; ++b) {
        std::vector<int> word = data[static_cast<std::size_t>(b)];
        word.insert(word.end(), check[static_cast<std::size_t>(b)].begin(), check[static_cast<std::size_t>(b)].end());
        for (int s = 0; s < ecc; ++s) {
            int value = 0;
            for (const int c : word) value = gf.Mul(value, gf.exp[s]) ^ c;
            syndromes = syndromes && value == 0;
        }
        message.insert(message.end(), data[static_cast<std::size_t>(b)].begin(), data[static_cast<std::size_t>(b)].end());
    }
    Check(syndromes, "Reed-Solomon syndromes are zero");
    // Byte mode segment.
    std::size_t pos = 0;
    const auto read = [&](int count) {
        int value = 0;
        for (int i = 0; i < count; ++i, ++pos) value = value << 1 | ((message[pos / 8] >> (7 - pos % 8)) & 1);
        return value;
    };
    Check(read(4) == 0b0100, "byte mode");
    const int length = read(version <= 9 ? 8 : 16);
    std::string text;
    for (int i = 0; i < length && pos + 8 <= message.size() * 8; ++i) text.push_back(static_cast<char>(read(8)));
    return text;
}

/// The logo square: inside the code, clear of every function pattern, and each block loses at
/// most a third of its error correction bytes (half is the hard limit; the rest is for the camera).
void CheckLogo(const Jadyx::Ui::QrCode& code) {
    const int side = Jadyx::Ui::QrLogoModules(code);
    const std::string name = "logo square, version " + std::to_string(code.version);
    if (code.level != QrLevel::H || code.version < 2 || code.version > 6) {
        Check(side == 0, (name + ": none outside H 2-6").c_str());
        return;
    }
    Check(side >= 5 && side % 2 == 1, (name + ": odd and at least 5").c_str());
    const int lo = (code.size - side) / 2, hi = lo + side;
    bool clear = true;
    for (int y = lo; y < hi; ++y) {
        for (int x = lo; x < hi; ++x) clear = clear && !Reserved(code.version, code.size, x, y);
    }
    Check(clear, (name + ": no function module under it").c_str());
    const auto modules = DataModules(code.version, code.size);
    const auto block = BlockOf(1, code.version);
    std::vector<int> hit(block.size());
    for (std::size_t i = 0; i < modules.size() && i / 8 < block.size(); ++i) {
        const auto& [x, y] = modules[i];
        if (x >= lo && x < hi && y >= lo && y < hi) hit[i / 8] = 1;
    }
    std::vector<int> lost(static_cast<std::size_t>(Blocks[1][code.version]));
    for (std::size_t i = 0; i < hit.size(); ++i) lost[static_cast<std::size_t>(block[i])] += hit[i];
    int worst = 0;
    for (const int n : lost) worst = std::max(worst, n);
    std::printf("%s: %dx%d modules, worst block loses %d codewords, can fix %d\n", name.c_str(), side, side,
                worst, Ecc[1][code.version] / 2);
    Check(worst * 3 <= Ecc[1][code.version], (name + ": correctable with room to spare").c_str());
}

void RoundTrip(const std::string& text, int expected_version, const Gf& gf, QrLevel level = QrLevel::M) {
    const auto code = Jadyx::Ui::EncodeQr(text, level);
    Check(code.has_value(), ("encodes: " + text).c_str());
    if (!code) return;
    Check(code->level == level, ("keeps the level: " + text).c_str());
    if (expected_version) Check(code->version == expected_version, ("smallest version: " + text).c_str());
    Check(Decode(*code, gf) == text, ("reads back: " + text).c_str());
    CheckLogo(*code);
    for (int mask = 0; mask < 8; ++mask) {
        const auto forced = Jadyx::Ui::EncodeQr(text, level, mask);
        Check(forced && forced->mask == mask && Decode(*forced, gf) == text, ("reads back with every mask: " + text).c_str());
    }
}

} // namespace

int main() {
    const Gf gf;
    // Capacity edges of level M byte mode: 14, 26, 42, 62, ... bytes.
    RoundTrip("A", 1, gf);
    RoundTrip(std::string(14, 'x'), 1, gf);
    RoundTrip(std::string(15, 'x'), 2, gf);
    RoundTrip("http://192.168.1.50:8088/#pair=1234&open=nextendo", 4, gf);
    RoundTrip(std::string(84, 'q'), 5, gf);
    RoundTrip(std::string(106, 'r'), 6, gf);
    RoundTrip(std::string(122, 's'), 7, gf);
    RoundTrip(std::string(152, 't'), 8, gf);
    RoundTrip(std::string(180, 'u'), 9, gf);
    RoundTrip(std::string(213, 'v'), 10, gf);
    std::string all;
    for (int c = 1; c < 256; ++c) all.push_back(static_cast<char>(c));
    RoundTrip(all.substr(0, 200), 10, gf);
    Check(!Jadyx::Ui::EncodeQr(std::string(214, 'w')), "refuses more than 213 bytes");
    Check(!Jadyx::Ui::EncodeQr(""), "refuses empty text");
    Check(!Jadyx::Ui::EncodeQr("x", QrLevel::M, 8), "refuses mask 8");
    // Level H capacity edges: 7, 14, 24, 34, 44, 58, 64, 84, 98, 119 bytes.
    const std::array<int, 11> edge{0, 7, 14, 24, 34, 44, 58, 64, 84, 98, 119};
    for (int version = 1; version <= 10; ++version) {
        RoundTrip(std::string(static_cast<std::size_t>(edge[version]), 'h'), version, gf, QrLevel::H);
        RoundTrip(std::string(static_cast<std::size_t>(edge[version - 1] + 1), 'k'), version, gf, QrLevel::H);
    }
    RoundTrip("http://192.168.1.50:8088/#pair=1234&open=nextendo", 6, gf, QrLevel::H);
    RoundTrip("http://255.255.255.255:65535/#pair=1234&open=nextendo", 6, gf, QrLevel::H);
    Check(!Jadyx::Ui::EncodeQr(std::string(120, 'w'), QrLevel::H), "refuses more than 119 bytes at H");
    std::printf("qr gate: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
