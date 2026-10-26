// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/content/content_service.h"
#include "jadyx/content/content_session.h"
#include "jadyx/i18n/localization.h"
namespace Jadyx { class UiPainter; }
namespace Jadyx::Content {
I18n::Text ErrorText(Error error);
/// Why a launch was refused, naming the firmware asked for when the emulated system is too old.
std::wstring LaunchErrorText(const LaunchResolution& resolution);
/// A button's letters and their color, as the controller in use shows them.
struct Key {
    std::wstring_view letters;
    unsigned color;
};
/// The buttons the pane's prompts name: confirm, back, and the two face buttons for actions.
struct Keys {
    Key confirm, back, primary, secondary;
};
void DrawContentPanel(UiPainter& painter, const ContentSession& session, std::wstring_view game, const Keys& keys);
void DrawMissingContent(UiPainter& painter);
} // namespace Jadyx::Content
