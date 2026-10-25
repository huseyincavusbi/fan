#include <string>
#include <vector>

#include <grp.h>
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "app.hpp"
#include "curve.hpp"
#include "fans.hpp"
#include "smc.hpp"

namespace fan {

namespace {

std::string executablePath() {
    char buffer[4096] = {};
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) return "";
    char resolved[4096] = {};
    if (realpath(buffer, resolved)) return resolved;
    return buffer;
}

gid_t adminGid() {
    if (struct group* group = getgrnam("admin")) return group->gr_gid;
    return 80;
}

int connectSocket() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string path = socketPath();
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int clientRequest(const std::string& request) {
    const int fd = connectSocket();
    if (fd < 0) {
        std::fprintf(stderr,
                     "fan: daemon is not running. Install with 'sudo fan install'; "
                     "'fan status' also reads directly.\n");
        return 1;
    }

    std::string line = request + "\n";
    (void)!write(fd, line.c_str(), line.size());
    ::shutdown(fd, SHUT_WR);

    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
        std::fwrite(buffer, 1, size_t(n), stdout);
    ::close(fd);
    return 0;
}

int directStatus() {
    try {
        SMC smc;
        Fans fans(smc);
        std::printf("daemon: none (direct read, root not required)\n");
        if (fans.list().empty()) {
            std::printf("fan: none (fanless Mac)\n");
            return 0;
        }
        for (const auto& status : fans.status()) {
            auto num = [](const std::optional<double>& value) {
                if (!value) return std::string("?");
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.0f", *value);
                return std::string(buffer);
            };
            const char* mode = !status.mode ? "?"
                               : *status.mode == 0 ? "auto"
                               : *status.mode == 1 ? "manual"
                               : *status.mode == 3 ? "system"
                                                   : "other";
            std::printf("  fan%d: actual=%s target=%s min=%s max=%s mode=%s\n", status.index,
                        num(status.actualRPM).c_str(), num(status.targetRPM).c_str(),
                        num(status.minimumRPM).c_str(), num(status.maximumRPM).c_str(), mode);
        }
        const auto cpuKeys = discoverTemperatureKeys(smc, {"Tp"});
        const auto gpuKeys = discoverTemperatureKeys(smc, {"Tg"});
        std::vector<double> cpuValues;
        for (const auto& reading : readTemperatures(smc, cpuKeys)) cpuValues.push_back(reading.second);
        std::vector<double> gpuValues;
        for (const auto& reading : readTemperatures(smc, gpuKeys)) gpuValues.push_back(reading.second);

        Settings settings = defaultSettings();
        const double cpu = aggregateTemperature(cpuValues, settings);
        const double gpu = gpuValues.empty() ? cpu : aggregateTemperature(gpuValues, settings);
        const auto hottest = hottestTemperature(smc, cpuKeys);
        std::printf("temps: cpu=%.1fC gpu=%.1fC max=%.1fC (%s)\n", cpu, gpu,
                    hottest ? hottest->second : 0.0,
                    hottest ? hottest->first.c_str() : "?");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fan: %s\n", error.what());
        return 1;
    }
}

std::string plistContent() {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n<dict>\n"
        << "  <key>Label</key><string>" << kLabel << "</string>\n"
        << "  <key>ProgramArguments</key><array><string>" << kInstallPath
        << "</string><string>daemon</string></array>\n"
        << "  <key>Sockets</key><dict><key>Listener</key><dict>\n"
        << "    <key>SockPathName</key><string>" << kSocketPath << "</string>\n"
        << "    <key>SockPathOwner</key><integer>0</integer>\n"
        << "    <key>SockPathGroup</key><integer>" << adminGid() << "</integer>\n"
        << "    <key>SockPathMode</key><integer>432</integer>\n"
        << "  </dict></dict>\n"
        << "  <key>RunAtLoad</key><true/>\n"
        << "  <key>KeepAlive</key><true/>\n"
        << "  <key>ProcessType</key><string>Background</string>\n"
        << "  <key>Nice</key><integer>10</integer>\n"
        << "  <key>ThrottleInterval</key><integer>10</integer>\n"
        << "</dict></plist>\n";
    return out.str();
}

int installDaemon() {
    if (geteuid() != 0) {
        std::fprintf(stderr, "fan: sudo required for install: sudo fan install\n");
        return 1;
    }

    ::mkdir("/usr/local", 0755);
    ::mkdir("/usr/local/libexec", 0755);
    ::mkdir("/usr/local/etc", 0755);
    ::mkdir("/usr/local/bin", 0755);

    const std::string self = executablePath();
    if (self.empty()) {
        std::fprintf(stderr, "fan: could not locate own executable\n");
        return 1;
    }

    if (self != kInstallPath) {
        std::ifstream input(self, std::ios::binary);
        std::ofstream output(kInstallPath, std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            std::fprintf(stderr, "fan: copy failed\n");
            return 1;
        }
        output << input.rdbuf();
    }
    ::chown(kInstallPath, 0, 0);
    ::chmod(kInstallPath, 0755);

    ::unlink(kSymlinkPath);
    if (::symlink(kInstallPath, kSymlinkPath) != 0)
        std::fprintf(stderr, "fan: symlink creation failed (ignored)\n");

    {
        std::ofstream plist(kPlistPath, std::ios::trunc);
        if (!plist) {
            std::fprintf(stderr, "fan: failed to write plist: %s\n", kPlistPath);
            return 1;
        }
        plist << plistContent();
    }
    ::chown(kPlistPath, 0, 0);
    ::chmod(kPlistPath, 0644);

    std::string bootout = "launchctl bootout system/" + std::string(kLabel) + " >/dev/null 2>&1";
    (void)std::system(bootout.c_str());
    ::usleep(300 * 1000);

    const std::string bootstrap = "launchctl bootstrap system " + std::string(kPlistPath);
    int rc = std::system(bootstrap.c_str());
    for (int attempt = 0; rc != 0 && attempt < 4; ++attempt) {
        // launchd may still be tearing down the previous instance; retry
        (void)std::system(bootout.c_str());
        ::usleep(500 * 1000);
        rc = std::system(bootstrap.c_str());
    }
    if (rc != 0) {
        std::fprintf(stderr, "fan: launchctl bootstrap failed; try manually: sudo launchctl bootstrap system %s\n",
                     kPlistPath);
        return 1;
    }

    std::printf("fan daemon installed.\n  binary: %s\n  socket: %s\n  config (optional): %s\n",
                kInstallPath, kSocketPath, kConfigPath);
    std::printf("Try: fan status\n");
    return 0;
}

int uninstallDaemon() {
    if (geteuid() != 0) {
        std::fprintf(stderr, "fan: sudo required for uninstall: sudo fan uninstall\n");
        return 1;
    }

    std::string command = "launchctl bootout system/" + std::string(kLabel) + " >/dev/null 2>&1";
    (void)std::system(command.c_str());

    ::unlink(kPlistPath);
    ::unlink(kSymlinkPath);
    ::unlink(kInstallPath);
    ::unlink(kSocketPath);

    std::printf("fan removed; fans are back under macOS auto control.\n");
    return 0;
}

void printUsage() {
    std::printf(
        "fan %s - macOS fan control (Apple Silicon)\n\n"
        "Usage:\n"
        "  fan status                  show fan status (works without daemon)\n"
        "  fan temps                   list discovered temperature sensors\n"
        "  fan set <rpm> [--fan N] [--ttl sec]\n"
        "  fan full                    hardware max RPM\n"
        "  fan auto                    release manual override (curve resumes)\n"
        "  fan curve on|off            enable/disable the embedded curve\n"
        "  sudo fan install            install daemon + socket (one-time)\n"
        "  sudo fan uninstall          remove\n"
        "  fan version\n",
        kVersion);
}

}  // namespace

}  // namespace fan

