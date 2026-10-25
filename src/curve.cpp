#include "curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fan {

CurvePoint CurvePoint::absolute(double tempC, double rpm) {
    CurvePoint point;
    point.tempC = tempC;
    point.kind = Kind::Absolute;
    point.value = rpm;
    return point;
}

CurvePoint CurvePoint::min(double tempC) {
    CurvePoint point;
    point.tempC = tempC;
    point.kind = Kind::Min;
    return point;
}

CurvePoint CurvePoint::max(double tempC) {
    CurvePoint point;
    point.tempC = tempC;
    point.kind = Kind::Max;
    return point;
}

Curve embeddedCurve() {
    Curve curve;
    // Mac mini: auto below 60C, then a gentle ramp; full blast only near
    // the thermal limit.
    curve.points = {
        CurvePoint::min(60),
        CurvePoint::absolute(68, 3000),
        CurvePoint::absolute(75, 3500),
        CurvePoint::max(85),
    };
    curve.hysteresisC = 3.0;
    curve.slewUpRPMPerSec = 500.0;
    curve.slewDownRPMPerSec = 150.0;
    curve.releaseBelowC = 60.0;
    curve.sensorPrefixes = {"Tp"};
    return curve;
}

Settings defaultSettings() { return Settings{}; }

namespace {

std::string trim(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

bool parseBool(const std::string& value) {
    return value == "true" || value == "1" || value == "yes";
}

bool parsePointToken(const std::string& token, CurvePoint& out) {
    const auto colon = token.find(':');
    if (colon == std::string::npos) return false;
    const std::string tempPart = trim(token.substr(0, colon));
    const std::string valuePart = trim(token.substr(colon + 1));
    if (tempPart.empty() || valuePart.empty()) return false;

    char* end = nullptr;
    const double temp = std::strtod(tempPart.c_str(), &end);
    if (end == tempPart.c_str()) return false;
    out.tempC = temp;

    if (valuePart == "min") {
        out.kind = CurvePoint::Kind::Min;
        return true;
    }
    if (valuePart == "max") {
        out.kind = CurvePoint::Kind::Max;
        return true;
    }
    const double rpm = std::strtod(valuePart.c_str(), &end);
    if (end == valuePart.c_str()) return false;
    out.kind = CurvePoint::Kind::Absolute;
    out.value = rpm;
    return true;
}

}  // namespace

bool loadConfig(const std::string& path, Curve& curve, Settings& settings) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    Curve loaded = curve;
    Settings loadedSettings = settings;

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));

        try {
            if (key == "sensors") {
                loaded.sensorPrefixes.clear();
                std::stringstream stream(value);
                std::string item;
                while (std::getline(stream, item, ',')) {
                    item = trim(item);
                    if (!item.empty()) loaded.sensorPrefixes.push_back(item);
                }
            } else if (key == "release_below") {
                loaded.releaseBelowC = std::stod(value);
            } else if (key == "hysteresis") {
                loaded.hysteresisC = std::stod(value);
            } else if (key == "slew_up") {
                loaded.slewUpRPMPerSec = std::stod(value);
            } else if (key == "slew_down") {
                loaded.slewDownRPMPerSec = std::stod(value);
            } else if (key == "tick") {
                loadedSettings.tickSec = std::stod(value);
            } else if (key == "engage_above") {
                loadedSettings.engageAboveC = std::stod(value);
            } else if (key == "engage_sustain") {
                loadedSettings.engageSustainTicks = std::max(1, std::stoi(value));
            } else if (key == "min_hold") {
                loadedSettings.minHoldSec = std::stod(value);
            } else if (key == "release_hysteresis") {
                loadedSettings.releaseHysteresisC = std::stod(value);
            } else if (key == "median_tolerance") {
                loadedSettings.medianToleranceC = std::stod(value);
            } else if (key == "include_gpu") {
                loadedSettings.includeGpu = parseBool(value);
            } else if (key == "respect_external") {
                loadedSettings.respectExternal = parseBool(value);
            } else if (key == "external_hold") {
                loadedSettings.externalHoldSec = std::stod(value);
            } else if (key == "thermal_guard") {
                loadedSettings.thermalGuardC = std::stod(value);
            } else if (key == "points") {
                std::vector<CurvePoint> points;
                std::stringstream stream(value);
                std::string item;
                bool ok = true;
                while (std::getline(stream, item, ',')) {
                    CurvePoint point;
                    if (!parsePointToken(trim(item), point)) {
                        ok = false;
                        break;
                    }
                    points.push_back(point);
                }
                if (ok && points.size() >= 2) loaded.points = points;
            }
        } catch (...) {
            // ignore malformed lines
        }
    }

    std::sort(loaded.points.begin(), loaded.points.end(),
              [](const CurvePoint& a, const CurvePoint& b) { return a.tempC < b.tempC; });

    curve = loaded;
    settings = loadedSettings;
    return true;
}

