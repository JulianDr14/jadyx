// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The account service's side of Nextendo: the signed BAAS id_token a game reads before it logs in,
// and the account id it shows the servers. Every function answers std::nullopt while Nextendo is
// off, so acc keeps its original stubs for offline play and Eden rooms.

#include <optional>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service {
class HLERequestContext;
}

namespace Service::Account::NextendoAcc {

/// The Nextendo principal id, used as the network service account id.
std::optional<u64> AccountId();

/// True while Nextendo is on and signed in: the license and availability checks pass.
bool Licensed();

/// An RS256 id_token for the running title (reused for up to two hours), without a terminator.
std::optional<std::vector<u8>> IdToken(Core::System& system);

/// Writes the token as a C string and zeroes the rest of the guest buffer. Returns its length.
u32 WriteIdToken(HLERequestContext& ctx, const std::vector<u8>& token);

} // namespace Service::Account::NextendoAcc
