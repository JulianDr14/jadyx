// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The interface typeface. Inter ships in the package (Assets/Fonts, SIL OFL 1.1); outside a
// package, or when a file is missing, the system's Segoe UI stands in.

#include <dwrite.h>

namespace Jadyx {

struct InterfaceTypeface {
    IDWriteFontCollection* collection; ///< nullptr: the system collection.
    const wchar_t* family;
};

/// Built once from the first factory that asks; DirectWrite's shared factory is a singleton, so
/// every surface (library, in-game menu, Pulse) gets the same collection.
InterfaceTypeface InterfaceFonts(IDWriteFactory* write);
InterfaceTypeface SystemInterfaceFonts(IDWriteFactory* write, bool monospace);

} // namespace Jadyx