double aggregateTemperature(const std::vector<double>& values, const Settings& settings) {
    // Plausibility gate: drop unreadable/parked sensors (0) and nonsense values
    std::vector<double> plausible;
    plausible.reserve(values.size());
    for (double value : values) {
        if (value >= 5.0 && value <= 125.0) plausible.push_back(value);
    }
    if (plausible.empty()) return 0;

    std::vector<double> sorted = plausible;
    std::sort(sorted.begin(), sorted.end(), [](double a, double b) { return a > b; });
    const size_t n = sorted.size();
    const double median =
        (n % 2 == 1) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;

    double sum = 0;
    size_t count = 0;
    for (double value : plausible) {
        if (std::abs(value - median) <= settings.medianToleranceC) {
            sum += value;
            ++count;
        }
    }
    if (count == 0) return median;
    return sum / double(count);
}

CurveEngine::CurveEngine(Curve curve) : curve_(std::move(curve)) {}

void CurveEngine::reset() {
    lastEffectiveC_.reset();
    lastRPM_.reset();
    lastTimeSec_ = -1;
}

std::optional<double> CurveEngine::evaluate(double tempC, double fanMin, double fanMax,
                                            double nowSec) {
    if (curve_.points.size() < 2) return std::nullopt;

    if (tempC < curve_.releaseBelowC) {
        reset();
        return std::nullopt;
    }

    // Hysteresis (on the way down only): rising temperature is followed immediately
    double effective = tempC;
    if (lastEffectiveC_) {
        const double hysteresis = std::max(0.0, curve_.hysteresisC);
        if (tempC > *lastEffectiveC_) {
            effective = tempC;
        } else if (tempC <= *lastEffectiveC_ - hysteresis) {
            effective = tempC;
        } else {
            effective = *lastEffectiveC_;
        }
    }

    // Linear interpolation
    const auto& points = curve_.points;
    auto resolve = [&](const CurvePoint& point) -> double {
        switch (point.kind) {
            case CurvePoint::Kind::Absolute:
                return point.value;
            case CurvePoint::Kind::Min:
                return fanMin;
            case CurvePoint::Kind::Max:
                return fanMax;
        }
        return fanMin;
    };

    double target = 0;
    if (effective <= points.front().tempC) {
        target = resolve(points.front());
    } else if (effective >= points.back().tempC) {
        target = resolve(points.back());
    } else {
        for (size_t i = 1; i < points.size(); ++i) {
            if (effective <= points[i].tempC) {
                const auto& a = points[i - 1];
                const auto& b = points[i];
                const double span = b.tempC - a.tempC;
                const double t = span > 0 ? (effective - a.tempC) / span : 0.0;
                target = resolve(a) + (resolve(b) - resolve(a)) * t;
                break;
            }
        }
    }

    target = std::clamp(target, fanMin, fanMax);

    // Slew rate limiting
    if (lastRPM_ && lastTimeSec_ >= 0 && nowSec > lastTimeSec_) {
        const double dt = nowSec - lastTimeSec_;
        const double maxUp = curve_.slewUpRPMPerSec * dt;
        const double maxDown = curve_.slewDownRPMPerSec * dt;
        target = std::clamp(target, *lastRPM_ - maxDown, *lastRPM_ + maxUp);
    }

    lastEffectiveC_ = effective;
    lastRPM_ = target;
    lastTimeSec_ = nowSec;
    return target;
}

}  // namespace fan
