// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
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

    // Player 1 keeps its pad while it can play, whatever the others do.
    using Jadyx::PreferredPlayerOne;
    std::vector<std::wstring> playable{L"one", L"two", L"three"};
    const auto can_play = [&](const std::wstring& id) {
        return std::find(playable.begin(), playable.end(), id) != playable.end();
    };
    slots.Assign({L"three", L"two"}); // Player 2 is "three", player 3 is "two".
    check(PreferredPlayerOne(L"one", slots, can_play) == L"one");
    // It leaves: the lowest player steps up, not the first pad listed.
    playable.erase(playable.begin());
    check(PreferredPlayerOne(L"one", slots, can_play) == L"three");
    // Nobody yet: any pad.
    check(PreferredPlayerOne(L"", Jadyx::PlayerSlots{}, can_play).empty());
    // A player whose pad cannot play now is skipped.
    playable = {L"two"};
    check(PreferredPlayerOne(L"one", slots, can_play) == L"two");
    playable.clear();
    check(PreferredPlayerOne(L"one", slots, can_play).empty());
    std::puts("player slots: assign/stable/leave/refill/overflow/player one PASS");
}
