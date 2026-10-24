// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include "core/frontend/applets/error.h"
#include "jadyx/game/error_display.h"

namespace Jadyx {
class UwpErrorApplet final : public Core::Frontend::ErrorApplet {
public:
    explicit UwpErrorApplet(std::shared_ptr<ErrorDisplay> display);
    void Close() const override;
    void ShowError(Result error, FinishedCallback finished) const override;
    void ShowErrorWithTimestamp(Result error, std::chrono::seconds time,
                                FinishedCallback finished) const override;
    void ShowCustomErrorText(Result error, std::string main_text, std::string detail_text,
                             FinishedCallback finished) const override;

private:
    void Show(Result error, std::string message, FinishedCallback finished) const;
    std::shared_ptr<ErrorDisplay> display;
};
} // namespace Jadyx
