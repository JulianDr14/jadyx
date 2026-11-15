// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

// Bookkeeping of an image's copies in other typeless families (ViewCast::Source::Copy). The
// image and its copies hold the same texels; at most one of them is newer than the others:
// the image (by write version), or the one copy rendered to since ("ahead"). Pure: indices and
// versions only; Image records the copies through buffers this asks for.
namespace D3D12 {

class ReinterpretSync {
public:
    /// Families a single image is viewed in at once stay few (G-buffers: two or three).
    static constexpr std::size_t MAX_COPIES = 4;

    [[nodiscard]] std::size_t Count() const noexcept { return count; }
    [[nodiscard]] bool Full() const noexcept { return count == MAX_COPIES; }

    /// A new copy, holding nothing of the image yet. Call only when not Full().
    std::size_t Add() noexcept {
        versions[count] = NEVER;
        return count++;
    }

    /// The copy rendered to whose texels the image does not have yet.
    [[nodiscard]] std::optional<std::size_t> Ahead() const noexcept {
        return ahead == NONE ? std::nullopt : std::optional<std::size_t>{ahead};
    }

    /// The copy to write back into the image before copy `index` is read or rendered: another
    /// copy's draws reach `index` only through the image.
    [[nodiscard]] std::optional<std::size_t> WriteBackBefore(std::size_t index) const noexcept {
        return ahead == NONE || ahead == index ? std::nullopt
                                               : std::optional<std::size_t>{ahead};
    }

    /// Whether copy `index` must be copied again from the image at image_version.
    [[nodiscard]] bool Stale(std::size_t index, std::uint64_t image_version) const noexcept {
        return index != ahead && versions[index] != image_version;
    }

    /// Copy `index` was copied from the image at image_version.
    void Refreshed(std::size_t index, std::uint64_t image_version) noexcept {
        versions[index] = image_version;
    }

    /// Copy `index` is a render target: it holds the newest texels until written back.
    void Rendered(std::size_t index) noexcept { ahead = static_cast<std::uint8_t>(index); }

    /// Starts writing the copy ahead back into the image: nothing is ahead any more. The caller
    /// copies it, then marks it Refreshed at the image's new version, which leaves every other
    /// copy stale.
    [[nodiscard]] std::optional<std::size_t> TakeAhead() noexcept {
        const auto taken = Ahead();
        ahead = NONE;
        return taken;
    }

private:
    static constexpr std::uint64_t NEVER = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint8_t NONE = 0xff;

    std::array<std::uint64_t, MAX_COPIES> versions{};
    std::uint8_t count{};
    std::uint8_t ahead{NONE};
};

} // namespace D3D12
