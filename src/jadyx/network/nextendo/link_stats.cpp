// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/link_stats.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Jadyx::Network::Nextendo {

void LinkStats::Add(std::optional<int> round_trip_ms) {
    if (samples_.size() == Window) samples_.erase(samples_.begin());
    samples_.push_back(round_trip_ms);
}

void LinkStats::Clear() {
    samples_.clear();
}

LinkSnapshot LinkStats::Snapshot(LinkVia via) const {
    LinkSnapshot result;
    result.via = via;
    result.samples = samples_.size();
    long long sum = 0;
    for (const auto& sample : samples_) {
        if (!sample) continue;
        ++result.answered;
        sum += *sample;
    }
    if (result.samples) {
        result.loss_percent =
            100.0 * static_cast<double>(result.samples - result.answered) / static_cast<double>(result.samples);
    }
    if (!result.answered) return result;
    const double mean = static_cast<double>(sum) / static_cast<double>(result.answered);
    double spread = 0;
    for (const auto& sample : samples_) {
        if (sample) spread += std::abs(static_cast<double>(*sample) - mean);
    }
    result.round_trip_ms = static_cast<int>(std::lround(mean));
    result.variance_ms = static_cast<int>(std::lround(spread / static_cast<double>(result.answered)));
    return result;
}

std::vector<LinkHudLine> LinkHudLines(const LinkSnapshot& snapshot) {
    std::vector<LinkHudLine> lines;
    lines.push_back({"ONLINE NEXTENDO"});
    const char* via = snapshot.via == LinkVia::Api ? "PING API" : "PING";
    char text[64]{};
    if (snapshot.samples == 0) {
        std::snprintf(text, sizeof(text), "%s MIDIENDO", via);
        lines.push_back({text});
        return lines;
    }
    if (snapshot.answered == 0) {
        std::snprintf(text, sizeof(text), "%s SIN RESPUESTA", via);
        lines.push_back({text, true});
        return lines;
    }
    std::snprintf(text, sizeof(text), "%s %d MS VAR %d MS", via, snapshot.round_trip_ms, snapshot.variance_ms);
    lines.push_back({text, snapshot.round_trip_ms > 150 || snapshot.variance_ms > 50});
    std::snprintf(text, sizeof(text), "PERDIDA %.1f%%", snapshot.loss_percent);
    lines.push_back({text, snapshot.loss_percent >= 2.0});
    return lines;
}

LinkPill LinkPillText(const LinkSnapshot& snapshot) {
    LinkPill pill{L"Nextendo", LinkIsSlow(snapshot) && snapshot.samples != 0};
    if (snapshot.answered) {
        pill.text += snapshot.via == LinkVia::Api ? L" · API " : L" · ";
        pill.text += std::to_wstring(snapshot.round_trip_ms) + L" ms";
    }
    return pill;
}

} // namespace Jadyx::Network::Nextendo
