// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/online/nextendo/config.h"

namespace Core::Online::Nextendo {

ConfigStore& GlobalConfig() {
    static ConfigStore store;
    return store;
}

AddressBook& GlobalAddressBook() {
    static AddressBook book;
    return book;
}

} // namespace Core::Online::Nextendo
