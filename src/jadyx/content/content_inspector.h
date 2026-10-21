// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
#include "jadyx/content/content_model.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"
namespace FileSys { class ManualContentProvider; }

namespace Jadyx::Content {
using Progress = std::function<void(std::uint64_t, std::uint64_t)>;
struct ContentFile {
    FileSys::ContentRecord record{};
    FileSys::VirtualFile file;
    std::uint64_t program{};
};
struct InspectedContent {
    Revision revision;
    Error error{};
    FileSys::VirtualFile meta;
    std::shared_ptr<FileSys::CNMT> cnmt;
    std::vector<ContentFile> files;
    std::vector<FileSys::VirtualFile> tickets;
};
struct Inspection {
    Error error{};
    std::vector<InspectedContent> contents;
};
// format overrides the extension (nsp/xci/nca) for files without one, such as uploads.
Inspection InspectPackage(FileSys::VfsFilesystem& vfs, const std::string& source,
                          bool verify, std::stop_token stop = {}, Progress progress = {},
                          std::string_view format = {});
Error VerifyContent(InspectedContent& content, std::stop_token stop = {}, Progress progress = {});
void RegisterContent(FileSys::ManualContentProvider& provider, const InspectedContent& content);
} // namespace Jadyx::Content
