// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/nxbox_import_store.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

int main() {
    using namespace Jadyx::NxboxImport;
    assert(!ParseSettings("{"));
    assert(!ParseSettings(R"({"version":2})"));
    assert(!ParseSettings(R"({"version":true})"));
    const auto settings = ParseSettings(R"({"version":1,"language":"pt_br",
        "resolution":0.75,"controller_layout":"switch","confirm_button":"b",
        "per_game":{"0100000000000001":{"resolution":2}},"unknown":"ignored"})");
    assert(settings && settings->size() == 4);
    std::map<std::wstring, Value> stored;
    stored[L"system_ui_language"] = std::string{"es"};
    stored[L"controller_swap_face"] = false; // Explicitly saved default is still user-owned.
    auto apply = [&] {
        return ApplySettings(
            *settings, [&](const auto& key) { return stored.contains(key); },
            [&](const auto& key, const auto& value) { stored[key] = value; });
    };
    assert(apply() == 2);
    assert(std::get<std::string>(stored.at(L"system_ui_language")) == "es");
    assert(!std::get<bool>(stored.at(L"controller_swap_face")));
    assert(apply() == 0);
    const auto invalid = ParseSettings(R"({"version":1,"language":"xx","resolution":99,
        "controller_layout":false,"per_game":{"../bad":{"resolution":2}}})");
    assert(invalid && invalid->empty());
    assert(!ParseLibrary(R"({"version":1,"games":{}})"));
    const auto library = ParseLibrary(R"({"version":1,"games":[
        {"program_id":"0100000000000001","pinned":true,"seconds":42,"last_played":123,"sessions":2},
        {"program_id":"bad","seconds":100},
        {"program_id":"0100000000000002","seconds":-1}]})");
    assert(library && library->size() == 1);
    assert(library->front().seconds == 42 && library->front().pinned);
    assert(ParseLibrary(SerializeLibrary(*library))->front().seconds == 42);
    auto seconds = std::uint64_t{50};
    MergeCounters(library->front(), seconds);
    assert(seconds == 50);
    seconds = 0;
    MergeCounters(library->front(), seconds);
    MergeCounters(library->front(), seconds);
    assert(seconds == 42); // Retrying never doubles time.
    const auto folders = ParseUsbFolders("E:\\Games\r\nE:\\Games\n\nF:\\Jogos\n");
    assert(folders.size() == 2);
    assert(!ParseReceipt(R"({"version":1,"saves":-1})"));
    const auto receipt =
        ParseReceipt(R"({"version":1,"keys":true,"firmware":"19.0.0","saves":3,"mods":2})");
    assert(receipt && receipt->saves == 3 && receipt->mods == 2);
    assert(!ParseSettings(std::string(MaxFileBytes + 1, ' ')));
    const auto root = std::filesystem::temp_directory_path() /
                      ("jadyx-import-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{.path = root};
    std::filesystem::create_directories(root / "destination");
    assert(Jadyx::WriteFileAtomically(root / "destination" / "keep", "data"));
    assert(!Jadyx::WriteFileAtomically(root / "destination", "replacement"));
    std::filesystem::remove_all(root);
    const auto transfer = root / "nxbox-import";
    const auto pending_path = root / "jadyx" / "pending.json";
    auto has = [&](const auto& key) { return stored.contains(key); };
    auto write = [&](const auto& key, const auto& value) { stored[key] = value; };
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::Absent);
    assert(!std::filesystem::exists(root));
    assert(
        Jadyx::WriteFileAtomically(transfer / "settings.json", R"({"version":1,"language":"en"})"));
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::Invalid);
    assert(!std::filesystem::exists(transfer / "imported.flag"));
    assert(Jadyx::WriteFileAtomically(transfer / "library.json", SerializeLibrary(*library)));
    assert(Jadyx::WriteFileAtomically(transfer / "receipt.json", R"({"version":1})"));
    assert(Jadyx::WriteFileAtomically(root / "blocked", "file"));
    assert(ImportTransfer(transfer, root / "blocked" / "pending.json", has, write) ==
           ImportResult::StorageError);
    assert(!std::filesystem::exists(transfer / "imported.flag"));
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::Imported);
    assert(std::get<std::string>(stored.at(L"system_ui_language")) == "es");
    assert(ParseLibrary(*ReadTransfer(pending_path))->size() == 1);
    assert(Jadyx::WriteFileAtomically(transfer / "settings.json", "broken"));
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::AlreadyImported);
    std::filesystem::remove(transfer / "imported.flag");
    assert(
        Jadyx::WriteFileAtomically(transfer / "settings.json", R"({"version":1,"resolution":1})"));
    stored.clear();
    auto fail = [](const auto&, const auto&) { throw std::runtime_error("storage unavailable"); };
    assert(ImportTransfer(transfer, pending_path, has, fail) == ImportResult::StorageError);
    assert(!std::filesystem::exists(transfer / "imported.flag"));
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::Imported);
    // Failure after one key was saved, followed by a user edit before retry.
    std::filesystem::remove(transfer / "imported.flag");
    assert(Jadyx::WriteFileAtomically(transfer / "settings.json",
                                      R"({"version":1,"language":"en","resolution":0.5})"));
    stored.clear();
    auto partial = [&](const auto& key, const auto& value) {
        if (!stored.empty())
            throw std::runtime_error("second write failed");
        stored[key] = value;
    };
    assert(ImportTransfer(transfer, pending_path, has, partial) == ImportResult::StorageError);
    assert(!std::filesystem::exists(transfer / "imported.flag"));
    stored[L"system_ui_language"] = std::string{"es"};
    assert(ImportTransfer(transfer, pending_path, has, write) == ImportResult::Imported);
    assert(std::get<std::string>(stored.at(L"system_ui_language")) == "es");
    struct Entry {
        std::uint64_t program_id{};
        std::string launch_path;
    };
    std::vector<Entry> entries;
    struct Record {
        std::uint64_t seconds{}, program_id{};
        std::int64_t last_played{};
        std::uint32_t sessions{};
    };
    std::set<std::string> favorites;
    std::map<std::string, Record> history;
    assert(ResolveGames(*library, entries, favorites, history).size() == 1);
    assert(history.empty() && favorites.empty());
    entries.push_back(
        {.program_id = library->front().program_id, .launch_path = "E:\\Games\\demo.nro"});
    assert(ResolveGames(*library, entries, favorites, history).empty());
    assert(favorites.contains(entries.front().launch_path));
    assert(history.at(entries.front().launch_path).seconds == 42);
    assert(ResolveGames(*library, entries, favorites, history).empty());
    assert(history.at(entries.front().launch_path).seconds == 42);
    history.at(entries.front().launch_path).seconds = 100;
    assert(ResolveGames(*library, entries, favorites, history).empty());
    assert(history.at(entries.front().launch_path).seconds == 100);
    const auto deep = std::string{"{\"version\":1,\"unknown\":"} + std::string(50, '[') + "0" +
                      std::string(50, ']') + "}";
    assert(!ParseSettings(deep));
    std::vector<Game> maximum(4096);
    for (std::size_t i = 0; i < maximum.size(); ++i) {
        maximum[i].program_id = i + 1;
        maximum[i].seconds = UINT64_MAX;
        maximum[i].sessions = UINT32_MAX;
        maximum[i].last_played = INT64_MAX;
    }
    assert(SerializeLibrary(maximum).size() <= MaxFileBytes);
    assert(ParseLibrary(SerializeLibrary(maximum))->size() == maximum.size());
    std::printf("NXbox import parser, ownership, retry and title resolution passed; "
                "4096 maximum records use %zu bytes\n",
                SerializeLibrary(maximum).size());
}
