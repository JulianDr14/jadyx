// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from NXbox tests/port/ftp_protocol.cpp (GPL-3.0-or-later).
#include <cassert>
#include <regex>
#include "jadyx/storage/ftp/ftp_protocol.h"

using namespace Jadyx::Ftp;
int main() {
    for (const auto* path :
         {"../x", "/../../x", "a/../../b", "..\\x", "\\..\\..\\x", "a\\..\\..\\b", "C:\\Windows",
          "//?/C:/x", "games/file:stream", "games/NUL.nsp", "games/COM1", "games/file.",
          "games/file ", "games/file.part", "games/FILE.PART", "games/a\r\nb"})
        assert(!NormalizePath("", path));
    assert(!NormalizePath("", std::string("games/a\0b", 9)));
    assert(NormalizePath("", "games/My Game.nsp") == "games/My Game.nsp");
    assert(NormalizePath("", "games\\My Game.nsp") == "games/My Game.nsp");
    assert(NormalizePath("games", "../eden/keys/prod.keys") == "eden/keys/prod.keys");
    assert(NormalizePath("games", "/firmware/./update.nca") == "firmware/update.nca");
    assert(NormalizePath("games/subdir", "..") == "games");
    assert(NormalizePath("games", "/") == "");
    assert(!NormalizePath("games", "../../escape"));
    assert(InGames("GAMES/My Game.nsp"));
    assert(!InGames("games-old/file.nsp"));

    const auto command = ParseCommand("sToR games/My Game with spaces.nsp\r\n");
    assert(command.verb == "STOR");
    assert(command.argument == "games/My Game with spaces.nsp");
    assert(ParseCommand("NOOP\r\n").argument.empty());
    assert(ParseCommand("PASS 123456").argument == "123456");
    assert(ParseCommand("STOR  leading space.nsp\r\n").argument == " leading space.nsp");
    assert(ParseCommand("OPTS UTF8 ON\r\n").argument == "UTF8 ON");

    assert(PasvReply({192, 168, 1, 42}, 50000) ==
           "227 Entering Passive Mode (192,168,1,42,195,80).\r\n");
    assert(PasvReply({10, 0, 0, 2}, 50009) == "227 Entering Passive Mode (10,0,0,2,195,89).\r\n");
    assert(EpsvReply(50000) == "229 Entering Extended Passive Mode (|||50000|).\r\n");
    assert(PrivateIPv4("10.0.0.2"));
    assert(PrivateIPv4("172.16.0.2"));
    assert(PrivateIPv4("172.31.255.254"));
    for (const auto* address : {"127.0.0.1", "172.32.0.2", "8.8.8.8", "192.168.1.256", "192.168.1",
                                "192.168.1.2.3", "192.168..2", "::1"})
        assert(!PrivateIPv4(address));

    std::tm utc{};
    utc.tm_year = 126;
    utc.tm_mon = 9;
    utc.tm_mday = 8;
    utc.tm_hour = 12;
    utc.tm_min = 34;
    utc.tm_sec = 56;
    assert(Timestamp(utc) == "20261008123456");
    const auto line = ListLine("My Game.nsp", false, 12ULL * 1024 * 1024 * 1024, utc);
    assert(line == "-rw-r--r-- 1 jadyx jadyx 12884901888 Oct  8 2026 My Game.nsp\r\n");
    assert(std::regex_match(
        line, std::regex(
                  R"(-[rwx-]{9} 1 jadyx jadyx [0-9]+ [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} .+\r\n)")));
    assert(ListLine("games", true, 0, utc) == "drwxr-xr-x 1 jadyx jadyx 0 Oct  8 2026 games\r\n");
    assert(!InGames("interno/games/test.nsp"));
    assert(!InGames("hd/games/test.nsp"));
}
