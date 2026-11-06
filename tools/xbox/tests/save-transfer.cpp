// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/library/save_transfer.cpp, src/jadyx/mods/mod_archive.cpp,
// src/jadyx/mods/mod_layout.cpp and libarchive's reader (run-save-transfer.ps1 does).
// The portal's save archives: what an uploaded archive is taken as, and the export/import on disk.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "jadyx/library/save_transfer.h"
#include "stored_zip.h"

namespace {
using namespace Jadyx;
using Mods::ArchiveItem;
namespace fs = std::filesystem;

constexpr std::uint64_t Title = 0x0100AAAABBBB0000;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

std::vector<ArchiveItem> Items(const std::vector<std::string>& paths) {
    std::vector<ArchiveItem> items;
    for (const auto& path : paths) items.push_back({path, 4});
    return items;
}

/// The save paths a listing lands on, or "!<error>:<detail>".
std::set<std::string> Plan(const std::vector<std::string>& paths) {
    const auto plan = PlanSaveImport(Items(paths), Title);
    if (!plan.error.empty()) return {"!" + plan.error + ":" + plan.detail};
    std::set<std::string> to;
    for (const auto& file : plan.files) to.insert(file.to);
    return to;
}

void Expect(const std::vector<std::string>& paths, const std::set<std::string>& expected, const char* what) {
    const auto got = Plan(paths);
    if (got != expected) {
        std::printf("  %s got:", what);
        for (const auto& path : got) std::printf(" [%s]", path.c_str());
        std::printf("\n");
    }
    Check(got == expected, what);
}

void ExpectLike(const std::vector<std::string>& paths, const std::vector<std::string>& existing,
                const std::set<std::string>& expected, const char* what) {
    const auto plan = PlanSaveImport(Items(paths), Title, existing);
    std::set<std::string> got;
    if (!plan.error.empty()) got.insert("!" + plan.error + ":" + plan.detail);
    else for (const auto& file : plan.files) got.insert(file.to);
    if (got != expected) {
        std::printf("  %s got:", what);
        for (const auto& path : got) std::printf(" [%s]", path.c_str());
        std::printf("\n");
    }
    Check(got == expected, what);
}

void Write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << data;
}

std::string Read(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}

void WriteZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    const auto bytes = TestArchives::StoredZip(files);
    Write(path, std::string{bytes.begin(), bytes.end()});
}

std::size_t Count(const fs::path& folder) {
    std::size_t count = 0;
    std::error_code ec;
    for (fs::directory_iterator it{folder, ec}, end; it != end; it.increment(ec)) ++count;
    return count;
}

