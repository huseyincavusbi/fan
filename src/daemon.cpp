#include "app.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <grp.h>
#include <launch.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <unistd.h>

#include "curve.hpp"
#include "fans.hpp"
#include "smc.hpp"
#include "util.hpp"

#ifndef SOL_LOCAL
#define SOL_LOCAL 0
#endif
#ifndef LOCAL_PEERCRED
#define LOCAL_PEERCRED 0x001
#endif

namespace fan {

namespace {

volatile sig_atomic_t gTerm = 0;
volatile sig_atomic_t gHup = 0;
int gSignalPipe[2] = {-1, -1};

void handleSignal(int sig) {
    if (sig == SIGHUP)
        gHup = 1;
    else
        gTerm = 1;
    const char byte = 1;
    (void)!write(gSignalPipe[1], &byte, 1);
}

double seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

gid_t adminGid() {
    if (struct group* group = getgrnam("admin")) return group->gr_gid;
    return 0;
}

bool peerAllowed(int fd) {
    struct xucred cred {};
    socklen_t length = sizeof(cred);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &length) != 0) return false;
    if (cred.cr_uid == 0) return true;
    const gid_t admin = adminGid();
    if (admin != 0) {
        for (int i = 0; i < cred.cr_ngroups; ++i)
            if (cred.cr_groups[i] == admin) return true;
    }
    return false;
}

Fd openListenSocket(const std::string& path, bool& fromLaunchd) {
    int* fds = nullptr;
    size_t count = 0;
    if (launch_activate_socket("Listener", &fds, &count) == 0 && count > 0 && fds != nullptr) {
        Fd fd(fds[0]);
        for (size_t i = 1; i < count; ++i) ::close(fds[i]);
        free(fds);
        fromLaunchd = true;
        return fd;
    }

    ::unlink(path.c_str());
    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) throw Error("socket creation failed");

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw Error("socket bind failed: " + path + " (" + std::strerror(errno) + ")");

    if (geteuid() == 0) {
        ::chown(path.c_str(), 0, adminGid());
        ::chmod(path.c_str(), 0660);
    } else {
        ::chmod(path.c_str(), 0600);
    }

    if (::listen(fd.get(), 8) != 0) throw Error("listen failed");
    fromLaunchd = false;
    return fd;
}

std::string numberOr(const std::optional<double>& value) {
    if (!value) return "?";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", *value);
    return buffer;
}

std::string modeName(const std::optional<int>& mode) {
    if (!mode) return "?";
    switch (*mode) {
        case 0: return "auto";
        case 1: return "manual";
        case 3: return "system";
        default: return "other";
    }
}

}  // namespace

