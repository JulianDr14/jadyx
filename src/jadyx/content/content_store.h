// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include "jadyx/content/content_model.h"

namespace Jadyx::Content {
struct Catalog {
    std::vector<Revision> revisions;
    std::vector<std::uint64_t> managed_applications;
    std::uint64_t generation{};
};
/// Identity of one game's registered updates/DLC (ids, which are enabled, whether it is managed).
/// Changes only when that game's content does, unlike the catalog-wide generation.
std::uint64_t ApplicationStamp(const Catalog& catalog, std::uint64_t application);
// Only the serial import worker writes this store. Readers get an immutable copy.
class ContentStore {
public:
    enum class PublishStep { WriteNext, ValidateNext, WriteBackup, ReplaceBackup, ReplaceCatalog };
    using Failure = std::function<bool(PublishStep)>;
    explicit ContentStore(std::filesystem::path root_, Failure failure_ = {})
        : root(std::move(root_)), failure(std::move(failure_)) {}
    Error Load(Catalog& catalog) const;
    Error Save(Catalog& catalog) const;
    Error Remove(Catalog& catalog, const std::string& id) const;
    /// Removes every matching revision and deletes their internal copies; Error::Invalid when
    /// none matched.
    Error Remove(Catalog& catalog, const std::function<bool(const Revision&)>& match) const;
    void Cleanup(const Catalog& catalog) const;
    std::filesystem::path Object(const std::string& id) const;
    std::filesystem::path Temporary(const std::string& id) const;
    const std::filesystem::path& Root() const { return root; }
private:
    std::filesystem::path root;
    Failure failure;
};
} // namespace Jadyx::Content
