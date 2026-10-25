#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "smc.hpp"

namespace fan {

struct FanInfo {
    int index = 0;
    std::string actualKey;
    std::string targetKey;
    std::string minKey;
    std::string maxKey;
    std::optional<std::string> modeKey;
};

struct FanStatus {
    int index = 0;
    std::optional<double> actualRPM;
    std::optional<double> targetRPM;
    std::optional<double> minimumRPM;
    std::optional<double> maximumRPM;
    std::optional<int> mode;  // 0 auto, 1 manual, 3 system
};

class Fans {
public:
    explicit Fans(SMC& smc);

    const std::vector<FanInfo>& list() const { return fans_; }
    bool ftstAvailable() const { return ftstAvailable_; }

    std::vector<FanStatus> status() const;
    std::pair<double, double> limits(int index) const;

    // RPM is clamped to the fan's own min/max; writes are verified by read-back
    void setManual(int index, double rpm) const;
    void setAllManual(double rpm) const;
    void setAllAuto() const;
    void clearFtstIfSet() const;

private:
    const FanInfo& fanAt(int index) const;
    void unlockManual(const FanInfo& fan) const;
    std::optional<double> readNumber(const std::string& key) const;
    bool anyManual() const;

    SMC& smc_;
    std::vector<FanInfo> fans_;
    bool ftstAvailable_ = false;
};

// Discover all temperature keys present on this machine for the given prefixes
std::vector<std::string> discoverTemperatureKeys(const SMC& smc,
                                                 const std::vector<std::string>& prefixes);

// Hottest temperature among the keys: (key, celsius)
std::optional<std::pair<std::string, double>> hottestTemperature(
    const SMC& smc, const std::vector<std::string>& keys);

// All readable temperatures from the given keys: (key, celsius)
std::vector<std::pair<std::string, double>> readTemperatures(
    const SMC& smc, const std::vector<std::string>& keys);

}  // namespace fan
