#pragma once
constexpr unsigned short kDefaultGatewayPort = 7004;
constexpr unsigned short kFallbackGatewayPort{7001};
constexpr unsigned short kAltGatewayPort(7002);
constexpr unsigned short kBasePort = 7009;
constexpr unsigned short kDerivedGatewayPort = kBasePort;
constexpr unsigned short kEnvGatewayPort = portFromEnvironment();
#define HELIOS_GATEWAY_PORT 7011
