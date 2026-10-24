// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include "jadyx/library/compatibility.h"

int main() {
    using namespace Jadyx::Compatibility;
    assert(!RefreshEnabled({}, {}));
    assert(!RefreshEnabled("0", "NXBOX_COMPAT_REFRESH=1"));
    assert(RefreshEnabled("1", {}));
    assert(RefreshEnabled({}, "library=1\nNXBOX_COMPAT_REFRESH=1\r\n"));
    assert(!RefreshEnabled({}, "NXBOX_COMPAT_REFRESH=1\nNXBOX_COMPAT_REFRESH=0"));
    assert(!RefreshEnabled({}, "# NXBOX_COMPAT_REFRESH=1\nNXBOX_COMPAT_REFRESH=true"));
    const std::string good =
        R"({"schema_version":1,"titles":{"010028600EBDA000":[{"title_id":"010028600EBDA000","name":"Example","status":"issues","console":"X","fps_note":"20-30 FPS","jadyx_version":"","note":"Test report","source":"README.md","product":"Jadyx"}]}})";
    assert(!Parse("broken"));
    assert(!Parse("[]"));
    assert(!Parse(R"({"schema_version":2,"titles":{}})"));
    assert(!Parse(R"({"schema_version":1,"titles":{"010028600EBDA000":[{}]}})"));
    const auto parsed = Parse(good);
    assert(parsed && Lookup(*parsed, 0x010028600EBDA000).status == Status::Issues);
    assert(Lookup(*parsed, 0x010028600EBDA800).status == Status::Issues);
    assert(Lookup(*parsed, 0x010028600EBDB001).status == Status::Untested); // DLC is not an update.
    assert(Lookup(*parsed, 0).status == Status::Untested);
    auto unknown = good;
    unknown.replace(unknown.find("issues"), 6, "future-status");
    assert(Parse(unknown) &&
           Lookup(*Parse(unknown), 0x010028600EBDA000).status == Status::Untested);
    auto foreign = good;
    foreign.replace(foreign.find("\"product\":\"Jadyx\""), 17, "\"product\":\"NXbox\"");
    assert(Parse(foreign) &&
           Lookup(*Parse(foreign), 0x010028600EBDA000).status == Status::Untested);
    assert(Select(good, "bad", "bad").size() == 1);
    assert(Select("bad", good, "bad").size() == 1);
    assert(Select("bad", "bad", good).size() == 1);
    assert(Select("bad", "bad", "bad").empty());
    assert(Select(R"({"schema_version":1,"titles":{}})", good, good).empty());
    const auto good_json = nlohmann::json::parse(good);
    for (const auto* field : {"title_id", "name", "status", "console", "fps_note", "jadyx_version",
                              "note", "source", "product"}) {
        auto missing = good_json;
        missing["titles"]["010028600EBDA000"][0].erase(field);
        assert(!Parse(missing.dump()));
        auto wrong_type = good_json;
        wrong_type["titles"]["010028600EBDA000"][0][field] = 123;
        assert(!Parse(wrong_type.dump()));
    }
    auto playable = good_json;
    playable["titles"]["010028600EBDA000"][0]["status"] = "playable";
    auto broken = good_json;
    broken["titles"]["010028600EBDA000"][0]["status"] = "broken";
    assert(Lookup(Select(playable.dump(), broken.dump(), good), 0x010028600EBDA000).status ==
           Status::Playable);
    assert(Lookup(Select("bad", broken.dump(), good), 0x010028600EBDA000).status == Status::Broken);
    assert(Lookup(*parsed, 0x010028600EBDA000, "S").status == Status::Untested);
    assert(Lookup(*parsed, 0x010028600EBDA000, "X").status == Status::Issues);
    auto mixed = good_json;
    auto s_report = playable["titles"]["010028600EBDA000"][0];
    s_report["console"] = "S";
    mixed["titles"]["010028600EBDA000"].push_back(s_report);
    assert(Lookup(*Parse(mixed.dump()), 0x010028600EBDA000).status == Status::Issues);
    assert(Lookup(*Parse(mixed.dump()), 0x010028600EBDA000, "S").status == Status::Playable);
    assert(!Parse(std::string(MaxBytes + 1, ' ')));
    assert(!Parse(good + "garbage"));
    auto mismatch = good_json;
    mismatch["titles"]["010028600EBDA000"][0]["title_id"] = "010028600EBDA800";
    assert(!Parse(mismatch.dump()));
    std::ifstream input{"dist/compat/compat.json"};
    assert(input);
    const std::string bundled{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    const auto seed = Parse(bundled);
    assert(seed && seed->size() == 4);
    assert(Lookup(*seed, 0x0100152000022800).status == Status::Playable);
    assert(Lookup(*seed, 0x01007EF00011E000).status == Status::Issues);
    assert(Lookup(*seed, 0x01005CA01580E000).status == Status::Untested);
    std::cout << "PASS compatibility parsing, unknown status, update matching, "
                 "provenance and fallback\n";
}
