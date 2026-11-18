// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

#include "jadyx/network/chat_page.h"
#include "jadyx/network/online_chat.h"
#include "jadyx/network/room_ping.h"

using namespace Jadyx::Online;
using Jadyx::MenuAction;

namespace {
void Check(bool ok, std::source_location where = std::source_location::current()) {
    if (ok) return;
    std::cerr << "Online chat check failed at line " << where.line() << '\n';
    std::exit(1);
}
} // namespace

int main() {
    // Room pings: one target per address and port, results handed to every room listed there.
    {
        std::vector<LobbyRoom> rooms(5);
        rooms[0].address = "1.2.3.4";
        rooms[0].port = 24872;
        rooms[1].address = "5.6.7.8";
        rooms[1].port = 24872;
        rooms[2].address = "1.2.3.4";
        rooms[2].port = 24872;
        rooms[3].address = "1.2.3.4";
        rooms[3].port = 24873;
        rooms[4].address = "9.9.9.9";
        const auto targets = PingTargets(rooms);
        Check(targets.size() == 3);
        Check(targets[0].address == "1.2.3.4" && targets[0].port == 24872);
        Check(targets[1].address == "5.6.7.8" && targets[2].port == 24873);
        Check(PingKey("1.2.3.4", 24872) == "1.2.3.4:24872");
        Check(PingLabel(rooms[0]).empty());
        ApplyPings(rooms, {{"1.2.3.4:24872", 42}, {"5.6.7.8:24872", std::nullopt}});
        Check(rooms[0].pinged && rooms[2].pinged && rooms[0].ping_ms == 42 && rooms[2].ping_ms == 42);
        Check(rooms[1].pinged && !rooms[1].ping_ms);
        Check(!rooms[3].pinged && !rooms[4].pinged);
        Check(PingLabel(rooms[0]) == L"42 ms");
        Check(PingLabel(rooms[1]) == L"— ms");
        Check(PingLabel(rooms[3]).empty());
        Check(PingToneFor(0) == PingTone::Good && PingToneFor(PingGoodMs) == PingTone::Good);
        Check(PingToneFor(PingGoodMs + 1) == PingTone::Fair && PingToneFor(PingFairMs) == PingTone::Fair);
        Check(PingToneFor(PingFairMs + 1) == PingTone::Poor);
        Check(PingTargets({}).empty());
    }
    // Cleaning: control characters, ends, empty input.
    Check(CleanChatText("  hola\tque\ntal  ") == "hola que tal");
    Check(CleanChatText("   ").empty());
    Check(CleanChatText("").empty());
    Check(CleanChatText("\r\n").empty());
    // The limit never splits a character: "ñ" is two bytes, "€" three, an emoji four.
    const std::string enye = "\xc3\xb1";
    const std::string euro = "\xe2\x82\xac";
    const std::string emoji = "\xf0\x9f\x8e\xae";
    Check(CleanChatText("ab" + enye, 3) == "ab");
    Check(CleanChatText("ab" + enye, 4) == "ab" + enye);
    Check(CleanChatText("a" + euro, 3) == "a");
    Check(CleanChatText("a" + emoji, 4) == "a");
    Check(CleanChatText("a" + emoji, 5) == "a" + emoji);
    // A message the room already cut inside a character loses the broken tail.
    Check(CleanChatText("ok" + euro.substr(0, 2)) == "ok");
    Check(CleanChatText("ok" + emoji.substr(0, 1)) == "ok");
    Check(CleanChatText("ok\x82") == "ok");
    Check(CleanChatText("x" + enye + euro + emoji) == "x" + enye + euro + emoji);
    // A long message stops at the room's limit, on a character boundary.
    std::string long_text;
    for (int i = 0; i < 300; ++i) long_text += enye;
    const auto cut = CleanChatText(long_text);
    Check(cut.size() == MaxChatBytes && cut.size() % 2 == 0);
    long_text = "a" + long_text;
    Check(CleanChatText(long_text).size() == MaxChatBytes - 1);

    ChatLog log;
    const auto v0 = log.Version();
    Check(!log.AddMessage("Ana", "   ", false));
    Check(log.Version() == v0);
    Check(log.AddMessage("Ana", "hola", false));
    Check(log.AddMessage("Yo", "hey", true));
    log.AddStatus(ChatKind::Joined, "Beto");
    auto snap = log.Snapshot();
    Check(snap.lines.size() == 3 && snap.unread == 1);
    Check(snap.lines[0].nickname == "Ana" && !snap.lines[0].own);
    Check(snap.lines[1].own && snap.lines[2].kind == ChatKind::Joined);
    Check(snap.lines[0].id < snap.lines[1].id && snap.lines[1].id < snap.lines[2].id);
    // Reading clears the count and changes the version once.
    const auto before_read = log.Version();
    log.MarkRead();
    Check(log.Unread() == 0 && log.Version() != before_read);
    const auto read = log.Version();
    log.MarkRead();
    Check(log.Version() == read);
    // A muted member's messages are dropped; one's own never are.
    log.Mute("Ana", true);
    Check(log.IsMuted("Ana"));
    Check(!log.AddMessage("Ana", "spam", false));
    Check(log.Unread() == 0);
    log.Mute("Ana", false);
    Check(log.AddMessage("Ana", "ya", false) && log.Unread() == 1);
    // The ring keeps the newest lines; ids keep growing.
    for (int i = 0; i < 500; ++i) log.AddMessage("Beto", std::to_string(i), false);
    snap = log.Snapshot();
    Check(snap.lines.size() == ChatHistory);
    Check(snap.lines.front().text == std::to_string(500 - ChatHistory));
    Check(snap.lines.back().text == "499");
    Check(snap.unread == 501);
    const auto last_id = snap.lines.back().id;
    // Leaving the room clears lines, unread messages and mutes.
    log.Mute("Beto", true);
    log.Clear();
    snap = log.Snapshot();
    Check(snap.lines.empty() && snap.unread == 0 && !log.IsMuted("Beto"));
    Check(log.AddMessage("Beto", "otra sala", false));
    Check(log.Snapshot().lines.back().id > last_id);

    // The page: scrolling stops at both ends, the picker wraps and sends, B leaves.
    using Effect = ChatPage::Effect;
    ChatPage page;
    Check(page.Handle(MenuAction::Down, 5) == Effect::None);
    Check(page.Handle(MenuAction::Up, 5) == Effect::Redraw && page.scroll == 1);
    for (int i = 0; i < 10; ++i) page.Handle(MenuAction::Up, 5);
    Check(page.scroll == 4);
    Check(page.Handle(MenuAction::Up, 1) == Effect::None);
    // New lines keep a reader scrolled up on the same lines; at the bottom it follows.
    page.Grew(2, 7);
    Check(page.scroll == 6);
    page.Grew(500, 200);
    Check(page.scroll == 199);
    page.scroll = 0;
    page.Grew(3, 10);
    Check(page.scroll == 0);
    page.Grew(0, 0);
    Check(page.scroll == 0);
    Check(page.Handle(MenuAction::Confirm, 3) == Effect::Compose);
    Check(page.Handle(MenuAction::Refresh, 3) == Effect::Redraw && page.picking);
    Check(page.Handle(MenuAction::Up, 3) == Effect::Redraw && page.phrase == QuickPhrases.size() - 1);
    Check(page.Handle(MenuAction::Down, 3) == Effect::Redraw && page.phrase == 0);
    page.Handle(MenuAction::Down, 3);
    page.scroll = 2;
    Check(page.Handle(MenuAction::Confirm, 3) == Effect::Send && !page.picking && page.scroll == 0);
    Check(PhraseAt(page.phrase) == QuickPhrase::Again);
    page.Handle(MenuAction::Refresh, 3);
    Check(page.Handle(MenuAction::Back, 3) == Effect::Redraw && !page.picking);
    Check(page.Handle(MenuAction::Back, 3) == Effect::Back);
    std::cout << "Online chat: cleaning, UTF-8 limits, unread, mute, ring, clear and page passed\n";
    return 0;
}
