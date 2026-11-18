// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/room_probe.h"

#include <chrono>
#include <cstdint>

#include "enet/enet.h"
#include "network/room.h"

namespace Network {

std::vector<std::optional<int>> ProbeRooms(const std::vector<RoomProbeTarget>& targets, u32 timeout_ms) {
    std::vector<std::optional<int>> results(targets.size());
    if (targets.empty()) return results;
    ENetHost* client = enet_host_create(nullptr, targets.size(), NumChannels, 0, 0);
    if (!client) return results;
    using Clock = std::chrono::steady_clock;
    std::vector<ENetPeer*> peers(targets.size(), nullptr);
    std::vector<Clock::time_point> started(targets.size());
    std::size_t waiting = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        ENetAddress address{};
        if (enet_address_set_host(&address, targets[i].host.c_str()) != 0) continue;
        address.port = targets[i].port;
        peers[i] = enet_host_connect(client, &address, NumChannels, 0);
        if (!peers[i]) continue;
        peers[i]->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i));
        started[i] = Clock::now();
        ++waiting;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds{timeout_ms};
    ENetEvent event{};
    while (waiting > 0 && Clock::now() < deadline) {
        if (enet_host_service(client, &event, 5) <= 0) continue;
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
            continue;
        }
        const auto index = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(event.peer->data));
        if (index >= peers.size() || peers[index] != event.peer) continue;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            results[index] = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started[index]).count());
            enet_peer_disconnect_now(event.peer, 0);
        }
        // Connected (and let go) or refused: either way this room is done.
        peers[index] = nullptr;
        --waiting;
    }
    // Rooms that never answered: drop them without sending anything more.
    for (auto* peer : peers)
        if (peer) enet_peer_reset(peer);
    enet_host_flush(client);
    enet_host_destroy(client);
    return results;
}

} // namespace Network
