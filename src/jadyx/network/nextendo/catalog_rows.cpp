// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/catalog_rows.h"

#include <algorithm>
#include <cctype>

namespace Jadyx::Network::Nextendo {

namespace Nx = Core::Online::Nextendo;

namespace {

RowVerdict VerdictOf(Nx::TitleSupport support) {
    switch (support) {
    case Nx::TitleSupport::Supported:
        return RowVerdict::Ready;
    case Nx::TitleSupport::WrongVersion:
        return RowVerdict::WrongVersion;
    case Nx::TitleSupport::UnsupportedClient:
        return RowVerdict::NeedsUpdate;
    case Nx::TitleSupport::Disabled:
    case Nx::TitleSupport::Unknown:
        break;
    }
    return RowVerdict::Disabled;
}

bool NameLess(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

std::string TitleIdText(std::uint64_t title_id) {
    static constexpr char Digits[] = "0123456789ABCDEF";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = Digits[title_id & 0xF];
        title_id >>= 4;
    }
    return text;
}

std::vector<CatalogRow> BuildCatalogRows(const Nx::GameCatalog& catalog,
                                         std::span<const InstalledTitle> installed) {
    std::vector<CatalogRow> rows;
    rows.reserve(catalog.games.size());
    for (const auto& game : catalog.games) {
        if (game.title_ids.empty()) {
            continue;
        }
        CatalogRow row;
        row.title_id = game.title_ids.front();
        row.recommended_version = game.recommended_version;
        row.name = game.name;
        const auto owned = std::find_if(installed.begin(), installed.end(), [&](const InstalledTitle& title) {
            return std::find(game.title_ids.begin(), game.title_ids.end(), title.title_id) !=
                   game.title_ids.end();
        });
        if (owned != installed.end()) {
            row.installed = true;
            row.title_id = owned->title_id;
            row.installed_version = owned->version;
            row.verdict = VerdictOf(Nx::CheckTitle(catalog, owned->title_id, owned->version).support);
            if (row.name.empty()) {
                row.name = owned->name;
            }
        } else if (!game.enabled) {
            row.verdict = RowVerdict::Disabled;
        }
        if (row.name.empty()) {
            row.name = TitleIdText(row.title_id);
        }
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const CatalogRow& a, const CatalogRow& b) {
        if (a.installed != b.installed) {
            return a.installed;
        }
        if (a.installed && a.verdict != b.verdict) {
            return a.verdict < b.verdict;
        }
        return NameLess(a.name, b.name);
    });
    return rows;
}

} // namespace Jadyx::Network::Nextendo
