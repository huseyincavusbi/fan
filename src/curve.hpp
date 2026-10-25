#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fan {

struct CurvePoint {
    enum class Kind { Absolute, Min, Max };

    double tempC = 0;
    Kind kind = Kind::Absolute;
    double value = 0;  // Absolute: RPM

    static CurvePoint absolute(double tempC, double rpm);
    static CurvePoint min(double tempC);
    static CurvePoint max(double tempC);
};

struct Curve {
    std::vector<CurvePoint> points;
    double hysteresisC = 3.0;
    double slewUpRPMPerSec = 500.0;
    double slewDownRPMPerSec = 150.0;
    double releaseBelowC = 60.0;
    std::vector<std::string> sensorPrefixes{"Tp"};
};

struct Settings {
    bool respectExternal = true;
    double externalHoldSec = 300.0;
    double thermalGuardC = 100.0;
    double tickSec = 3.0;
    // Engagement gate: require this many consecutive ticks at or above
    // engageAboveC before the curve engages at all.
    double engageAboveC = 70.0;
    int engageSustainTicks = 5;
    // Once engaged, stay engaged at least this long before releasing (seconds)
    double minHoldSec = 30.0;
    // Release only when the temperature falls this far below the threshold
    double releaseHysteresisC = 5.0;
    // Median-filtered average: readings further than this from the median are dropped
    double medianToleranceC = 15.0;
    // Include the GPU (Tg) sensors: the control temperature becomes max(cpu, gpu)
    bool includeGpu = true;
};

// Embedded default tuned for the Mac mini (Apple Silicon)
Curve embeddedCurve();
Settings defaultSettings();

// Overrides embedded values if the config file exists
bool loadConfig(const std::string& path, Curve& curve, Settings& settings);

// Median-filtered average of the sensor readings
double aggregateTemperature(const std::vector<double>& values, const Settings& settings);

class CurveEngine {
public:
    explicit CurveEngine(Curve curve);

    // Target RPM; returns nullopt below releaseBelowC (hand fans back to macOS auto)
    std::optional<double> evaluate(double tempC, double fanMin, double fanMax, double nowSec);

    const Curve& curve() const { return curve_; }
    void reset();

private:
    Curve curve_;
    std::optional<double> lastEffectiveC_;
    std::optional<double> lastRPM_;
    double lastTimeSec_ = -1;
};

}  // namespace fan
