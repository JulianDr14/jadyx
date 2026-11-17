// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop gate for the Nextendo pieces that need no server: name rules, hosts file, address book,
// id_token, signed catalog, trust roots, PKCE, API parsing and the rooms/Nextendo arbiter.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "core/online/nextendo/address_book.h"
#include "core/online/nextendo/catalog.h"
#include "core/online/nextendo/config.h"
#include "core/online/nextendo/encoding.h"
#include "core/online/nextendo/host_rules.h"
#include "core/online/nextendo/id_token.h"
#include "core/online/nextendo/trust_roots.inc"
#include "core/online/nextendo/trust_store.h"
#include "jadyx/network/nextendo/api_types.h"
#include "jadyx/network/nextendo/network_mode.h"
#include "jadyx/network/nextendo/pkce.h"

using namespace Core::Online::Nextendo;
namespace App = Jadyx::Network::Nextendo;

namespace {

int g_checks = 0;

void Check(bool condition, std::source_location where = std::source_location::current()) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%u\n", where.file_name(), static_cast<unsigned>(where.line()));
        std::exit(1);
    }
}

std::string Base64Standard(std::string_view data) {
    std::string out(((data.size() + 2) / 3) * 4 + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(data.data()),
                                  static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string Base64UrlDecodeToString(std::string part) {
    for (auto& c : part) {
        c = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    while (part.size() % 4 != 0) {
        part += '=';
    }
    const auto bytes = Base64Decode(part);
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
}

void TestHostRules() {
    Check(WildcardMatch("*.nintendo.net", "a.b.nintendo.net"));
    Check(WildcardMatch("*.nintendo.net", "A.NINTENDO.NET"));
    Check(!WildcardMatch("*.nintendo.net", "nintendo.net"));
    Check(!WildcardMatch("*.nintendo.net", "evilnintendo.net.example.com"));
    Check(WildcardMatch("nncs2-*.n.n.srv.nintendo.net", "nncs2-lp1.n.n.srv.nintendo.net"));
    Check(WildcardMatch("pvzgw3prodapp-*.ea.com", "pvzgw3prodapp-12.ea.com"));
    Check(!WildcardMatch("pvzgw3prodapp-*.ea.com", "ea.com"));

    Check(IsLiteralIPv4("10.0.0.1"));
    Check(IsLiteralIPv4("255.255.255.255"));
    Check(!IsLiteralIPv4("256.0.0.1"));
    Check(!IsLiteralIPv4("1.2.3"));
    Check(!IsLiteralIPv4("1.2.3.4.5"));
    Check(!IsLiteralIPv4("01.2.3.4"));
    Check(!IsLiteralIPv4("a.b.c.d"));
    Check(!IsLiteralIPv4(""));

    Check(RewriteAccountHost("accounts.nintendo.com") == kBaasHost);
    Check(RewriteAccountHost("API.Accounts.Nintendo.com.") == kBaasHost);
    Check(RewriteAccountHost("example.com") == "example.com");

    Check(MatchBuiltinRule("nncs2-lp1.n.n.srv.nintendo.net") == RuleTarget::Nat);
    Check(MatchBuiltinRule("g2b309e01-lp1.s.n.srv.nintendo.net") == RuleTarget::Nat);
    Check(MatchBuiltinRule("g2df33d01-lp1.p.srv.nintendo.net") == RuleTarget::Nat);
    Check(MatchBuiltinRule("api.sect.srv.nintendo.net") == RuleTarget::Server);
    Check(MatchBuiltinRule("Atum.HAC.LP1.D4C.Nintendo.net.") == RuleTarget::Server);
    Check(MatchBuiltinRule("conntest.nintendowifi.net") == RuleTarget::Server);
    Check(MatchBuiltinRule("x.nintendo.co.jp") == RuleTarget::Server);
    Check(MatchBuiltinRule("demonware.net") == RuleTarget::Server);
    Check(MatchBuiltinRule("a.demonware.net") == RuleTarget::Server);
    Check(MatchBuiltinRule("eu.ultimatechickenhorseserver.com") == RuleTarget::Server);
    Check(MatchBuiltinRule("us.ultimatechickenhorseserver.com") == RuleTarget::None);
    Check(MatchBuiltinRule("nintendo.net") == RuleTarget::None);
    Check(MatchBuiltinRule("example.com") == RuleTarget::None);
    Check(MatchBuiltinRule("ea.com") == RuleTarget::None);

    Check(FilterAlpn("api.lp1.npln.srv.nintendo.net", {"h2", "http/1.1"}) ==
          std::vector<std::string>{"h2", "http/1.1"});
    Check(FilterAlpn("x.gs.nintendo.net", {"h2"}) == std::vector<std::string>{"h2"});
    Check(FilterAlpn("api.example.com", {"h2", "http/1.1"}) ==
          std::vector<std::string>{"http/1.1"});
    Check(FilterAlpn("npln.example", {}) == std::vector<std::string>{"http/1.1"});
    Check(IsPlainHttpHost("ctest.cdn.nintendo.net"));
    Check(!IsPlainHttpHost("cdn.nintendo.net"));
}

void TestHostsFile() {
    const auto hosts = HostsFile::Parse(
        "# comment line\n"
        "10.1.1.1 exact.nintendo.net # trailing comment\n"
        "10.2.2.2\tone.example two.example\r\n"
        "10.3.3.3 api-%.example\n"
        "10.4.4.4 *.example\n"
        "10.5.5.5 *.deep.example\n"
        "not-an-ip broken.example\n"
        "10.6.6.6\n");
    Check(!hosts.Empty());
    Check(hosts.FindExact("exact.nintendo.net") == "10.1.1.1");
    Check(hosts.FindExact("EXACT.nintendo.net.") == "10.1.1.1");
    Check(hosts.FindExact("one.example") == "10.2.2.2");
    Check(hosts.FindExact("two.example") == "10.2.2.2");
    Check(hosts.FindExact("api-lp1.example") == "10.3.3.3");
    Check(!hosts.FindExact("broken.example"));
    Check(hosts.FindWildcard("a.deep.example") == "10.5.5.5"); // more specific wins
    Check(hosts.FindWildcard("a.other.example") == "10.4.4.4");
    Check(!hosts.FindWildcard("example.org"));
    Check(HostsFile::Parse("").Empty());

    const Endpoints configured{"192.0.2.10", "192.0.2.20"};
    const Endpoints server_only{"192.0.2.10", ""};
    const Endpoints none{};
    // 1. literal
    Check(ResolveHost(" 8.8.8.8 ", hosts, configured) == "8.8.8.8");
    // 2. exact hosts entry beats the built-in rule
    Check(ResolveHost("exact.nintendo.net", hosts, configured) == "10.1.1.1");
    // 3. built-in rules
    Check(ResolveHost("other.nintendo.net", hosts, configured) == "192.0.2.10");
    Check(ResolveHost("nncs2-lp1.n.n.srv.nintendo.net", hosts, configured) == "192.0.2.20");
    Check(ResolveHost("nncs2-lp1.n.n.srv.nintendo.net", hosts, server_only) == "192.0.2.10");
    Check(ResolveHost("other.nintendo.net", hosts, none) == kUnconfiguredIp);
    Check(ResolveHost("nncs2-lp1.n.n.srv.nintendo.net", hosts, none) == kUnconfiguredIp);
    // 4. wildcard hosts entry, only after the built-in rules
    const auto wild = HostsFile::Parse("10.9.9.9 *.nintendo.net\n10.8.8.8 *.example\n");
    Check(ResolveHost("x.nintendo.net", wild, configured) == "192.0.2.10");
    Check(ResolveHost("x.example", wild, configured) == "10.8.8.8");
    // 5. real DNS
    Check(!ResolveHost("example.org", hosts, configured));
}

void TestAddressBook() {
    AddressBook book{2};
    book.Remember("1.1.1.1", "a.example");
    book.Remember("2.2.2.2", "b.example");
    Check(book.Lookup("1.1.1.1") == "a.example");
    book.Remember("1.1.1.1", "c.example"); // newer name wins and refreshes age
    Check(book.Lookup("1.1.1.1") == "c.example");
    book.Remember("3.3.3.3", "d.example"); // evicts the oldest (2.2.2.2)
    Check(!book.Lookup("2.2.2.2"));
    Check(book.Lookup("3.3.3.3") == "d.example");
    Check(book.Size() == 2);
    book.Remember("4.4.4.4", "4.4.4.4"); // literal names are not SNI material
    Check(!book.Lookup("4.4.4.4"));
    book.Clear();
    Check(book.Size() == 0);

    ConfigStore store;
    Check(!store.Enabled());
    const auto before = store.Snapshot();
    Config next;
    next.enabled = true;
    next.endpoints.server_ip = "192.0.2.10";
    store.Publish(next);
    Check(store.Enabled());
    Check(!before->enabled); // old snapshots stay intact
}

void TestEncoding() {
    Check(Base64UrlEncode(std::string_view{""}).empty());
    Check(Base64UrlEncode(std::string_view{"f"}) == "Zg");
    Check(Base64UrlEncode(std::string_view{"fo"}) == "Zm8");
    Check(Base64UrlEncode(std::string_view{"foo"}) == "Zm9v");
    Check(Base64UrlEncode(std::string_view{"\xfb\xff"}) == "-_8");
    const auto decoded = Base64Decode("Zm9vYmE=");
    Check(decoded && std::string(decoded->begin(), decoded->end()) == "fooba");
    Check(!Base64Decode("Zm9v!"));
    Check(!Base64Decode("Zg=a"));
    Check(RandomBytes(32).size() == 32);
}

void TestPkce() {
    // Expected value computed independently with Python hashlib + urlsafe_b64encode.
    Check(App::ChallengeFor("dBjftJeZ4CVP-mJ92K9qpvXjhJIUrGUfVRj6b8Ha3wM") ==
          "S8H_1EQCc7xSKyYtrwgVZze1IAhCkRTUNhEY4Hpw8H0");
    const auto pair = App::MakePkcePair();
    Check(pair && pair->verifier.size() == 64 && pair->challenge.size() == 43);
    Check(App::ChallengeFor(pair->verifier) == pair->challenge);
    const auto other = App::MakePkcePair();
    Check(other && other->verifier != pair->verifier);
    Check(!App::MakeState().empty());

    Check(App::UrlEncode("a b/c:~") == "a%20b%2Fc%3A~");
    const std::string url = App::BuildAuthorizeUrl(App::kRedirectUri, "st", "ch");
    Check(url.rfind("https://nextendo.network/api/oauth/authorize?response_type=code", 0) == 0);
    Check(url.find("client_id=nxc_dXdnliZpNaNT") != std::string::npos);
    Check(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%2Fcallback") != std::string::npos);
    Check(url.find("scope=game.matchmaking%20identity%20presence") != std::string::npos);
    Check(url.find("code_challenge=ch&code_challenge_method=S256") != std::string::npos);
    Check(App::BuildCodeExchangeBody("c d", App::kRedirectUri, "v") ==
          "grant_type=authorization_code&code=c%20d&client_id=nxc_dXdnliZpNaNT"
          "&redirect_uri=http%3A%2F%2F127.0.0.1%2Fcallback&code_verifier=v");
    Check(App::BuildRefreshBody("r+t") ==
          "grant_type=refresh_token&refresh_token=r%2Bt&client_id=nxc_dXdnliZpNaNT");

    auto cb = App::ParseCallback("http://127.0.0.1/callback?code=abc%2B1&state=xyz", "xyz");
    Check(cb.code == "abc+1" && cb.error.empty());
    cb = App::ParseCallback("http://127.0.0.1/callback?code=abc&state=bad", "xyz");
    Check(cb.code.empty() && cb.error == "state_mismatch");
    cb = App::ParseCallback("http://127.0.0.1/callback?error=access_denied&state=xyz", "xyz");
    Check(cb.error == "access_denied");
    cb = App::ParseCallback("http://127.0.0.1/callback?state=xyz", "xyz");
    Check(cb.error == "missing_code");
    cb = App::ParseCallback("http://127.0.0.1/callback?code=abc", "xyz");
    Check(cb.error == "state_mismatch");
    cb = App::ParseCallback("http://127.0.0.1/callback?code=abc&state=", "");
    Check(cb.error == "state_mismatch");
}

void TestIdToken() {
    auto signer = RsaSigner::Generate("jadyx-baas-1");
    Check(signer != nullptr);
    const std::string pem = signer->PrivatePem();
    Check(pem.find("PRIVATE KEY") != std::string::npos);
    auto reloaded = RsaSigner::FromPem("jadyx-baas-1", pem);
    Check(reloaded != nullptr);
    Check(RsaSigner::FromPem("x", "garbage") == nullptr);

    IdTokenRequest request;
    request.title_id = 0x0100D870045B6000ULL;
    request.title_version = "1.2.0";
    request.nex_token = "nx2.opaque\"value";
    request.issued_at = 1'800'000'000;
    const auto token = BuildIdToken(request, *signer);
    Check(token.has_value());

    const std::size_t dot1 = token->find('.');
    const std::size_t dot2 = token->find('.', dot1 + 1);
    Check(dot1 != std::string::npos && dot2 != std::string::npos);
    Check(token->find('.', dot2 + 1) == std::string::npos);
    Check(token->find('=') == std::string::npos);

    const auto header = nlohmann::json::parse(Base64UrlDecodeToString(token->substr(0, dot1)));
    Check(header["alg"] == "RS256" && header["typ"] == "id_token" && header["kid"] == "jadyx-baas-1");

    const auto payload =
        nlohmann::json::parse(Base64UrlDecodeToString(token->substr(dot1 + 1, dot2 - dot1 - 1)));
    Check(payload["iss"] == "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com");
    Check(payload["aud"] == "ed9e2f05d286f7b8");
    Check(payload["typ"] == "id_token");
    Check(payload["iat"] == 1'800'000'000);
    Check(payload["exp"] == 1'800'000'000 + 3 * 3600);
    Check(payload["sn"] == "XAW10000000000");
    Check(payload["hm"] == true);
    Check(payload["nnex"] == "nx2.opaque\"value");
    Check(payload["tv"] == "1.2.0");
    Check(payload["app_id"] == "0100D870045B6000");
    Check(payload["sub"].get<std::string>().size() == 32);
    Check(payload["di"].get<std::string>().size() == 32);
    Check(payload["bs:did"].get<std::string>().size() == 32);
    Check(payload["nintendo"]["di"] == payload["di"]);
    const std::string jti = payload["jti"];
    Check(jti.size() == 36 && jti[8] == '-' && jti[14] == '4');

    const std::string signing_input = token->substr(0, dot2);
    const std::string sig_text = Base64UrlDecodeToString(token->substr(dot2 + 1));
    const std::vector<std::uint8_t> signature(sig_text.begin(), sig_text.end());
    Check(signature.size() == 256);
    Check(reloaded->VerifyRs256(signing_input, signature));
    Check(!reloaded->VerifyRs256(signing_input + "x", signature));

    IdTokenRequest plain;
    plain.title_id = 0x0100000000010000ULL;
    plain.issued_at = 1;
    const auto token2 = BuildIdToken(plain, *signer);
    Check(token2.has_value());
    const std::size_t a = token2->find('.');
    const std::size_t b = token2->find('.', a + 1);
    const auto payload2 = nlohmann::json::parse(Base64UrlDecodeToString(token2->substr(a + 1, b - a - 1)));
    Check(!payload2.contains("nnex") && !payload2.contains("tv") && !payload2.contains("app_id"));
    Check(payload2["sub"] != payload["sub"]);

    Check(NeedsAppIdClaim(0x0100A3D008C5C000ULL));
    Check(!NeedsAppIdClaim(0));
}

struct Ed25519Key {
    EVP_PKEY* key = nullptr;
    std::string public_b64;
};

Ed25519Key MakeEd25519() {
    Ed25519Key out;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    Check(ctx != nullptr && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &out.key) == 1);
    EVP_PKEY_CTX_free(ctx);
    unsigned char raw[32];
    std::size_t len = sizeof(raw);
    Check(EVP_PKEY_get_raw_public_key(out.key, raw, &len) == 1 && len == 32);
    out.public_b64 = Base64Standard(std::string_view(reinterpret_cast<char*>(raw), len));
    return out;
}

std::string SignEnvelope(const Ed25519Key& key, std::string_view key_id, std::string_view payload) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char sig[64];
    std::size_t len = sizeof(sig);
    Check(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key.key) == 1);
    Check(EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size()) == 1);
    EVP_MD_CTX_free(ctx);
    nlohmann::json envelope;
    envelope["key_id"] = key_id;
    envelope["payload"] = Base64Standard(payload);
    envelope["signature"] = Base64Standard(std::string_view(reinterpret_cast<char*>(sig), len));
    return envelope.dump();
}

void TestCatalog() {
    Check(CompareVersions("4.0.1", "4.0.1.0") == 0);
    Check(CompareVersions("1.10", "1.9") > 0);
    Check(CompareVersions("1.9", "1.10") < 0);
    Check(CompareVersions("2.7.7.92380", "2.7.7.92380") == 0);
    Check(CompareVersions("1.0.0", "1.0.1") < 0);
    Check(CompareVersions("1.0a", "1.0b") < 0);

    Check(ParseUtcTimestamp("1970-01-01T00:00:00Z") == 0);
    Check(ParseUtcTimestamp("2026-11-16T19:09:07.183287Z") == 1794856147);
    Check(ParseUtcTimestamp("2000-02-29T12:00:00Z") == 951825600);
    Check(!ParseUtcTimestamp("2026-11-16 19:09:07"));
    Check(!ParseUtcTimestamp("2026-13-16T19:09:07Z"));
    Check(!ParseUtcTimestamp("2026-11-16T19:09:07.Z"));

    const std::string payload = R"({"document_type":"game_catalog","schema_version":1,"revision":7,
        "issued_at":"2026-11-16T00:00:00Z","expires_at":"2026-11-18T00:00:00Z","games":[
        {"title_ids":["0100000000010000","0100000000010800"],"name":"Sample Game","enabled":true,"protocol":"nex",
         "required_capabilities":["versions.exact.v1"],
         "versions":{"policy":"exact","accepted":["1.2.0","1.3"],"recommended":"1.3"}},
        {"title_ids":["0100000000020000"],"enabled":false,"protocol":"npln",
         "required_capabilities":["versions.exact.v1"],"versions":{"policy":"exact","accepted":[]}},
        {"title_ids":["0100000000030000"],"enabled":true,
         "required_capabilities":["versions.exact.v1","future.thing.v9"],"versions":{"policy":"exact"}},
        {"title_ids":["bad"],"enabled":true}
        ]})";
    const auto key = MakeEd25519();
    const SigningKey test_key{"test-key-1", key.public_b64};
    const std::string envelope = SignEnvelope(key, "test-key-1", payload);

    const auto verified = VerifySignedDocument(envelope, test_key);
    Check(verified.error == SignedDocumentError::None && verified.payload == payload);
    Check(VerifySignedDocument(envelope, kCatalogKey).error == SignedDocumentError::WrongKey);
    Check(VerifySignedDocument(SignEnvelope(key, "test-key-1", payload + " "), test_key).error ==
          SignedDocumentError::None);
    auto tampered = nlohmann::json::parse(envelope);
    tampered["payload"] = Base64Standard(payload + " ");
    Check(VerifySignedDocument(tampered.dump(), test_key).error == SignedDocumentError::BadSignature);
    Check(VerifySignedDocument("not json", test_key).error == SignedDocumentError::Malformed);
    const auto other = MakeEd25519();
    Check(VerifySignedDocument(SignEnvelope(other, "test-key-1", payload), test_key).error ==
          SignedDocumentError::BadSignature);
    EVP_PKEY_free(other.key);
    EVP_PKEY_free(key.key);

    const std::int64_t now = *ParseUtcTimestamp("2026-11-17T00:00:00Z");
    const auto catalog = ParseGameCatalog(verified.payload, now);
    Check(catalog.error == CatalogError::None);
    Check(catalog.revision == 7 && catalog.games.size() == 3);
    Check(catalog.games[0].name == "Sample Game" && catalog.games[1].name.empty());
    Check(ParseGameCatalog(verified.payload, *ParseUtcTimestamp("2026-11-18T00:00:00Z")).error ==
          CatalogError::Expired);
    Check(ParseGameCatalog(R"({"document_type":"other","revision":1})", now).error ==
          CatalogError::WrongDocumentType);
    Check(ParseGameCatalog(R"({"document_type":"game_catalog","revision":0})", now).error ==
          CatalogError::BadRevision);
    Check(ParseGameCatalog("[]", now).error == CatalogError::Malformed);

    Check(CheckTitle(catalog, 0x0100000000010000ULL, "1.2.0.0").support == TitleSupport::Supported);
    Check(CheckTitle(catalog, 0x0100000000010800ULL, "1.3.0").support == TitleSupport::Supported);
    const auto wrong = CheckTitle(catalog, 0x0100000000010000ULL, "1.1.0");
    Check(wrong.support == TitleSupport::WrongVersion && wrong.recommended_version == "1.3");
    Check(CheckTitle(catalog, 0x0100000000020000ULL, "1.0").support == TitleSupport::Disabled);
    Check(CheckTitle(catalog, 0x0100000000030000ULL, "1.0").support == TitleSupport::UnsupportedClient);
    Check(CheckTitle(catalog, 0x0100000000099000ULL, "1.0").support == TitleSupport::Unknown);

    GameCatalog older = catalog;
    older.revision = 6;
    Check(ShouldReplaceCatalog(older, catalog));
    Check(!ShouldReplaceCatalog(catalog, older));

    // Optional: the real published catalog, if the runner points at a downloaded copy. Only the
    // signature and the envelope are checked; its content stays out of the repository.
    if (const char* path = std::getenv("NEXTENDO_CATALOG_SAMPLE"); path && *path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        const auto real = VerifySignedDocument(text.str(), kCatalogKey);
        Check(real.error == SignedDocumentError::None);
        const auto parsed = ParseGameCatalog(real.payload, 0);
        Check(parsed.error == CatalogError::None && parsed.revision >= 1 && !parsed.games.empty());
        std::printf("real catalog: revision %lld, %zu games, signature OK\n",
                    static_cast<long long>(parsed.revision), parsed.games.size());
    }
}

