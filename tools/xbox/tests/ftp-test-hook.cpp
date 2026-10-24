// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <sstream>
#include "jadyx/storage/ftp/ftp_test_hook.h"

int main() {
    const auto parse = [](const std::string& text) {
        std::istringstream input{text};
        return Jadyx::Ftp::ReadTestPassword(input);
    };
    assert(parse("").empty());
    assert(parse("library=1\npassword=abcdefghijkl\n").empty());
    assert(parse("ftp_test_password=\n").empty());
    assert(parse("ftp_test_password=abcdefghijk\n").empty());
    assert(parse("ftp_test_password=abcdefghijkl\n") == "abcdefghijkl");
    assert(parse("library=1\r\nftp_test_password=abcdefghijkl\r\n") == "abcdefghijkl");
    assert(parse("ftp_test_password=abcdefghijkl=mnop") == "abcdefghijkl=mnop");
    assert(parse("#ftp_test_password=abcdefghijkl\n").empty());
    assert(parse(" ftp_test_password=abcdefghijkl\n").empty());
    assert(parse("ftp_test_password=abcdef\tghijkl\n").empty());
    assert(parse(std::string{"ftp_test_password="} + std::string{"abcdef\0ghijkl", 13}).empty());
}
