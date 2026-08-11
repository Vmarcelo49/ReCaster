// src/exe/session/relay_setup.hpp
//
// Resolves a relay server list, validates it, creates a RelayClient,
// and wires it to an EnetTransport. Used by all relay-related Start*
// branches in NetplaySession::apply_command.

#pragma once

#include "../../common/net/enet_transport.hpp"
#include "../../common/net/relay/relay_client.hpp"
#include "../../common/net/relay/relay_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace caster::exe::session {

class RelaySetup {
public:
    // Resolve the relay server list from a user-provided source string.
    // Empty source → default list. Returns false if the result is empty.
    bool resolve(const std::string& relay_source);

    // Create a RelayClient and wire it to the transport.
    // - role: Host or Client
    // - local_port: host port (or 0 for OS-assigned)
    // - peer_identifier: 4-letter room code (client only)
    //
    // Calls transport.set_relay_sink() and transport.install_intercept().
    // Returns false on error (sets error_msg).
    bool create_client(common::net::EnetTransport& transport,
                       common::net::relay_client::ClientRole role,
                       std::uint16_t local_port,
                       const std::string& peer_identifier = {},
                       std::string* error_msg = nullptr);

    // Access the resolved list (for inspection / logging).
    const common::net::relay_config::RelayList& relay_list() const;

    // Access the created client (ownership transferred via create_client).
    std::unique_ptr<common::net::relay_client::RelayClient> take_client();

private:
    common::net::relay_config::RelayList relay_list_;
    std::unique_ptr<common::net::relay_client::RelayClient> client_;
};

} // namespace caster::exe::session
