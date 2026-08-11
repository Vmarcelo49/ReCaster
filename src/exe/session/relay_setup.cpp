// src/exe/session/relay_setup.cpp

#include "relay_setup.hpp"

#include "../../common/logger.hpp"

namespace caster::exe::session {

namespace rc = common::net::relay_config;
namespace rclient = common::net::relay_client;

bool RelaySetup::resolve(const std::string& relay_source) {
    if (relay_source.empty()) {
        relay_list_ = rc::default_list();
    } else {
        relay_list_ = rc::parse_list(relay_source);
    }
    return !relay_list_.empty();
}

bool RelaySetup::create_client(common::net::EnetTransport& transport,
                               rclient::ClientRole role,
                               std::uint16_t local_port,
                               const std::string& peer_identifier,
                               std::string* error_msg) {
    if (relay_list_.empty()) {
        if (error_msg) *error_msg = "No relay servers configured";
        return false;
    }

    rclient::RelayClientInit init;
    init.relay = relay_list_[0];
    init.role = role;
    init.local_port = local_port;
    init.peer_identifier = peer_identifier;
    init.external_udp_socket = transport.udp_socket_fd();

    client_ = std::make_unique<rclient::RelayClient>(init);
    transport.set_relay_sink(client_.get());
    transport.install_intercept();
    return true;
}

const rc::RelayList& RelaySetup::relay_list() const {
    return relay_list_;
}

std::unique_ptr<rclient::RelayClient> RelaySetup::take_client() {
    return std::move(client_);
}

} // namespace caster::exe::session