void TestTrust() {
    Check(CountValidRoots() == 5);
    for (const auto& root : Roots::kRoots) {
        Check(CertificateSha256(root.pem) == root.sha256);
    }
    X509_STORE* store = BuildRootStore();
    Check(store != nullptr);
    X509_STORE_free(store);
    Check(CertificateSha256("not a certificate").empty());
}

void TestApi() {
    using App::CallOutcome;
    Check(App::OutcomeForStatus(200) == CallOutcome::Ok);
    Check(App::OutcomeForStatus(401) == CallOutcome::RefreshFirst);
    Check(App::OutcomeForStatus(403) == CallOutcome::MissingScope);
    Check(App::OutcomeForStatus(429) == CallOutcome::RateLimited);
    Check(App::OutcomeForStatus(400) == CallOutcome::Rejected);
    Check(App::OutcomeForStatus(503) == CallOutcome::RetryLater);
    Check(App::OutcomeForStatus(0) == CallOutcome::RetryLater);
    Check(App::ParseRetryAfter(" 30 ") == 30);
    Check(App::ParseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT", 45) == 45);
    Check(App::ParseRetryAfter("999999") == 3600);

    auto tokens = App::ParseTokenResponse(
        200, R"({"access_token":"at","refresh_token":"rt2","expires_in":3600,"scope":"game.matchmaking"})");
    Check(tokens.outcome == CallOutcome::Ok && tokens.refresh_token == "rt2" && tokens.expires_in == 3600);
    tokens = App::ParseTokenResponse(400, R"({"error":"invalid_grant"})");
    Check(tokens.outcome == CallOutcome::Rejected && tokens.error == "invalid_grant");
    Check(App::ParseTokenResponse(200, "{}").outcome == CallOutcome::Malformed);

    auto game = App::ParseGameToken(
        200, R"({"nex_token":"nx2abc","pid":1234567890,"friend_code":"1234-5678-9012","username":"u","expires_in":86400})",
        "");
    Check(game.outcome == CallOutcome::Ok && game.pid == 1234567890 && game.nex_token == "nx2abc");
    Check(game.friend_code == "1234-5678-9012" && game.expires_in == 86400);
    game = App::ParseGameToken(200, R"({"nex_token":"nx2abc","pid":"42"})", "");
    Check(game.outcome == CallOutcome::Ok && game.pid == 42);
    Check(App::ParseGameToken(200, R"({"pid":1})", "").outcome == CallOutcome::Malformed);
    game = App::ParseGameToken(429, "", "12");
    Check(game.outcome == CallOutcome::RateLimited && game.retry_after_seconds == 12);
    Check(App::ParseGameToken(403, "", "").outcome == CallOutcome::MissingScope);

    auto online = App::ParseOnlineStatus(200, R"({"allow":true,"reason":null})");
    Check(online.allow && online.reason == App::OnlineBlock::None);
    online = App::ParseOnlineStatus(200, R"({"allow":false,"reason":"discord_unlinked"})");
    Check(!online.allow && online.reason == App::OnlineBlock::DiscordUnlinked);
    online = App::ParseOnlineStatus(200, R"({"allow":false,"reason":"something_new"})");
    Check(online.reason == App::OnlineBlock::Unknown);

    const auto session = App::ParseSessionStatus(200, R"({"email_verified":true,"revoked":false})");
    Check(session.email_verified && !session.revoked);

    auto presence = nlohmann::json::parse(
        App::BuildPresenceBody(App::PresenceState::Playing, 0x0100000000010000ULL, "Lobby"));
    Check(presence["status"] == 2 && presence["app_id"] == "0100000000010000" &&
          presence["app_detail"] == "Lobby" && presence["platform"] == "emulator");
    presence = nlohmann::json::parse(App::BuildPresenceBody(App::PresenceState::Online, 0, ""));
    Check(presence["status"] == 1 && !presence.contains("app_id"));
    std::string long_detail(63, 'a');
    long_detail += "\xC3\xA9xyz"; // 'é' straddles byte 64
    presence = nlohmann::json::parse(
        App::BuildPresenceBody(App::PresenceState::Playing, 1, long_detail));
    Check(presence["app_detail"].get<std::string>() == std::string(63, 'a'));

    Check(App::ParseNsaId(R"({"nsa":"0123456789abcdef"})") == "0123456789abcdef");
    Check(!App::ParseNsaId(R"({"nsa":"xyz"})"));
}

void TestNetworkMode() {
    using App::NetworkMode;
    App::NetworkModeArbiter arbiter;
    Check(arbiter.Current() == NetworkMode::Offline);

    auto change = arbiter.Request(NetworkMode::Rooms, false);
    Check(change.allowed && !change.leave_room && !change.stop_nextendo);
    arbiter.Commit(change.next);

    change = arbiter.Request(NetworkMode::Nextendo, false);
    Check(change.allowed && change.leave_room && !change.stop_nextendo);
    arbiter.Commit(change.next);

    change = arbiter.Request(NetworkMode::Rooms, false);
    Check(change.allowed && change.stop_nextendo && !change.leave_room);

    change = arbiter.Request(NetworkMode::Rooms, true);
    Check(!change.allowed);
    change = arbiter.Request(NetworkMode::Offline, true);
    Check(change.allowed && change.stop_nextendo);
    change = arbiter.Request(NetworkMode::Nextendo, true);
    Check(change.allowed && !change.stop_nextendo);
}

} // namespace

int main() {
    TestHostRules();
    TestHostsFile();
    TestAddressBook();
    TestEncoding();
    TestPkce();
    TestIdToken();
    TestCatalog();
    TestTrust();
    TestApi();
    TestNetworkMode();
    std::printf("nextendo gate: %d checks passed\n", g_checks);
    return 0;
}
