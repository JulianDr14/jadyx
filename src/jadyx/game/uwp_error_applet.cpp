// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/game/uwp_error_applet.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace Jadyx {
UwpErrorApplet::UwpErrorApplet(std::shared_ptr<ErrorDisplay> error_display)
    : display{std::move(error_display)} {}

void UwpErrorApplet::Close() const {
    display->Close();
}

void UwpErrorApplet::Show(Result error, std::string message, FinishedCallback finished) const {
    std::ostringstream code;
    code << std::setfill('0') << std::setw(4) << (2000 + static_cast<unsigned>(error.GetModule()))
         << '-' << std::setw(4) << error.GetDescription();
    display->Show(code.str(), std::move(message), std::move(finished));
}

void UwpErrorApplet::ShowError(Result error, FinishedCallback finished) const {
    Show(error, {}, std::move(finished));
}

void UwpErrorApplet::ShowErrorWithTimestamp(Result error, std::chrono::seconds,
                                            FinishedCallback finished) const {
    // The core reporter preserves the timestamp; the dialog shows the guest error code.
    Show(error, {}, std::move(finished));
}

void UwpErrorApplet::ShowCustomErrorText(Result error, std::string main_text,
                                         std::string detail_text, FinishedCallback finished) const {
    if (!main_text.empty() && !detail_text.empty())
        main_text += "\n\n";
    main_text += detail_text;
    Show(error, std::move(main_text), std::move(finished));
}
} // namespace Jadyx
