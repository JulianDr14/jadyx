// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/uwp_library.h"
#include "jadyx/library/library_session.h"

namespace Jadyx {

std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed,
                                         std::string_view auto_pick, std::string_view initial_notice,
                                         std::uint64_t* selected_program_id) {
    LibrarySession session{core_window, width, height, root, seed, auto_pick, initial_notice, selected_program_id};
    return session.Run();
}
} // namespace Jadyx
