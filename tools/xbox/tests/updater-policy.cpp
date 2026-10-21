// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/updates/release_key.h"
#include "jadyx/updates/update_policy.h"
#include "jadyx/updates/update_ui.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
using namespace Jadyx::Updates;
int main(int argc, char **argv) {
  if (argc == 3) {
    const auto read = [](const char *path) {
      std::ifstream input{path, std::ios::binary};
      return std::string{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
    };
    assert(VerifyManifest(read(argv[1]), read(argv[2]), ReleasePublicKey, {},
                          Channel::Preview));
    std::cout << "actual release metadata signature: OK\n";
  }
  assert(ParseVersion("0.3.6.0") == Version({0, 3, 6, 0}));
  for (const auto text : {"", "v0.3.6.0", "0.3.6", "0.3.6.0.0", "00.3.6.0",
                          "0.-3.6.0", "0.3.65536.0", "0.3.6.0junk", "0.3.6. 0"})
    assert(!ParseVersion(text));
  assert(*ParseVersion("0.3.10.0") > *ParseVersion("0.3.9.9"));
  assert(FormatVersion(*ParseVersion("65535.65535.65535.65535")) ==
         "65535.65535.65535.65535");
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator{
      EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
  assert(generator && EVP_PKEY_keygen_init(generator.get()) == 1 &&
         EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
  EVP_PKEY *raw{};
  assert(EVP_PKEY_keygen(generator.get(), &raw) == 1);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{raw, EVP_PKEY_free};
  std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()), BIO_free};
  assert(PEM_write_bio_PUBKEY(bio.get(), key.get()) == 1);
  char *data{};
  auto size = BIO_get_mem_data(bio.get(), &data);
  std::string pem{data, static_cast<std::size_t>(size)};
  const auto sign = [&](const std::string &body) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{
        EVP_MD_CTX_new(), EVP_MD_CTX_free};
    assert(EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                              key.get()) == 1);
    std::size_t length{};
    assert(EVP_DigestSign(ctx.get(), nullptr, &length,
                          reinterpret_cast<const unsigned char *>(body.data()),
                          body.size()) == 1);
    std::string sig(length, '\0');
    assert(EVP_DigestSign(
               ctx.get(), reinterpret_cast<unsigned char *>(sig.data()),
               &length, reinterpret_cast<const unsigned char *>(body.data()),
               body.size()) == 1);
    sig.resize(length);
    return sig;
  };
  using Json = nlohmann::json;
  const Json valid{{"schema", 1},
                   {"version", "0.3.6.0"},
                   {"tag", "v0.3.6.0"},
                   {"channel", "stable"},
                   {"identity",
                    {{"name", "Jadyx"},
                     {"publisher", "CN=JadyxDev"},
                     {"architecture", "x64"}}},
                   {"diagnostic", false},
                   {"min_os", "10.0.17763.0"},
                   {"vclibs_min", "14.0.33519.0"},
                   {"url", "https://github.com/JulianDr14/jadyx/releases/"
                           "download/v0.3.6.0/Jadyx_0.3.6.0_x64.appx"},
                   {"sha256", std::string(64, 'a')},
                   {"size", 1024},
                   {"notes", "Notas: español y português"},
                   {"published", "2026-10-19T00:00:00Z"}};
  const auto body = valid.dump(), signature = sign(body);
  const Version installed{0, 3, 5, 0};
  assert(VerifyManifest(body, signature, pem, installed, Channel::Stable));
  assert(
      !VerifyManifest(body + " ", signature, pem, installed, Channel::Stable));
  assert(!VerifyManifest(body, signature.substr(1), pem, installed,
                         Channel::Stable));
  assert(!VerifyManifest(body, signature, "", installed, Channel::Stable));
  assert(!VerifyManifest(body, signature, pem, {0, 3, 6, 0}, Channel::Stable));
  assert(!VerifyManifest(body, signature, pem, {0, 3, 7, 0}, Channel::Stable));
  unsigned rejected = 0;
  const auto reject = [&](Json value) {
    const auto text = value.dump();
    assert(!VerifyManifest(text, sign(text), pem, installed, Channel::Stable));
    ++rejected;
  };
  for (const auto field : {"schema", "version", "tag", "channel", "identity",
                           "diagnostic", "min_os", "vclibs_min", "url",
                           "sha256", "size", "notes", "published"}) {
    auto j = valid;
    j.erase(field);
    reject(j);
    j = valid;
    j[field] = nullptr;
    reject(j);
  }
  for (const auto url : {"http://github.com/JulianDr14/jadyx/releases/download/"
                         "v0.3.6.0/Jadyx_0.3.6.0_x64.appx",
                         "https://github.com/evil/jadyx/releases/download/"
                         "v0.3.6.0/Jadyx_0.3.6.0_x64.appx",
                         "https://github.com/JulianDr14/jadyx/releases/"
                         "download/v0.3.6.0/Jadyx_0.3.6.0_x64.appx?evil"}) {
    auto j = valid;
    j["url"] = url;
    reject(j);
  }
  for (const auto size : {0ull, (4ull << 30) + 1}) {
    auto j = valid;
    j["size"] = size;
    reject(j);
  }
  for (const auto field : {"size", "schema"}) {
    auto j = valid;
    j[field] = 8.5;
    reject(j);
  }
  for (const auto field : {"name", "publisher", "architecture"}) {
    auto j = valid;
    j["identity"][field] = "wrong";
    reject(j);
  }
  {
    auto j = valid;
    j["diagnostic"] = true;
    reject(j);
    j = valid;
    j["sha256"] = std::string(64, 'A');
    reject(j);
    j = valid;
    j["notes"] = std::string(16385, 'a');
    reject(j);
  }
  {
    auto j = valid;
    j["channel"] = "preview";
    reject(j);
    auto b = j.dump();
    assert(VerifyManifest(b, sign(b), pem, installed, Channel::Preview));
  }
  assert(VerifyManifest(body, signature, pem, installed, Channel::Preview));
  assert(ReleaseNotesText("## What's new\r\n\r\n- **Fix** the `updater`\r\n"
                          "  continued line\r\n* See [docs](https://x)\r\n\r\n\r\nEnd\r\n") ==
         "What's new\n\n\xE2\x80\xA2 Fix the updater continued line\n\xE2\x80\xA2 See docs\n\nEnd");
  assert(ReleaseNotesText(std::string(20000, 'a')).size() == 16384);
  Snapshot state;
  state.phase = Phase::Downloading;
  assert(std::ranges::equal(
      Actions(state), std::array{Action::Pause, Action::Cancel, Action::Back}));
  state.phase = Phase::Requested;
  assert(std::ranges::equal(Actions(state), std::array{Action::Back}));
  state.phase = Phase::Verifying;
  assert(std::ranges::equal(Actions(state), std::array{Action::Back}));
  std::cout << "updater policy: signatures, versions, " << rejected
            << " malformed signed manifests, channel, notes and UI states OK\n";
}