void PlanCases() {
    // The save's files as they are, and the folders they are usually zipped in.
    Expect({"main.dat", "slot/1.bin"}, {"main.dat", "slot/1.bin"}, "plain save");
    Expect({"0100AAAABBBB0000/main.dat", "0100AAAABBBB0000/slot/1.bin"}, {"main.dat", "slot/1.bin"},
           "title folder");
    Expect({"0100aaaabbbb0000/main.dat"}, {"main.dat"}, "title folder, lower case");
    Expect({"0100AAAABBBB0000-save/main.dat", "0100AAAABBBB0000-save/slot/1.bin"}, {"main.dat", "slot/1.bin"},
           "a downloaded save unpacked and zipped again");
    Expect({"0100AAAABBBB0000-save (1)/main.dat"}, {"main.dat"}, "a second copy of it");
    Expect({"0100FFFF00000000-save/main.dat"}, {"!wrong_game:0100FFFF00000000-save"},
           "a downloaded save of another game");
    Expect({"0100AAAABBBB00001/main.dat"}, {"!save_folder:0100AAAABBBB00001"}, "a longer number is not a title");
    Expect({"0100AAAABBBB0000x/main.dat"}, {"!save_folder:0100AAAABBBB0000x"}, "a longer name is not a title");
    Expect({"0/main.dat"}, {"main.dat"}, "newer layout's 0 folder");
    Expect({"0/main.dat", "1/old.dat"}, {"main.dat"}, "Ryujinx's 0 and 1");
    Expect({"0100AAAABBBB0000/0/main.dat"}, {"main.dat"}, "title then 0");
    Expect({"user/save/0000000000000000/0123456789ABCDEF0123456789ABCDEF/0100AAAABBBB0000/main.dat"},
           {"main.dat"}, "an emulator's whole tree");
    Expect({"bis/user/save/0000000000000001/0/main.dat", "bis/user/save/0000000000000001/1/main.dat"},
           {"main.dat"}, "Ryujinx's tree");
    Expect({"0100AAAABBBB0000/main.dat", "0100CCCC00000000/other.dat"}, {"main.dat"},
           "several games with ours");
    // With no save to compare with, a lone folder could be a wrapper or the game's own.
    Expect({"save/slot1.dat"}, {"!save_folder:save"}, "a lone folder with no save to compare");
    Expect({"partida/a/main.dat"}, {"!save_folder:partida"}, "lone folders with no save to compare");
    Expect({"0/a.dat", "2/b.dat"}, {"0/a.dat", "2/b.dat"}, "numbered slots stay");

    // The save being replaced tells where the new one starts, whatever the folders are called.
    const std::vector<std::string> slots{"0/game.sav", "1/game.sav", "option.sav"};
    ExpectLike({"mi partida/0/game.sav", "mi partida/1/game.sav", "mi partida/option.sav"}, slots,
               {"0/game.sav", "1/game.sav", "option.sav"}, "a renamed wrapper");
    ExpectLike({"a/b/c/0/game.sav", "a/b/c/option.sav"}, slots, {"0/game.sav", "option.sav"}, "several wrappers");
    ExpectLike({"MI PARTIDA/0/GAME.SAV"}, slots, {"0/GAME.SAV"}, "another case");
    ExpectLike({"mi partida/0/game.sav", "readme.txt"}, slots, {"0/game.sav"}, "a file beside the wrapper is dropped");
    ExpectLike({"0/game.sav", "1/game.sav", "option.sav"}, slots, {"0/game.sav", "1/game.sav", "option.sav"},
               "slots 0 and 1 are not Ryujinx's when the save has them");
    ExpectLike({"mi partida/5/new.sav"}, slots, {"!save_unknown:mi partida"}, "a lone folder unlike the save");
    ExpectLike({"nothing.dat", "alike.dat"}, slots, {"nothing.dat", "alike.dat"}, "unlike files at the root still load");
    const std::vector<std::string> own{"save/slot1.dat"};
    ExpectLike({"save/slot2.dat"}, own, {"save/slot2.dat"}, "a game's own folder stays");
    ExpectLike({"save/slot1.dat"}, own, {"save/slot1.dat"}, "a game's own folder stays, same files");
    ExpectLike({"copia/save/slot1.dat"}, own, {"save/slot1.dat"}, "a wrapper around a game's own folder");
    const std::vector<std::string> plain{"main.dat"};
    ExpectLike({"0/main.dat", "1/main.dat"}, plain, {"main.dat"}, "Ryujinx's 0 and 1 against the save");
    ExpectLike({"0100FFFF00000000/main.dat"}, plain, {"!wrong_game:0100FFFF00000000"},
               "another game with the same names");
    ExpectLike({"x/0100FFFF00000000-save/main.dat"}, plain, {"!save_unknown:x"},
               "another game's names inside a wrapper");
    Expect({"0100AAAABBBB0000/main.dat", "readme.txt"}, {"0100AAAABBBB0000/main.dat", "readme.txt"},
           "a folder beside files is not a wrapper");
    // What Mac and Windows add.
    Expect({"__MACOSX/0100AAAABBBB0000/._main.dat", "0100AAAABBBB0000/main.dat", "0100AAAABBBB0000/.DS_Store",
            "Thumbs.db", "0100AAAABBBB0000/desktop.ini"},
           {"main.dat"}, "junk ignored");

    // What is refused, and the detail the page shows.
    Expect({}, {"!empty:"}, "nothing");
    Expect({"__MACOSX/._a", ".DS_Store"}, {"!empty:"}, "only junk");
    Expect({"0100FFFF00000000/main.dat"}, {"!wrong_game:0100FFFF00000000"}, "another game");
    Expect({"0100FFFF00000000/a", "0100EEEE00000000/b"}, {"!wrong_game:0100EEEE00000000,0100FFFF00000000"},
           "other games");
    Expect({"user/save/0000000000000000/0123456789ABCDEF0123456789ABCDEF/0100FFFF00000000/a"},
           {"!wrong_game:0100FFFF00000000"}, "another game in a tree");
    Expect({"main.dat", "Game [0100AAAABBBB0000].nsp"}, {"!not_save:Game [0100AAAABBBB0000].nsp"}, "a package");
    Expect({"x/update.NCA"}, {"!not_save:update.NCA"}, "a content file");
    Expect({"../evil.dat"}, {"!archive_unsafe:../evil.dat"}, "a parent path");
    Expect({"/root.dat"}, {"!archive_unsafe:/root.dat"}, "an absolute path");
    Expect({"C:/x.dat"}, {"!archive_unsafe:C:/x.dat"}, "a drive path");
    Expect({"a\\..\\b"}, {"!archive_unsafe:a\\..\\b"}, "a backslash path");
    Expect({"a//b"}, {"!archive_unsafe:a//b"}, "an empty step");
    Expect({"Slot: 1.dat"}, {"!bad_name:Slot: 1.dat"}, "a colon in a name");
    Expect({"CON.txt"}, {"!bad_name:CON.txt"}, "a device name");
    Expect({"lpt1"}, {"!bad_name:lpt1"}, "a printer name");
    Expect({"a?b"}, {"!bad_name:a?b"}, "a refused character");
    Expect({"dot."}, {"!bad_name:dot."}, "a trailing dot");
    Expect({"folder /a"}, {"!bad_name:folder "}, "a trailing space");
    Expect({"Main.dat", "main.DAT"}, {"!bad_name:main.DAT"}, "names that differ in case");
    Expect({"x", "x/y"}, {"!bad_name:x"}, "a file and a folder of one name");
    Expect({"bad\x01name"}, {"!bad_name:bad?name"}, "a control character, shown cleaned");
    Expect({"a\xFF\xFE:b"}, {"!bad_name:a??:b"}, "invalid UTF-8 shown cleaned");

    std::vector<std::string> many;
    for (std::size_t i = 0; i <= SaveMaxFiles; ++i) many.push_back("f" + std::to_string(i));
    Expect(many, {"!save_limit:"}, "too many files");
    auto huge = PlanSaveImport({{"big.bin", SaveMaxImportBytes + 1}}, Title);
    Check(huge.error == "save_limit", "too many bytes");
    auto sized = PlanSaveImport({{"0100AAAABBBB0000/a", 10}, {"0100AAAABBBB0000/b", 20}}, Title);
    Check(sized.error.empty() && sized.bytes == 30 && sized.files.size() == 2, "plan bytes");
    Check(sized.files[0].from == "0100AAAABBBB0000/a", "plan keeps the archive path");
}

