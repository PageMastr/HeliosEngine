#pragma once
// Log channels shared by the server module's sources.
#include "helios/core/log.h"

namespace helios::server {
HELIOS_LOG_CHANNEL(LogServer, "Server");
HELIOS_LOG_CHANNEL(LogCell, "Cell");
HELIOS_LOG_CHANNEL(LogGateway, "Gateway");
HELIOS_LOG_CHANNEL(LogOrch, "Orchestrator");
} // namespace helios::server
