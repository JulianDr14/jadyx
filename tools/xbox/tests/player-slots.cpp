// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "jadyx/input/player_slots.h"

int main() {
    using Jadyx::MaxPlayers;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    using Changed = std::vector<std::size_t>;
    Jadyx::PlayerSlots slots;

    // Nobody but player 1.
    check(slots.Assign({}).empty());
    check(slots.Id(0).empty() && slots.Id(1).empty());

    // Two more pads become players 2 and 3, in discovery order.
    check(slots.Assign({L"b", L"c"}) == Changed{1, 2});
    check(slots.Id(1) == L"b" && slots.Id(2) == L"c");
    // Same pads, any order: nothing moves.
    check(slots.Assign({L"c", L"b"}).empty());

    // Player 2 leaves: player 3 keeps its number, and the next pad fills the gap.
    check(slots.Assign({L"c"}) == Changed{1});
    check(slots.Id(1).empty() && slots.Id(2) == L"c");
    check(slots.Assign({L"d", L"c"}) == Changed{1});
    check(slots.Id(1) == L"d" && slots.Id(2) == L"c");

    // A pad leaving and another arriving at once on the same player reports it once.
    check(slots.Assign({L"e", L"c"}) == Changed{1});
    check(slots.Id(1) == L"e");

    // More pads than players: the eighth extra one waits for a free player.
    std::vector<std::wstring> many;
    for (int i = 0; i < 8; ++i) many.push_back(L"p" + std::to_wstring(i));
    slots.Assign(many);
    for (std::size_t player = 1; player < MaxPlayers; ++player)
        check(slots.Id(player) == many[player - 1]);
    many.erase(many.begin() + 2); // Player 4 leaves; the waiting pad takes it.
    check(slots.Assign(many) == Changed{3});
    check(slots.Id(3) == L"p7");

    // Everyone leaves.
    check(slots.Assign({}).size() == MaxPlayers - 1);
    std::puts("player slots: assign/stable/leave/refill/overflow PASS");
}
