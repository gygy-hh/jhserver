#pragma once

#include "jh/config.hpp"

#include <httplib.h>

namespace jh::handlers {

void register_routes(httplib::Server& server, const ServerConfig& config);

}  // namespace jh::handlers
