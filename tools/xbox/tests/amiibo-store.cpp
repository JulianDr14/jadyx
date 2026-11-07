// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The amiibo store: which uploads it keeps and where. Only made-up tags and keys, checked with the
// same AmiiboCrypto code the guest services use.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/hle/service/nfc/common/amiibo_crypto.h"
#include "jadyx/amiibo/amiibo_store.h"
#include "jadyx/storage/atomic_file.h"

namespace {

namespace fs = std::filesystem;
using namespace Jadyx;
using namespace Service::NFP;

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

EncryptedNTAG215File MakeTag() {
    EncryptedNTAG215File tag{};
    tag.uuid.part1 = {0x04, 0x11, 0x22};
    tag.uuid.crc_check1 = static_cast<u8>(0x88 ^ 0x04 ^ 0x11 ^ 0x22);
    tag.uuid.part2 = {0x33, 0x44, 0x55};
    tag.uuid.nintendo_id = 0x80;
    tag.uuid_crc_check2 = static_cast<u8>(0x33 ^ 0x44 ^ 0x55 ^ 0x80);
    tag.static_lock = 0xE00F;
    tag.compatibility_container = 0xEEFF10F1U;
    tag.user_memory.model_info.tag_type = Service::NFC::PackedTagType::Type2;
    tag.dynamic_lock = 0x0F0001U;
    tag.CFG0 = 0x04000000U;
    tag.CFG1 = 0x5F;
    return tag;
}

template <typename T>
std::string Bytes(const T& value) {
    return {reinterpret_cast<const char*>(&value), sizeof(value)};
}

std::string MakeKey() {
    AmiiboCrypto::InternalKey unfixed_info{}, locked_secret{};
    std::memcpy(unfixed_info.type_string.data(), "unfixed infos", 13);
    unfixed_info.magic_length = 14;
    std::memcpy(locked_secret.type_string.data(), "locked secret", 13);
    locked_secret.magic_length = 16;
    return Bytes(unfixed_info) + Bytes(locked_secret);
}

void Names() {
    Check(AmiiboFileName("figure.bin") == "figure.bin", "plain name");
    Check(AmiiboFileName("FIGURE.BIN") == "FIGURE.BIN", "extension in capitals");
    Check(AmiiboFileName("folder/sub\\figure.bin") == "figure.bin", "folders are dropped");
    Check(AmiiboFileName("../../figure.bin") == "figure.bin", "no way out of the folder");
    Check(AmiiboFileName("figure.txt").empty(), "wrong extension");
    Check(AmiiboFileName(".bin").empty(), "only an extension");
    Check(AmiiboFileName("").empty(), "empty name");
    Check(AmiiboFileName("folder/").empty(), "only a folder");
    Check(AmiiboFileName("a:b.bin").empty(), "stream separator");
    Check(AmiiboFileName("a?.bin").empty(), "wildcard");
    Check(AmiiboFileName(std::string{"a\tb.bin"}).empty(), "control character");
    Check(AmiiboFileName("CON.bin").empty(), "device name");
    Check(AmiiboFileName("lpt1.bin").empty(), "numbered device name");
    Check(!AmiiboFileName("console.bin").empty(), "a name that only starts like a device");
    Check(AmiiboFileName(std::string(97, 'a') + ".bin").empty(), "too long");
    Check(!AmiiboFileName(std::string(96, 'a') + ".bin").empty(), "longest name");
}

void Images() {
    const auto encrypted = Bytes(MakeTag());
    const auto plain = Bytes(AmiiboCrypto::NfcDataToEncodedData(MakeTag()));
    Check(encrypted != plain, "the two layouts differ");
    Check(IsAmiiboImage(encrypted), "encrypted dump");
    Check(IsAmiiboImage(plain), "decrypted dump");
    Check(IsAmiiboImage(encrypted.substr(0, 532)), "dump without the password");
    Check(IsAmiiboImage(encrypted + std::string(0x20, '\x7f')), "dump with the signature");

    Check(!IsAmiiboImage(""), "nothing");
    Check(!IsAmiiboImage(std::string(540, '\0')), "right size, empty");
    Check(!IsAmiiboImage(encrypted.substr(0, 539)), "one byte short");
    Check(!IsAmiiboImage(encrypted + "x"), "one byte long");
    Check(!IsAmiiboImage(std::string(1024, '\0')), "other tag types");
    auto wrong_uuid = encrypted;
    wrong_uuid[3] ^= 1;
    Check(!IsAmiiboImage(wrong_uuid), "broken uuid check");
    auto wrong_type = MakeTag();
    wrong_type.user_memory.model_info.tag_type = Service::NFC::PackedTagType::None;
    Check(!IsAmiiboImage(Bytes(wrong_type)), "other tag type in the model info");
}

void Keys() {
    const auto key = MakeKey();
    Check(IsAmiiboKey(key), "key");
    Check(!IsAmiiboKey(key.substr(0, 159)), "short key");
    Check(!IsAmiiboKey(key + "x"), "long key");
    Check(!IsAmiiboKey(std::string(160, '\0')), "right size, empty");
    Check(!IsAmiiboKey(key.substr(80) + key.substr(0, 80)), "keys swapped");
    auto bad_magic = key;
    bad_magic[0x1F] = 17; // magic_length of the first key
    Check(!IsAmiiboKey(bad_magic), "magic longer than its field");
}

void Store() {
    const auto root = fs::temp_directory_path() / "jadyx-amiibo-store-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto key_path = root / "keys" / "key_retail.bin";
    AmiiboStore store{root / "amiibo" / "files", key_path};
    const auto tag = Bytes(MakeTag());

    Check(store.List().empty(), "no folder yet");
    Check(store.Add("../figure.bin", tag, false).empty(), "adds");
    Check(fs::exists(root / "amiibo" / "files" / "figure.bin"), "kept inside the folder");
    Check(store.Add("b.txt", tag, false) == "name", "refuses a name");
    Check(store.Add("b.bin", "nope", false) == "not_amiibo", "refuses other files");
    Check(store.Add("b.bin", MakeKey(), false) == "not_amiibo", "refuses the key as an amiibo");
    Check(!fs::exists(root / "amiibo" / "files" / "b.bin"), "nothing kept when refused");

    auto changed = MakeTag();
    changed.user_memory.keygen_salt[0] = 1;
    Check(store.Add("FIGURE.bin", Bytes(changed), false) == "exists", "same name, other case");
    Check(*ReadWholeFile(root / "amiibo" / "files" / "figure.bin") == tag, "not overwritten");
    Check(store.Add("figure.bin", Bytes(changed), true).empty(), "replaces when asked");
    Check(*ReadWholeFile(root / "amiibo" / "files" / "figure.bin") == Bytes(changed), "replaced");

    const std::string accented = "Pok\xC3\xA9mon.bin";
    Check(store.Add(accented, tag, false).empty(), "accented name");
    Check(fs::exists(root / "amiibo" / "files" / fs::path{u8"Pokémon.bin"}), "stored as UTF-8");
    Check(store.PathOf(accented).ends_with(accented), "path for the core is UTF-8");
    Check(store.Add("bad\xC3.bin", tag, false) == "name", "invalid UTF-8");
    Check(store.PathOf("bad\xC3.bin").empty(), "no path for invalid UTF-8");
    Check(store.PathOf("x.txt").empty(), "no path for a bad name");

    // Other files in the folder are not offered to the game.
    Check(WriteFileAtomically(root / "amiibo" / "files" / "notes.txt", "hello"), "test file");
    Check(store.Add("Alpha.bin", tag.substr(0, 532), false).empty(), "short dump");
    const auto list = store.List();
    Check(list.size() == 3, "three files");
    if (list.size() == 3) {
        Check(list[0] == AmiiboFile{"Alpha.bin", 532}, "sorted, its real size");
        Check(list[1].name == "figure.bin", "sorted without case");
        Check(list[2] == AmiiboFile{accented, 540}, "accented name listed");
    }

    Check(store.Remove("figure.bin").empty(), "removes");
    Check(store.Remove("figure.bin") == "missing", "already gone");
    Check(store.Remove("..\\..\\keys\\key_retail.txt") == "name", "not a stored name");

    Check(!store.HasKey(), "no key");
    Check(store.SetKey("short") == "not_key", "refuses a wrong key");
    Check(!store.HasKey(), "still no key");
    Check(store.SetKey(MakeKey()).empty(), "stores the key");
    Check(store.HasKey(), "has the key");
    Check(*ReadWholeFile(key_path) == MakeKey(), "exact key bytes");
    Check(store.RemoveKey().empty(), "removes the key");
    Check(!store.HasKey(), "key gone");
    Check(store.RemoveKey().empty(), "nothing to remove is fine");

    fs::remove_all(root, ec);
}

} // namespace

int main() {
    Names();
    Images();
    Keys();
    Store();
    if (failures != 0) {
        std::printf("amiibo-store: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("amiibo-store: ok\n");
    return 0;
}
