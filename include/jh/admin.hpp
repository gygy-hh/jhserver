#pragma once

#include "jh/config.hpp"

namespace jh::gateway {
class Router;
}

namespace jh::admin {

void register_routes(gateway::Router& server, const ServerConfig& config);

}  // namespace jh::admin