int runDaemon(bool foreground) {
    signal(SIGPIPE, SIG_IGN);
    if (pipe(gSignalPipe) != 0) return 1;

    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);

    try {
        SMC smc;
        Fans fans(smc);
        Curve curve = embeddedCurve();
        Settings settings = defaultSettings();
        if (loadConfig(kConfigPath, curve, settings) && foreground)
            std::fprintf(stderr, "fan: config loaded: %s\n", kConfigPath);

        CurveEngine engine(curve);
        std::vector<std::string> tempKeys = discoverTemperatureKeys(smc, curve.sensorPrefixes);
        if (tempKeys.empty())
            tempKeys = discoverTemperatureKeys(smc, {"Tp", "Ts", "Tg", "TC"});
        std::vector<std::string> gpuKeys = discoverTemperatureKeys(smc, {"Tg"});

        const std::string path = socketPath();
        bool fromLaunchd = false;
        Fd listenFd = openListenSocket(path, fromLaunchd);

        if (foreground)
            std::fprintf(stderr, "fan daemon %s (fans %zu, sensors %zu, socket %s%s)\n",
                         kVersion, fans.list().size(), tempKeys.size(),
                         path.c_str(), fromLaunchd ? " (launchd)" : "");

        struct State {
            bool curveEnabled = true;
            bool curvePaused = false;
            bool external = false;
            double externalHoldUntil = 0;
            std::optional<double> lastApplied;
            std::optional<double> ttlDeadline;
            double nextTickAt = 0;
            int hotStreak = 0;
            double engagedAt = 0;
            std::string triggerKey;
            double triggerTemp = 0;
        } state;

        for (const auto& status : fans.status()) {
            if (status.mode && *status.mode == 1) {
                state.external = true;
                break;
            }
        }
        if (state.external) {
            state.externalHoldUntil = seconds() + settings.externalHoldSec;
            if (foreground)
                std::fprintf(stderr,
                             "fan: external control detected at startup (manual mode), backing off\n");
        }
        state.nextTickAt = seconds() + settings.tickSec;

        auto needTicks = [&]() {
            return (state.curveEnabled && !state.curvePaused) || state.lastApplied.has_value() ||
                   state.external || state.ttlDeadline.has_value();
        };

        // Current CPU/GPU/control temperatures for one tick
        struct Temps {
            double cpu = 0;
            double gpu = 0;
            double curve = 0;
            double rawMax = 0;
            std::string rawKey;
            std::string curveLabel;
        };
        auto measure = [&]() -> Temps {
            Temps temps;

            auto cpu = readTemperatures(smc, tempKeys);
            std::sort(cpu.begin(), cpu.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::vector<double> cpuValues;
            cpuValues.reserve(cpu.size());
            for (const auto& reading : cpu) cpuValues.push_back(reading.second);
            temps.cpu = aggregateTemperature(cpuValues, settings);
            if (!cpu.empty()) {
                temps.rawMax = cpu.front().second;
                temps.rawKey = cpu.front().first;
            }
            std::string cpuLabel;
            if (!cpu.empty()) cpuLabel = "median-avg";

            auto gpu = readTemperatures(smc, gpuKeys);
            std::vector<double> gpuValues;
            gpuValues.reserve(gpu.size());
            for (const auto& reading : gpu) gpuValues.push_back(reading.second);
            temps.gpu = gpuValues.empty() ? temps.cpu : aggregateTemperature(gpuValues, settings);

            if (settings.includeGpu && temps.gpu > temps.cpu) {
                temps.curve = temps.gpu;
                temps.curveLabel = "gpu";
            } else {
                temps.curve = temps.cpu;
                temps.curveLabel = cpuLabel.empty() ? "cpu" : cpuLabel;
            }
            return temps;
        };

        auto statusText = [&]() {
            std::string out;
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "daemon %s (fans %zu, sensors %zu)\n",
                          kVersion, fans.list().size(), tempKeys.size());
            out += buffer;
            out += "curve: ";
            out += !state.curveEnabled ? "off" : (state.curvePaused ? "paused" : "on");
            std::snprintf(buffer, sizeof(buffer), " (engage >= %.0f C x %d ticks, hold %.0f s)\n",
                          settings.engageAboveC, settings.engageSustainTicks, settings.minHoldSec);
            out += buffer;
            out += "control: ";
            out += state.external ? "external" : (state.lastApplied ? "fan" : "system");
            out += "\n";
            if (state.external) {
                std::snprintf(buffer, sizeof(buffer), "external hold: %.0f s\n",
                              std::max(0.0, state.externalHoldUntil - seconds()));
                out += buffer;
            }
            if (state.ttlDeadline) {
                std::snprintf(buffer, sizeof(buffer), "ttl: %.0f s\n",
                              std::max(0.0, *state.ttlDeadline - seconds()));
                out += buffer;
            }
            if (state.lastApplied) {
                std::snprintf(buffer, sizeof(buffer), "engaged by: %s %.1f C\n",
                              state.triggerKey.c_str(), state.triggerTemp);
                out += buffer;
            }
            out += "fans:\n";
            for (const auto& status : fans.status()) {
                std::snprintf(buffer, sizeof(buffer),
                              "  fan%d: actual=%s target=%s min=%s max=%s mode=%s\n",
                              status.index, numberOr(status.actualRPM).c_str(),
                              numberOr(status.targetRPM).c_str(),
                              numberOr(status.minimumRPM).c_str(),
                              numberOr(status.maximumRPM).c_str(),
                              modeName(status.mode).c_str());
                out += buffer;
            }
            if (!tempKeys.empty() || !gpuKeys.empty()) {
                const Temps temps = measure();
                std::snprintf(buffer, sizeof(buffer),
                              "temps: max=%.1fC (%s) cpu=%.1fC gpu=%.1fC curve=%.1fC (%s)\n",
                              temps.rawMax, temps.rawKey.c_str(), temps.cpu, temps.gpu,
                              temps.curve, temps.curveLabel.c_str());
                out += buffer;
            }
            return out;
        };

        auto respond = [](int fd, const std::string& text) {
            std::string out = text;
            if (out.empty() || out.back() != '\n') out += '\n';
            (void)!write(fd, out.c_str(), out.size());
        };

        auto handleClient = [&](int fd) {
            if (!peerAllowed(fd)) {
                respond(fd, "ERR unauthorized (admin group required)");
                return;
            }
            char buffer[512] = {};
            const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) return;
            std::string line(buffer, size_t(n));
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

            std::istringstream stream(line);
            std::string command;
            stream >> command;

            try {
                if (command == "PING") {
                    respond(fd, std::string("PONG ") + kVersion);
                } else if (command == "STATUS") {
                    respond(fd, statusText());
                } else if (command == "SET") {
                    double rpm = 0;
                    std::string fanArg = "all";
                    int ttl = 0;
                    stream >> rpm;
                    if (!(stream >> fanArg)) fanArg = "all";
                    stream >> ttl;
                    if (rpm <= 0) {
                        respond(fd, "ERR invalid rpm");
                        return;
                    }
                    if (fanArg == "all")
                        fans.setAllManual(rpm);
                    else
                        fans.setManual(std::stoi(fanArg), rpm);
                    state.lastApplied = rpm;
                    state.external = false;
                    state.curvePaused = true;
                    state.ttlDeadline.reset();
                    if (ttl > 0) {
                        state.ttlDeadline = seconds() + ttl;
                        state.nextTickAt = std::min(state.nextTickAt, *state.ttlDeadline);
                    }
                    char reply[128];
                    std::snprintf(reply, sizeof(reply), "OK set %.0f", rpm);
                    respond(fd, reply);
                } else if (command == "FULL") {
                    const double maximum = fans.limits(0).second;
                    fans.setAllManual(maximum);
                    state.lastApplied = maximum;
                    state.external = false;
                    state.curvePaused = true;
                    state.ttlDeadline.reset();
                    respond(fd, "OK full");
                } else if (command == "AUTO") {
                    fans.setAllAuto();
                    state.lastApplied.reset();
                    state.ttlDeadline.reset();
                    state.curvePaused = false;
                    respond(fd, "OK auto (curve resumed)");
                } else if (command == "CURVE") {
                    std::string sub;
                    stream >> sub;
                    if (sub == "ON") {
                        state.curveEnabled = true;
                        state.curvePaused = false;
                        respond(fd, "OK curve on");
                    } else if (sub == "OFF") {
                        state.curveEnabled = false;
                        state.curvePaused = true;
                        if (state.lastApplied) {
                            fans.setAllAuto();
                            state.lastApplied.reset();
                        }
                        respond(fd, "OK curve off");
                    } else {
                        respond(fd, "ERR usage: CURVE ON|OFF");
                    }
                } else {
                    respond(fd, "ERR unknown command");
                }
            } catch (const std::exception& error) {
                respond(fd, std::string("ERR ") + error.what());
            }
        };

        auto tick = [&]() {
            const double now = seconds();
            auto status = fans.status();

            if (state.ttlDeadline && now >= *state.ttlDeadline) {
                try {
                    fans.setAllAuto();
                } catch (...) {
                }
                state.lastApplied.reset();
                state.ttlDeadline.reset();
                state.curvePaused = false;  // return control to the curve
            }

            if (!fans.list().empty()) {
                if (state.lastApplied && !status.empty()) {
                    const auto& first = status.front();
                    if (first.mode && *first.mode == 1 && first.targetRPM &&
                        std::abs(*first.targetRPM - *state.lastApplied) >
                            std::max(5.0, *state.lastApplied * 0.02)) {
                        state.external = true;
                        state.externalHoldUntil = now + settings.externalHoldSec;
                        state.lastApplied.reset();
                        if (foreground)
                            std::fprintf(stderr,
                                         "fan: external control detected (target %.0f), backing off\n",
                                         *first.targetRPM);
                    }
                }
                if (state.external) {
                    if (now < state.externalHoldUntil) return;
                    bool stillManual = false;
                    for (const auto& fanStatus : status)
                        if (fanStatus.mode && *fanStatus.mode == 1) stillManual = true;
                    if (stillManual) {
                        state.externalHoldUntil = now + settings.externalHoldSec;
                        return;
                    }
                    state.external = false;
                    if (foreground)
                        std::fprintf(stderr, "fan: external control released, resuming curve\n");
                }
            }

            if (!state.curveEnabled || state.curvePaused) return;

            if (fans.list().empty()) return;
            const Temps temps = measure();
            const double curveTemp = temps.curve;
            const std::string curveKey = temps.curveLabel;
            if (curveTemp <= 0) return;

            if (curveTemp >= settings.thermalGuardC) {
                try {
                    fans.setAllAuto();
                } catch (...) {
                }
                state.lastApplied.reset();
                state.curvePaused = true;
                if (foreground)
                    std::fprintf(stderr, "fan: thermal guard (%.1fC), curve paused\n", curveTemp);
                return;
            }

            // Engagement gate: only sustained heat at or above the engage
            // threshold may start the fans (brief flickers never engage)
            if (!state.lastApplied) {
                if (curveTemp >= settings.engageAboveC)
                    state.hotStreak++;
                else
                    state.hotStreak = 0;
                if (state.hotStreak < settings.engageSustainTicks) return;
            } else {
                state.hotStreak = 0;
            }

            const auto [fanMin, fanMax] = fans.limits(0);
            const auto target = engine.evaluate(curveTemp, fanMin, fanMax, now);
            if (!target) {
                if (state.lastApplied) {
                    // Minimum hold + release hysteresis: avoid short on/off bursts
                    const bool coolEnough =
                        curveTemp < curve.releaseBelowC - settings.releaseHysteresisC;
                    const bool heldEnough = now - state.engagedAt >= settings.minHoldSec;
                    if (coolEnough && heldEnough) {
                        try {
                            fans.setAllAuto();
                        } catch (...) {
                        }
                        state.lastApplied.reset();
                    }
                }
                return;
            }

            bool oursActive = false;
            if (state.lastApplied) {
                for (const auto& fanStatus : status) {
                    if (fanStatus.mode && *fanStatus.mode == 1 && fanStatus.targetRPM &&
                        std::abs(*fanStatus.targetRPM - *state.lastApplied) < 5.0) {
                        oursActive = true;
                        break;
                    }
                }
            }
            if (!state.lastApplied || !oursActive ||
                std::abs(*target - *state.lastApplied) > 25.0) {
                try {
                    fans.setAllManual(*target);
                    if (!state.lastApplied) state.engagedAt = now;
                    state.triggerKey = curveKey;
                    state.triggerTemp = curveTemp;
                    state.lastApplied = *target;
                } catch (const std::exception& error) {
                    if (foreground)
                        std::fprintf(stderr, "fan: write error: %s\n", error.what());
                }
            }
        };

        while (!gTerm) {
            int timeout = -1;
            if (needTicks()) {
                double wakeAt = state.nextTickAt;
                if (state.external) wakeAt = std::min(wakeAt, state.externalHoldUntil);
                if (state.ttlDeadline) wakeAt = std::min(wakeAt, *state.ttlDeadline);
                double milliseconds = (wakeAt - seconds()) * 1000.0;
                milliseconds = std::clamp(milliseconds, 0.0, settings.tickSec * 1000.0);
                timeout = int(milliseconds);
            }

            struct pollfd fds[2] = {{listenFd.get(), POLLIN, 0}, {gSignalPipe[0], POLLIN, 0}};
            const int ready = poll(fds, 2, timeout);
            if (ready < 0 && errno != EINTR) break;
            if (gTerm) break;

            if (gHup) {
                gHup = 0;
                Curve reloaded = embeddedCurve();
                Settings reloadedSettings = defaultSettings();
                if (loadConfig(kConfigPath, reloaded, reloadedSettings)) {
                    curve = reloaded;
                    settings = reloadedSettings;
                    engine = CurveEngine(curve);
                    tempKeys = discoverTemperatureKeys(smc, curve.sensorPrefixes);
                    if (tempKeys.empty())
                        tempKeys = discoverTemperatureKeys(smc, {"Tp", "Ts", "Tg", "TC"});
                    gpuKeys = discoverTemperatureKeys(smc, {"Tg"});
                    if (foreground) std::fprintf(stderr, "fan: config reloaded\n");
                }
            }

            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (::read(gSignalPipe[0], drain, sizeof(drain)) > 0) {
                }
            }

            if (fds[0].revents & POLLIN) {
                Fd client(::accept(listenFd.get(), nullptr, nullptr));
                if (client.valid()) handleClient(client.get());
            }

            if (needTicks() && seconds() >= state.nextTickAt) {
                tick();
                state.nextTickAt = seconds() + settings.tickSec;
            }
        }

        try {
            fans.setAllAuto();
        } catch (...) {
        }
        if (!fromLaunchd) ::unlink(path.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fan daemon error: %s\n", error.what());
        return 1;
    }
}

}  // namespace fan
