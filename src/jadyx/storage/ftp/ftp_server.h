// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from NXbox src/eden_uwp/ftp (GPL-3.0-or-later).
#pragma once
#include <cstdint>
#include "jadyx/storage/ftp/ftp_paths.h"
#include <filesystem>
#include <memory>
#include <string>

namespace Jadyx::Ftp {
// Lifetime owns all sockets. Stop also cancels pending data I/O and removes partial uploads.
class Server {
  public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool Start(const Roots& roots, const std::string& ipv4);
    void Stop();
    bool Running() const;
    std::wstring Address() const;
    std::wstring Password() const;
    std::uint64_t Changes() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace Jadyx::Ftp
