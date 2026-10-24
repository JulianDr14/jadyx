// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include "jadyx/storage/files_browser.h"
int main() {
    using namespace Jadyx;
    FilesBrowser browser;
    FilesPage roots;
    FilesEntry internal;
    internal.name = L"Internal";
    internal.folder = true;
    roots.entries.push_back(internal);
    roots.sources_unavailable = true;
    browser.Apply(std::move(roots));
    assert(browser.page.sources_unavailable && browser.page.entries.size() == 1);
    browser.busy = true;
    assert(!browser.Enter(0) && !browser.NextPage() && !browser.PreviousPage());
    browser.busy = false;
    assert(!browser.Enter(1));
    assert(browser.Enter(0));
    assert(!browser.roots && browser.location.relative.empty());
    FilesPage page;
    FilesEntry folder;
    folder.name = L"Folder";
    folder.component = "Folder";
    folder.folder = true;
    page.entries.push_back(folder);
    FilesEntry file;
    file.name = L"Empty.txt";
    file.component = "Empty.txt";
    page.entries.push_back(file);
    browser.Apply(page);
    assert(!browser.Enter(1)); // Files are metadata only; no launch or preview.
    assert(browser.Enter(0) && browser.location.relative == "Folder");
    assert(browser.Back() && browser.location.relative.empty() && !browser.roots);
    assert(browser.Back() && browser.roots);
    assert(!browser.Back());
    assert(!FilesChild({}, "../escape"));
    assert(!FilesChild({}, "a/b"));
    assert(!FilesChild({}, "a\\b"));
    assert(!FilesChild({}, "x:stream"));
    assert(!FilesChild({}, std::string("a\0b", 3)));
    assert(!FilesChild({}, "."));
    assert(!FilesChild({}, ".."));
    assert(!FilesChild({}, ""));
    browser.roots = false;
    browser.Apply(page);
    assert(!browser.NextPage());
    page.more = true;
    browser.Apply(page);
    assert(browser.NextPage() && browser.offset == FilesPageSize);
    assert(browser.PreviousPage() && browser.offset == 0);
    assert(!browser.PreviousPage());
    page.error = FilesError::Unavailable;
    browser.Apply(page);
    assert(browser.page.entries.empty() && !browser.page.more);
    assert(!browser.Enter(0) && !browser.NextPage());
    browser.location.relative = "parent/child";
    assert(browser.Back() && browser.location.relative == "parent");
    browser.offset = std::numeric_limits<unsigned>::max() - FilesPageSize + 1;
    browser.page.more = true;
    assert(!browser.NextPage()); // Never wrap the broker offset.
    FilesLocation invalid;
    invalid.relative = "../outside";
    assert(!FilesChild(invalid, "child"));
    assert(FilesChild({}, "日本語.txt")->relative == "日本語.txt");
    page.error = FilesError::Permission;
    browser.Apply(page);
    assert(browser.page.error == FilesError::Permission && browser.page.entries.empty());
    browser.Apply({}); // Empty directory is not an error.
    assert(browser.page.error == FilesError::None && browser.page.entries.empty());
    std::puts("files browser: navigation, paging, traversal, errors, roots and metadata PASS");
}
