#include "../src/curve.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace fan;

static int failures = 0;

static void expect(bool condition, const char* name) {
    if (condition) {
        std::printf("PASS  %s\n", name);
    } else {
        std::printf("FAIL  %s\n", name);
        ++failures;
    }
}

static void expectNear(double actual, double expected, double tolerance, const char* name) {
    if (std::abs(actual - expected) <= tolerance) {
        std::printf("PASS  %s (%.1f)\n", name, actual);
    } else {
        std::printf("FAIL  %s: expected %.1f, got %.1f\n", name, expected, actual);
        ++failures;
    }
}

int main() {
    {
        Settings settings = defaultSettings();
        expect(settings.engageAboveC == 70.0, "defaults: engage at 70C");
        expect(settings.engageSustainTicks == 5, "defaults: 5-tick sustain");
        expect(settings.minHoldSec == 30.0, "defaults: 30s min hold");
        expect(settings.releaseHysteresisC == 5.0, "defaults: 5C release hysteresis");
        expect(settings.respectExternal, "defaults: respect external control");
        expect(settings.thermalGuardC == 100.0, "defaults: 100C thermal guard");
    }
    {
        CurveEngine engine(embeddedCurve());
        expect(!engine.evaluate(59.0, 1000, 4900, 0).has_value(),
               "release below: 59C -> auto");
    }
    {
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(60.0, 1000, 4900, 0).value_or(-1), 1000, 1,
                   "60C -> min RPM");
    }
    {
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(64.0, 1000, 4900, 0).value_or(-1), 2000, 1,
                   "64C interpolation -> 2000");
    }
    {
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(68.0, 1000, 4900, 0).value_or(-1), 3000, 1,
                   "68C -> 3000 RPM");
    }
    {
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(75.0, 1000, 4900, 0).value_or(-1), 3500, 1,
                   "75C -> 3500 RPM");
    }
    {
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(90.0, 1000, 4900, 0).value_or(-1), 4900, 1,
                   "90C -> max RPM (4900)");
    }
    {
        // Interpolation: 71.5C sits halfway between 68->3000 and 75->3500
        CurveEngine engine(embeddedCurve());
        expectNear(engine.evaluate(71.5, 1000, 4900, 0).value_or(-1), 3250, 1,
                   "71.5C interpolation -> 3250");
    }
    {
        // Hysteresis: 70 -> 71 -> 69.5; RPM must not drop until below 71-3
        CurveEngine engine(embeddedCurve());
        (void)engine.evaluate(70.0, 1000, 4900, 0);
        auto up = engine.evaluate(71.0, 1000, 4900, 10);
        auto down = engine.evaluate(69.5, 1000, 4900, 20);
        expect(up.has_value() && down.has_value() && *down >= *up - 1.0,
               "hysteresis: RPM retained on small drop");
    }
    {
        // Slew: from 3000 RPM, jumping to 85C may rise at most 500 RPM in 1s
        Curve curve = embeddedCurve();
        curve.slewUpRPMPerSec = 500;
        CurveEngine engine(curve);
        auto first = engine.evaluate(68.0, 1000, 4900, 0);
        expectNear(first.value_or(-1), 3000, 1, "first tick target");
        auto second = engine.evaluate(85.0, 1000, 4900, 1);
        expectNear(second.value_or(-1), 3500, 1, "slew: max +500 RPM in 1s");
    }
    {
        Curve curve;
        curve.points = {CurvePoint::min(50), CurvePoint::max(90)};
        Settings settings;
        expect(loadConfig("/nonexistent/fan.conf", curve, settings) == false,
               "missing config -> false");
    }
    {
        Settings settings;
        settings.medianToleranceC = 15.0;
        expectNear(aggregateTemperature({40.0, 41.0, 74.0, 42.0, 43.0}, settings), 41.5, 0.01,
                   "aggregate: median-filtered average");
        expectNear(aggregateTemperature({40.0, 41.0, 74.0, 75.0, 42.0}, settings), 41.0, 0.01,
                   "aggregate: median filter drops two spikes");
        expectNear(aggregateTemperature({0.0, 40.0, 41.0, 42.0}, settings), 41.0, 0.01,
                   "aggregate: plausibility gate drops 0");
    }

    if (failures == 0) {
        std::printf("\nAll tests passed\n");
        return 0;
    }
    std::printf("\n%d tests failed\n", failures);
    return 1;
}
