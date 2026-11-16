// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include "jadyx/content/content_service.h"

namespace Jadyx::Content {
/// Registers the updates/DLC embedded in a base package the first time an application is
/// managed, then records it as managed. Shared by the content manager and the portal so
/// either one may be the first to touch a game.
Error AdoptEmbeddedContent(ContentStore& store, Catalog& catalog, FileSys::VfsFilesystem& vfs,
                           std::uint64_t application, const std::string& base_path,
                           std::uint32_t base_version, std::stop_token stop = {}, Progress progress = {});

/// Portal-facing actions return an error word ("" on success), as the browser shows them.

/// Installs one received NSP/XCI for `application`: in LocalState, or on D: with `developer`.
/// Only content of `kind` is taken; the package must belong to that game. Returns a portal error
/// word, empty on success.
std::string ImportUploadedContent(const std::filesystem::path& package, Kind kind, std::uint64_t application,
                                  const std::string& base_path, bool developer = false,
                                  std::stop_token stop = {}, Progress progress = {});

/// Moves the console's copy of one update/DLC of `application` to D: (`developer`) or back to
/// LocalState. The new copy is hashed before the catalog points at it; the old one is deleted
/// after. "" when it is already there.
std::string MoveContent(std::uint64_t application, const std::string& id, bool developer,
                        std::stop_token stop = {}, Progress progress = {});

/// Deletes one update/DLC registered for `application`, internal copy included.
std::string RemoveContent(std::uint64_t application, const std::string& id);
/// Deletes every update/DLC of `application` and forgets that it was managed.
std::string RemoveApplicationContent(std::uint64_t application);
} // namespace Jadyx::Content
