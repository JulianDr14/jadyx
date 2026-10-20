// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>
#include <vector>
#include "jadyx/i18n/localization.h"
#include "jadyx/updates/updater.h"
namespace Jadyx::Updates {
inline I18n::Text PhaseText(Phase phase) {
    using T = I18n::Text;
    switch (phase) {
    case Phase::Idle:
        return T::UpdaterIdle;
    case Phase::Checking:
        return T::UpdaterChecking;
    case Phase::Available:
        return T::UpdaterAvailable;
    case Phase::Downloading:
        return T::UpdaterDownloading;
    case Phase::Paused:
        return T::UpdaterPaused;
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
enum class Action {
    Check,
    Channel,
    Download,
    Pause,
    Cancel,
    Install,
    Later,
    Skip,
    Notes,
    History,
    Back
};
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
inline ActionList Actions(const Snapshot& state) {
    if (state.phase == Phase::Checking || state.phase == Phase::Verifying ||
        state.phase == Phase::Preparing)
        return {Action::Back};
    if (state.phase == Phase::Downloading)
        return {Action::Pause, Action::Cancel, Action::Back};
    ActionList out;
    if (state.phase != Phase::Requested) {
        out = {Action::Check, Action::Channel};
        if (state.release) {
            out.push_back(state.phase == Phase::Ready || state.phase == Phase::Assistance
                              ? Action::Install
                              : Action::Download);
            out.push_back(Action::Cancel);
            out.push_back(Action::Later);
            out.push_back(Action::Skip);
        }
    }
    if (state.release)
        out.push_back(Action::Notes);
    if (!state.history.empty())
        out.push_back(Action::History);
    out.push_back(Action::Back);
    return out;
}
inline I18n::Text ActionText(Action action, const Snapshot& state) {
    using T = I18n::Text;
    switch (action) {
    case Action::Notes:
        return T::UpdaterHint;
    case Action::History:
        return T::UpdaterHistory;
    case Action::Channel:
        return state.channel == Channel::Stable ? T::UpdaterStable : T::UpdaterPreview;
    case Action::Download:
        return state.phase == Phase::Paused || state.phase == Phase::Failed ? T::UpdaterResume
                                                                            : T::UpdaterDownload;
    case Action::Install:
        return state.phase == Phase::Assistance ? T::UpdaterAssistance : T::UpdaterInstall;
    case Action::Check:
        return T::UpdaterCheck;
    case Action::Pause:
        return T::UpdaterPause;
    case Action::Cancel:
        return T::UpdaterCancel;
    case Action::Later:
        return T::UpdaterLater;
    case Action::Skip:
        return T::UpdaterSkip;
    case Action::Back:
        return T::UpdaterBack;
    }
    return T::UpdaterBack;
}
inline std::vector<std::wstring> DetailLines(const Snapshot& state, bool history) {
    std::wstring text;
    if (!history && state.release) {
        // UTF-8 decoding is provided by the caller for notes (the UI is Windows/WinRT).
        return {};
    }
    for (const auto& event : state.history) {
        I18n::Text label = I18n::Text::UpdaterCheck;
        if (event.find("confirmed") != std::string::npos)
            label = event.find("unconfirmed") != std::string::npos ? I18n::Text::UpdaterDeployment
                                                                   : I18n::Text::UpdaterConfirmed;
        else if (event.find("verified") != std::string::npos)
            label = I18n::Text::UpdaterReady;
        else if (event.find("cancelled") != std::string::npos)
            label = I18n::Text::UpdaterCancel;
        else if (event.find("installation") != std::string::npos)
            label = I18n::Text::UpdaterAssistance;
        else if (event.find("deployment accepted") != std::string::npos)
            label = I18n::Text::UpdaterRequested;
        else if (event.find("deployment unavailable") != std::string::npos)
            label = I18n::Text::UpdaterDeployment;
        text += std::wstring{I18n::Tr(label)} + L"\n";
    }
    std::vector<std::wstring> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(L'\n', begin);
        lines.push_back(text.substr(begin, end - begin));
        if (end == text.npos)
            break;
        begin = end + 1;
    }
    return lines;
}
} // namespace Jadyx::Updates
