#include "fans.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace fan {

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

Fans::Fans(SMC& smc) : smc_(smc) {
    int declaredCount = 0;
    try {
        if (auto value = smc_.read("FNum").number) declaredCount = int(*value);
    } catch (...) {
        // FNum may be missing on fanless Macs; fan keys are probed individually below
    }

    for (int i = 0; i < 8; ++i) {
        FanInfo info;
        info.index = i;
        const std::string prefix = "F" + std::to_string(i);
        info.actualKey = prefix + "Ac";
        info.targetKey = prefix + "Tg";
        info.minKey = prefix + "Mn";
        info.maxKey = prefix + "Mx";

        if (!smc_.hasKey(info.actualKey)) continue;

        for (const char* suffix : {"Md", "md"}) {
            const std::string key = prefix + suffix;
            if (smc_.hasKey(key)) {
                info.modeKey = key;
                break;
            }
        }
        fans_.push_back(info);
    }

    if (declaredCount > 0 && int(fans_.size()) > declaredCount)
        fans_.resize(size_t(declaredCount));

    ftstAvailable_ = smc_.hasKey("Ftst");
}

const FanInfo& Fans::fanAt(int index) const {
    for (const auto& fan : fans_)
        if (fan.index == index) return fan;
    throw Error("fan " + std::to_string(index) + " not found");
}

std::optional<double> Fans::readNumber(const std::string& key) const {
    try {
        return smc_.read(key).number;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<FanStatus> Fans::status() const {
    std::vector<FanStatus> out;
    for (const auto& fan : fans_) {
        FanStatus status;
        status.index = fan.index;
        status.actualRPM = readNumber(fan.actualKey);
        status.targetRPM = readNumber(fan.targetKey);
        status.minimumRPM = readNumber(fan.minKey);
        status.maximumRPM = readNumber(fan.maxKey);
        if (fan.modeKey) {
            if (auto mode = readNumber(*fan.modeKey)) status.mode = int(std::lround(*mode));
        }
        out.push_back(status);
    }
    return out;
}

std::pair<double, double> Fans::limits(int index) const {
    const FanInfo& fan = fanAt(index);
    double minimum = readNumber(fan.minKey).value_or(0);
    double maximum = readNumber(fan.maxKey).value_or(0);
    if (maximum <= 0) maximum = 100000;
    return {minimum, maximum};
}

void Fans::unlockManual(const FanInfo& fan) const {
    if (!fan.modeKey)
        throw Error("no mode key for fan " + std::to_string(fan.index));

    try {
        smc_.write(*fan.modeKey, {1});
        return;
    } catch (...) {
        if (!ftstAvailable_)
            throw Error("manual control unsupported (no Ftst)");
    }

    smc_.write("Ftst", {1});
    sleepMs(500);

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        try {
            smc_.write(*fan.modeKey, {1});
            return;
        } catch (...) {
            sleepMs(100);
        }
    }
    throw Error("Ftst unlock failed within 10s");
}

void Fans::setManual(int index, double rpm) const {
    const FanInfo& fan = fanAt(index);
    auto [minimum, maximum] = limits(index);
    const double target = std::clamp(rpm, minimum, maximum);

    unlockManual(fan);
    smc_.write(fan.targetKey, encodeFloatLE(float(target)));

    // SMC writes can apply asynchronously; verify by read-back with retries
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (auto value = readNumber(fan.targetKey)) {
            const double tolerance = std::max(1.0, target * 0.001);
            if (std::abs(*value - target) <= tolerance) return;
        }
        sleepMs(200);
    }
    throw Error("target RPM verification failed: " + fan.targetKey);
}

void Fans::setAllManual(double rpm) const {
    for (const auto& fan : fans_) setManual(fan.index, rpm);
}

bool Fans::anyManual() const {
    for (const auto& fan : fans_) {
        if (!fan.modeKey) continue;
        if (auto value = readNumber(*fan.modeKey); value && std::lround(*value) == 1)
            return true;
    }
    return false;
}

void Fans::setAllAuto() const {
    std::string lastError;
    bool anyAttempt = false;
    for (const auto& fan : fans_) {
        if (!fan.modeKey) continue;
        anyAttempt = true;
        try {
            smc_.write(*fan.modeKey, {0});
        } catch (const std::exception& error) {
            lastError = error.what();
        }
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!anyManual()) {
            clearFtstIfSet();
            return;
        }
        sleepMs(150);
    }

    if (anyAttempt)
        throw Error(lastError.empty() ? std::string("auto mode release not verified") : lastError);
}

void Fans::clearFtstIfSet() const {
    if (!ftstAvailable_) return;
    if (auto value = readNumber("Ftst"); value && std::lround(*value) == 1) {
        try {
            smc_.write("Ftst", {0});
        } catch (...) {
        }
    }
}

std::vector<std::string> discoverTemperatureKeys(const SMC& smc,
                                                 const std::vector<std::string>& prefixes) {
    std::vector<std::string> keys;
    for (const auto& prefix : prefixes) {
        for (int i = 0; i < 40; ++i) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%s%02d", prefix.c_str(), i);
            if (smc.hasKey(buffer)) keys.emplace_back(buffer);
        }
        for (const char* suffix : {"0P", "1P", "2P", "3P", "0C", "0D", "0E", "0F", "0H", "0M"}) {
            const std::string key = prefix + suffix;
            if (smc.hasKey(key)) keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::optional<std::pair<std::string, double>> hottestTemperature(
    const SMC& smc, const std::vector<std::string>& keys) {
    std::optional<std::pair<std::string, double>> hottest;
    for (const auto& key : keys) {
        try {
            if (auto value = smc.read(key).number) {
                if (!hottest || *value > hottest->second) hottest = {key, *value};
            }
        } catch (...) {
        }
    }
    return hottest;
}

std::vector<std::pair<std::string, double>> readTemperatures(
    const SMC& smc, const std::vector<std::string>& keys) {
    std::vector<std::pair<std::string, double>> readings;
    readings.reserve(keys.size());
    for (const auto& key : keys) {
        try {
            if (auto value = smc.read(key).number) readings.push_back({key, *value});
        } catch (...) {
        }
    }
    return readings;
}

}  // namespace fan
