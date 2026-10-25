#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace fan {

inline constexpr const char* kVersion = "0.1.0";
inline constexpr const char* kLabel = "local.fan.daemon";
inline constexpr const char* kSocketPath = "/var/run/fan.sock";
inline constexpr const char* kInstallPath = "/usr/local/libexec/fan";
inline constexpr const char* kSymlinkPath = "/usr/local/bin/fan";
inline constexpr const char* kPlistPath = "/Library/LaunchDaemons/local.fan.daemon.plist";
inline constexpr const char* kConfigPath = "/usr/local/etc/fan.conf";

// Override for tests/development via the FAN_SOCKET environment variable
inline std::string socketPath() {
    if (const char* override = std::getenv("FAN_SOCKET")) return override;
    return kSocketPath;
}

// daemon
int runDaemon(bool foreground);

}  // namespace fan