void DiskCases(const fs::path& root) {
    // An export holds the folder's files, nested ones and Unicode names included.
    const auto save = root / "save";
    Write(save / "main.dat", "MAIN");
    Write(save / "slot" / "1.bin", std::string(70000, 'x'));
    Write(save / fs::path{u8"r\u00e9sum\u00e9.dat"}, "U");
    fs::create_directories(save / "empty");
    std::string zip;
    Check(ExportSaveFolder(save, zip).empty(), "export");
    const auto exported = root / "export.zip";
    Write(exported, zip);
    {
        std::vector<std::unique_ptr<Mods::ArchiveSource>> volumes;
        volumes.push_back(Mods::OpenFileSource(exported));
        const auto listing = Mods::ListArchive(volumes);
        Check(listing.error == Mods::ArchiveError::None, "export lists");
        std::set<std::string> names;
        for (const auto& item : listing.items) names.insert(item.path + "=" + std::to_string(item.size));
        Check(names == std::set<std::string>{"main.dat=4", "slot/1.bin=70000", "r\xC3\xA9sum\xC3\xA9.dat=1"},
              "export holds the files");
    }
    Check(ExportSaveFolder(root / "missing", zip) == "no_save", "export of no folder");
    fs::create_directories(root / "nothing" / "inner");
    Check(ExportSaveFolder(root / "nothing", zip) == "no_save", "export of folders only");

    // Download then load: the save comes back and the one it replaces is kept.
    const auto target = root / "user" / "0100AAAABBBB0000";
    const auto backups = root / "backups";
    Write(target / "main.dat", "NEWER");
    Write(target / "extra.dat", "E");
    std::string log;
    Check(ImportSaveArchive(exported, Title, target, backups, {}, {}, &log).empty(), "import");
    Check(Read(target / "main.dat") == "MAIN" && Read(target / "slot" / "1.bin").size() == 70000 &&
              !fs::exists(target / "extra.dat"),
          "import replaces the save");
    Check(!fs::exists(root / "user" / "0100AAAABBBB0000.import"), "staging removed");
    const auto kept = backups / "0100AAAABBBB0000";
    Check(Count(kept) == 1, "one backup");
    const auto backup = fs::directory_iterator{kept}->path();
    Check(Read(backup / "main.dat") == "NEWER" && Read(backup / "extra.dat") == "E", "backup holds the old save");
    Check(log.find("kept") != std::string::npos, "log names the backup");

    // A save zipped in its title folder, into a game never played.
    const auto wrapped = root / "wrapped.zip";
    WriteZip(wrapped, {{"0100AAAABBBB0000/main.dat", "W"}, {"__MACOSX/0100AAAABBBB0000/._main.dat", "J"}});
    const auto fresh = root / "fresh" / "0100AAAABBBB0000";
    Check(ImportSaveArchive(wrapped, Title, fresh, backups, {}).empty(), "import into no save");
    Check(Read(fresh / "main.dat") == "W" && Count(fresh) == 1, "wrapper and junk dropped");
    Check(Count(kept) == 1, "no backup when there was no save");

    // A renamed folder, loaded over the save it was made from.
    const auto renamed = root / "renamed.zip";
    WriteZip(renamed, {{"mi partida/main.dat", "R"}, {"mi partida/slot/1.bin", "S"}});
    const auto slotted = root / "slotted" / "0100AAAABBBB0000";
    Write(slotted / "main.dat", "OLD");
    Check(ImportSaveArchive(renamed, Title, slotted, root / "slotted-backups", {}).empty(),
          "import of a renamed folder");
    Check(Read(slotted / "main.dat") == "R" && Read(slotted / "slot" / "1.bin") == "S" && Count(slotted) == 2,
          "renamed folder lands at the root");

    // Every refusal leaves the save as it was and says why.
    const auto unchanged = [&](const char* what) {
        Check(Read(target / "main.dat") == "MAIN" && !fs::exists(root / "user" / "0100AAAABBBB0000.import"), what);
    };
    const auto unknown = root / "unknown.zip";
    WriteZip(unknown, {{"mi partida/nothing.dat", "U"}});
    Check(ImportSaveArchive(unknown, Title, target, backups, {}) == "save_unknown:mi partida", "unknown folder word");
    unchanged("unknown folder keeps the save");
    Check(ImportSaveArchive(unknown, Title, root / "user" / "none", backups, {}) == "save_folder:mi partida",
          "lone folder with no save word");
    Check(!fs::exists(root / "user" / "none"), "lone folder with no save writes nothing");
    const auto other = root / "other.zip";
    WriteZip(other, {{"0100FFFF00000000/main.dat", "O"}});
    Check(ImportSaveArchive(other, Title, target, backups, {}) == "wrong_game:0100FFFF00000000", "wrong game word");
    unchanged("wrong game keeps the save");
    const auto package = root / "package.zip";
    WriteZip(package, {{"game.xci", "X"}});
    Check(ImportSaveArchive(package, Title, target, backups, {}) == "not_save:game.xci", "package word");
    unchanged("package keeps the save");
    const auto broken = root / "broken.zip";
    Write(broken, "PK\x03\x04 this is not really a zip");
    Check(ImportSaveArchive(broken, Title, target, backups, {}, {}, &log) == "archive", "damaged word");
    unchanged("damaged keeps the save");
    const auto cut = root / "cut.zip";
    Write(cut, zip.substr(0, zip.size() / 2));
    const auto cut_error = ImportSaveArchive(cut, Title, target, backups, {});
    Check(cut_error == "archive" || cut_error == "empty", "incomplete word");
    unchanged("incomplete keeps the save");
    Check(ImportSaveArchive(root / "gone.zip", Title, target, backups, {}) == "disk", "missing upload word");
    std::stop_source stop;
    stop.request_stop();
    Check(ImportSaveArchive(exported, Title, target, backups, stop.get_token()) == "interrupted", "cancelled word");
    unchanged("cancel keeps the save");
    Check(Count(kept) == 1, "refusals make no backup");

#ifdef _WIN32
    // The save cannot be moved aside (a file of it is open): nothing changes.
    {
        std::ifstream open{target / "main.dat", std::ios::binary};
        Check(ImportSaveArchive(wrapped, Title, target, backups, {}, {}, &log) == "disk", "busy save word");
    }
    unchanged("busy save is kept");
    Check(Count(kept) == 1, "busy save makes no backup");
#endif
}
} // namespace

int main() {
    PlanCases();
    const auto root = fs::temp_directory_path() / "jadyx-save-transfer";
    fs::remove_all(root);
    DiskCases(root);
    fs::remove_all(root);
    std::printf("save-transfer: ok\n");
    return 0;
}
