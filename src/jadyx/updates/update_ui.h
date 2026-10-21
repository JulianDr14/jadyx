// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <string>
#include "jadyx/i18n/localization.h"
#include "jadyx/updates/updater.h"
namespace Jadyx::Updates {
// A remembered release reopens as Paused even when nothing was downloaded yet.
inline bool DownloadStarted(const Snapshot& state) {
    return state.received > 0 || state.phase == Phase::Downloading ||
           state.phase == Phase::Verifying || state.phase == Phase::Ready ||
           state.phase == Phase::Assistance;
}
// The page's headline: what is happening, in the user's words.
inline I18n::Text PhaseText(const Snapshot& state) {
    using T = I18n::Text;
    switch (state.phase) {
    case Phase::Idle:
        return state.release ? T::UpdaterAvailable : T::UpdaterIdle;
    case Phase::Checking:
        return T::UpdaterChecking;
    case Phase::Available:
        return T::UpdaterAvailable;
    case Phase::Downloading:
        return T::UpdaterDownloading;
    case Phase::Paused:
        return state.received ? T::UpdaterPaused : T::UpdaterAvailable;
    case Phase::Verifying:
        return T::UpdaterVerifying;
    case Phase::Ready:
        return T::UpdaterReady;
    case Phase::Preparing:
        return T::UpdaterPreparing;
    case Phase::Requested:
        return T::UpdaterRequested;
    case Phase::Confirmed:
        return T::UpdaterConfirmed;
    case Phase::Assistance:
        return T::UpdaterAssistance;
    case Phase::Failed:
        return T::UpdaterFailed;
    }
    return T::UpdaterIdle;
}
inline I18n::Text ErrorText(Error error) {
    using T = I18n::Text;
    switch (error) {
    case Error::Network:
        return T::UpdaterNetwork;
    case Error::Manifest:
        return T::UpdaterManifest;
    case Error::Disk:
        return T::UpdaterDisk;
    case Error::Integrity:
        return T::UpdaterIntegrity;
    case Error::Deployment:
        return T::UpdaterDeployment;
    case Error::Persistence:
        return T::UpdaterPersistence;
    case Error::Busy:
        return T::UpdaterBusy;
    default:
        return T::UpdaterFailed;
    }
}
enum class Action { Check, Channel, Download, Pause, Cancel, Install, Later, Skip, Notes };
// At most nine actions; building a frame's controls requires no heap allocation.
struct ActionList {
    std::array<Action, 9> items{};
    std::size_t count{};
    ActionList() = default;
    ActionList(std::initializer_list<Action> values) {
        for (auto value : values)
            push_back(value);
    }
    void push_back(Action action) {
        if (count < items.size())
            items[count++] = action;
    }
    std::size_t size() const {
        return count;
    }
    bool empty() const {
        return count == 0;
    }
    Action operator[](std::size_t index) const {
        return items[index];
    }
    auto begin() const {
        return items.begin();
    }
    auto end() const {
        return items.begin() + count;
    }
};
// Only what can be done now: no cancel without a download, no install without a package.
// Leaving the page is B (or X), never a row.
inline ActionList Actions(const Snapshot& state) {
    switch (state.phase) {
    case Phase::Checking:
    case Phase::Verifying:
    case Phase::Preparing:
    case Phase::Requested:
        return {};
    case Phase::Downloading:
        return {Action::Pause, Action::Cancel};
    case Phase::Ready:
    case Phase::Assistance:
        if (!state.release)
            break;
        if (state.release->notes.empty())
            return {Action::Install, Action::Cancel};
        return {Action::Install, Action::Cancel, Action::Notes};
    default:
        break;
    }
    if (!state.release)
        return {Action::Check, Action::Channel};
    ActionList out{Action::Download};
    if (state.received)
        out.push_back(Action::Cancel);
    if (!state.release->notes.empty())
        out.push_back(Action::Notes);
    if (!state.received) {
        out.push_back(Action::Later);
        out.push_back(Action::Skip);
        out.push_back(Action::Channel);
    }
    return out;
}
inline I18n::Text ActionText(Action action, const Snapshot& state) {
    using T = I18n::Text;
    switch (action) {
    case Action::Check:
        return T::UpdaterCheck;
    case Action::Channel:
        return T::UpdaterChannel;
    case Action::Download:
        return state.phase == Phase::Failed ? T::UpdaterRetry
               : state.received             ? T::UpdaterResume
                                            : T::UpdaterDownload;
    case Action::Pause:
        return T::UpdaterPause;
    case Action::Cancel:
        return state.phase == Phase::Downloading ? T::UpdaterCancel : T::UpdaterDiscard;
    case Action::Install:
        return state.phase == Phase::Assistance ? T::UpdaterInstallGuided : T::UpdaterInstall;
    case Action::Later:
        return T::UpdaterLater;
    case Action::Skip:
        return T::UpdaterSkip;
    case Action::Notes:
        return T::UpdaterNotes;
    }
    return T::UpdaterCheck;
}
// What a row says under its label once focused.
inline I18n::Text ActionHint(Action action, const Snapshot& state) {
    using T = I18n::Text;
    switch (action) {
    case Action::Check:
        return T::UpdaterCheckHint;
    case Action::Channel:
        return T::UpdaterChannelHint;
    case Action::Download:
        return T::UpdaterDownloadHint;
    case Action::Pause:
        return T::UpdaterPauseHint;
    case Action::Cancel:
        return T::UpdaterDiscardHint;
    case Action::Install:
        return state.phase == Phase::Assistance ? T::UpdaterHelp : T::UpdaterInstallHint;
    case Action::Later:
        return T::UpdaterLaterHint;
    case Action::Skip:
        return T::UpdaterSkipHint;
    case Action::Notes:
        return T::UpdaterNotesHint;
    }
    return T::UpdaterCheckHint;
}
// The label with its values: the version to download, the percent already saved, the channel.
inline std::wstring ActionLabel(Action action, const Snapshot& state) {
    const auto text = ActionText(action, state);
    if (action == Action::Channel)
        return I18n::Format(text, {I18n::Tr(state.channel == Channel::Stable
                                                 ? I18n::Text::UpdaterStable
                                                 : I18n::Text::UpdaterPreview)});
    if (action == Action::Download && state.release) {
        const auto version = FormatVersion(state.release->version);
        const auto percent =
            state.total ? std::min<std::uint64_t>(99, state.received * 100 / state.total) : 0;
        return I18n::Format(text, {std::wstring{version.begin(), version.end()},
                                   std::to_wstring(percent)});
    }
    return std::wstring{I18n::Tr(text)};
}
} // namespace Jadyx::Updates
