// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/bcat_package.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "jadyx/network/nextendo/pkce.h"

namespace Jadyx::Network::Nextendo {

std::string BcatUrl(std::uint64_t title_id) {
    char id[17]{};
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
    return std::string(kApiBase) + "/api/bcat/" + id;
}

std::optional<std::uint64_t> BcatSiblingTitle(std::uint64_t title_id) {
    // Two editions of one game share an event, published once under the second edition's ID.
    if (title_id == 0x0100A3D008C5C000ULL) {
        return 0x01008F6008C5E000ULL;
    }
    return std::nullopt;
}

BcatAnswer ClassifyBcatAnswer(unsigned status, bool has_sibling) {
    if (status == 200) {
        return BcatAnswer::Package;
    }
    if (has_sibling && (status == 204 || status == 404)) {
        return BcatAnswer::TrySibling;
    }
    return BcatAnswer::KeepLocal;
}

bool ValidBcatName(std::string_view name) {
    if (name.empty() || name.size() > 31 || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
}

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

BcatPlan Refuse(BcatPackageError error) {
    BcatPlan plan;
    plan.error = error;
    return plan;
}

} // namespace

BcatPlan PlanBcatPackage(const std::vector<BcatEntry>& entries) {
    if (entries.size() > kBcatMaxEntries) {
        return Refuse(BcatPackageError::TooManyEntries);
    }
    if (entries.empty()) {
        return Refuse(BcatPackageError::Empty);
    }
    BcatPlan plan;
    std::set<std::string> seen_files;
    std::set<std::string> seen_directories;
    for (const auto& entry : entries) {
        const auto slash = entry.path.find('/');
        if (slash == std::string::npos || entry.path.find('/', slash + 1) != std::string::npos) {
            return Refuse(BcatPackageError::BadPath);
        }
        const std::string_view directory{entry.path.data(), slash};
        const std::string_view name{entry.path.data() + slash + 1, entry.path.size() - slash - 1};
        if (directory.empty() || name.empty() || directory == "." || directory == ".." ||
            name == "." || name == "..") {
            return Refuse(BcatPackageError::BadPath);
        }
        if (!ValidBcatName(directory) || !ValidBcatName(name)) {
            return Refuse(BcatPackageError::BadName);
        }
        if (!seen_files.insert(Lower(entry.path)).second) {
            return Refuse(BcatPackageError::Duplicate);
        }
        if (entry.size > kBcatMaxBytes || plan.total_bytes + entry.size > kBcatMaxBytes) {
            return Refuse(BcatPackageError::TooLarge);
        }
        plan.total_bytes += entry.size;
        if (seen_directories.insert(Lower(directory)).second) {
            plan.directories.emplace_back(directory);
        }
        plan.files.push_back({entry.path, std::string(directory), std::string(name), entry.size});
    }
    return plan;
}

const char* BcatPackageErrorName(BcatPackageError error) {
    switch (error) {
    case BcatPackageError::None:
        return "none";
    case BcatPackageError::Empty:
        return "empty";
    case BcatPackageError::BadPath:
        return "bad path";
    case BcatPackageError::BadName:
        return "bad name";
    case BcatPackageError::Duplicate:
        return "duplicate";
    case BcatPackageError::TooManyEntries:
        return "too many entries";
    case BcatPackageError::TooLarge:
        return "too large";
    }
    return "?";
}

bool BcatNeedsInstall(std::string_view stored_hash, std::string_view package_hash,
                      bool cache_present) {
    return !cache_present || stored_hash.empty() || Lower(stored_hash) != Lower(package_hash);
}

} // namespace Jadyx::Network::Nextendo
