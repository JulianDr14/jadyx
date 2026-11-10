// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The room chat's history, apart from the network so a desktop gate can check it. Eden's rooms
// carry text only (no voice): a message of up to 500 bytes, and status lines when someone joins
// or leaves. The room forwards a message to everyone but its sender, so a sent one is added here.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Online {

/// The room's limit for one message, in UTF-8 bytes (Network::MaxMessageSize).
inline constexpr std::size_t MaxChatBytes = 500;
/// How many lines the history keeps; older ones fall off.
inline constexpr std::size_t ChatHistory = 200;

enum class ChatKind : std::uint8_t { Message, Joined, Left, Kicked, Banned };

struct ChatLine {
    ChatKind kind{ChatKind::Message};
    std::string nickname; ///< Who wrote it, or who joined or left.
    std::string text;     ///< Messages only.
    bool own{};           ///< Sent from this console.
    std::uint64_t id{};   ///< Grows by one per line, never reused while the app runs.
};

struct ChatSnapshot {
    std::vector<ChatLine> lines; ///< Oldest first.
    std::size_t unread{};        ///< Messages from others since the chat was last read.
    std::uint64_t version{};     ///< Changes whenever the lines or the unread count do.
};

/// A message as the room may send it: at most `limit` bytes, never cut inside a UTF-8 character,
/// control characters as spaces and the ends trimmed. Empty when nothing is left.
inline std::string CleanChatText(std::string_view text, std::size_t limit = MaxChatBytes) {
    std::string out;
    out.reserve(std::min(text.size(), limit));
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        out.push_back(c < 0x20 || c == 0x7f ? ' ' : raw);
    }
    if (out.size() > limit) {
        std::size_t end = limit;
        // Back off continuation bytes, then the lead byte of the character they belong to.
        while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xc0) == 0x80) --end;
        out.resize(end);
    }
    // A sender that cut a character (the room cuts at 500 bytes) leaves a broken tail.
    if (!out.empty()) {
        const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(out[at]); };
        std::size_t start = out.size() - 1;
        while (start > 0 && out.size() - start < 4 && (byte(start) & 0xc0) == 0x80) --start;
        const auto lead = byte(start);
        const std::size_t length = lead < 0x80   ? 1
                                   : lead >= 0xf0 ? 4
                                   : lead >= 0xe0 ? 3
                                   : lead >= 0xc0 ? 2
                                                  : 0;
        if (length == 1) out.resize(start + 1);
        else if (length != out.size() - start) out.resize(start);
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

/// The history itself. Not thread safe: Online keeps it under its lock.
class ChatLog {
public:
    /// Adds a message; false when it is empty or its sender is muted.
    bool AddMessage(std::string nickname, std::string_view text, bool own) {
        if (!own && muted.contains(nickname)) return false;
        std::string clean = CleanChatText(text);
        if (clean.empty()) return false;
        Push({ChatKind::Message, std::move(nickname), std::move(clean), own, 0});
        if (!own) ++unread;
        return true;
    }
    void AddStatus(ChatKind kind, std::string nickname) {
        Push({kind, std::move(nickname), {}, false, 0});
    }
    void MarkRead() {
        if (unread == 0) return;
        unread = 0;
        ++version;
    }
    /// Hides a member's messages until the room is left. Ones already shown stay.
    void Mute(const std::string& nickname, bool mute) {
        const bool changed = mute ? muted.insert(nickname).second : muted.erase(nickname) > 0;
        if (changed) ++version;
    }
    bool IsMuted(const std::string& nickname) const { return muted.contains(nickname); }
    /// The room was left: its lines, unread messages and mutes go with it.
    void Clear() {
        if (lines.empty() && unread == 0 && muted.empty()) return;
        lines.clear();
        muted.clear();
        unread = 0;
        ++version;
    }
    ChatSnapshot Snapshot() const {
        return {{lines.begin(), lines.end()}, unread, version};
    }
    std::uint64_t Version() const { return version; }
    std::size_t Unread() const { return unread; }

private:
    void Push(ChatLine line) {
        line.id = ++next_id;
        lines.push_back(std::move(line));
        while (lines.size() > ChatHistory) lines.pop_front();
        ++version;
    }

    std::deque<ChatLine> lines;
    std::set<std::string> muted;
    std::size_t unread{};
    std::uint64_t version{};
    std::uint64_t next_id{};
};

} // namespace Jadyx::Online