int main(int argc, char** argv) {
    using namespace fan;
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return args.empty() ? 1 : 0;
    }

    const std::string& command = args[0];

    if (command == "version" || command == "--version") {
        std::printf("fan %s\n", kVersion);
        return 0;
    }
    if (command == "daemon") return runDaemon(true);
    if (command == "install") return installDaemon();
    if (command == "uninstall") return uninstallDaemon();
    if (command == "temps" || command == "sensors") {
        try {
            SMC smc;
            const auto keys = discoverTemperatureKeys(smc, {"Tp", "Tg", "TC"});
            for (const auto& key : keys) {
                if (auto value = smc.read(key).number)
                    std::printf("%s  %.1f C\n", key.c_str(), *value);
            }
            if (auto hottest = hottestTemperature(smc, keys))
                std::printf("hottest: %s  %.1f C\n", hottest->first.c_str(), hottest->second);
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "fan: %s\n", error.what());
            return 1;
        }
    }
    if (command == "status") {
        const int fd = connectSocket();
        if (fd < 0) return directStatus();
        ::close(fd);
        return clientRequest("STATUS");
    }
    if (command == "set") {
        if (args.size() < 2) {
            std::fprintf(stderr, "fan: usage: fan set <rpm> [--fan N] [--ttl sec]\n");
            return 1;
        }
        double rpm = 0;
        try {
            rpm = std::stod(args[1]);
        } catch (...) {
            std::fprintf(stderr, "fan: invalid rpm\n");
            return 1;
        }
        std::string fanArg = "all";
        int ttl = 0;
        bool direct = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--fan" && i + 1 < args.size()) {
                fanArg = args[++i];
            } else if (args[i] == "--ttl" && i + 1 < args.size()) {
                ttl = std::atoi(args[++i].c_str());
            } else if (args[i] == "--direct") {
                direct = true;
            }
        }
        if (direct && geteuid() != 0) {
            std::fprintf(stderr, "fan: --direct requires sudo\n");
            return 1;
        }
        if (direct || (geteuid() == 0 && connectSocket() < 0)) {
            // no daemon + root: write directly (development/test mode)
            try {
                SMC smc;
                Fans fans(smc);
                if (fanArg == "all")
                    fans.setAllManual(rpm);
                else
                    fans.setManual(std::stoi(fanArg), rpm);
                std::printf("OK set %.0f (direct, no daemon)\n", rpm);
                return 0;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "fan: %s\n", error.what());
                return 1;
            }
        }
        return clientRequest("SET " + args[1] + " " + fanArg + " " + std::to_string(ttl));
    }
    if (command == "full") {
        const bool direct = std::find(args.begin(), args.end(), "--direct") != args.end();
        if (direct || (geteuid() == 0 && connectSocket() < 0)) {
            if (geteuid() != 0) {
                std::fprintf(stderr, "fan: --direct requires sudo\n");
                return 1;
            }
            try {
                SMC smc;
                Fans fans(smc);
                fans.setAllManual(fans.limits(0).second);
                std::printf("OK full (direct)\n");
                return 0;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "fan: %s\n", error.what());
                return 1;
            }
        }
        return clientRequest("FULL");
    }
    if (command == "auto") {
        const bool direct = std::find(args.begin(), args.end(), "--direct") != args.end();
        if (direct || (geteuid() == 0 && connectSocket() < 0)) {
            if (geteuid() != 0) {
                std::fprintf(stderr, "fan: --direct requires sudo\n");
                return 1;
            }
            try {
                SMC smc;
                Fans fans(smc);
                fans.setAllAuto();
                std::printf("OK auto (direct)\n");
                return 0;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "fan: %s\n", error.what());
                return 1;
            }
        }
        return clientRequest("AUTO");
    }
    if (command == "curve") {
        if (args.size() < 2 || (args[1] != "on" && args[1] != "off")) {
            std::fprintf(stderr, "fan: usage: fan curve on|off\n");
            return 1;
        }
        std::string upper = args[1] == "on" ? "ON" : "OFF";
        return clientRequest("CURVE " + upper);
    }

    std::fprintf(stderr, "fan: unknown command: %s\n", command.c_str());
    printUsage();
    return 1;
}
