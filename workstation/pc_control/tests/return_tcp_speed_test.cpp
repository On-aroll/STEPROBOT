#include "../workstation_motion.h"
#include <iostream>
#include <limits>
#include <random>

int main() {
    const JointPose start{}, target{1, -2, 3, -4, 5, -6}, rates{11, 10, 10, 17, 16, 22};
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) { if (!ok) { ++failures; std::cerr << reason << '\n'; } };
    for (const double speed : {-.1, 0., .05, 250.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        check(!limitReturnTcpSpeed(start, target, rates, 4330.5, speed, 250).error.empty(), "invalid TCP cap accepted");
    check(!limitReturnTcpSpeed(start, target, rates, 4330.5, 51, 50).error.empty(), "manual limit bypassed");
    check(!limitReturnTcpSpeed(start, target, rates, 0, 25, 250).error.empty(), "missing geometry accepted");
    check(!limitReturnTcpSpeed(start, target, rates, 4330.5, .1, 250).error.empty(), "unrepresentable override accepted");
    check(!limitReturnTcpSpeed(start, start, rates, 4330.5, 25, 250).error.empty(), "stationary target requires no move");
    auto invalidRates = rates;
    invalidRates[2] = std::numeric_limits<double>::quiet_NaN();
    check(!limitReturnTcpSpeed(start, target, invalidRates, 4330.5, 25, 250).error.empty(), "invalid manual profile accepted");

    std::mt19937 random(175);
    std::uniform_real_distribution<double> displacement(-90, 90), rate(5, 35), reach(1000, 6000);
    int accepted = 0;
    for (int n = 0; n < 128; ++n) {
        JointPose end{}, manual{};
        for (int i = 0; i < 6; ++i) { end[i] = displacement(random); manual[i] = rate(random); }
        if (n % 2 == 0) for (int i = 1; i < 6; ++i) end[i] = 0;
        const double radius = reach(random), maximum = *std::max_element(manual.begin(), manual.end());
        for (const double cap : {1., 25., 100., 250.}) {
            const auto result = limitReturnTcpSpeed(start, end, manual, radius, cap, 250);
            if (!result.error.empty()) continue;
            ++accepted;
            double tcpBound = 0;
            for (int i = 0; i < 6; ++i) if (end[i] != 0)
                tcpBound += radius * std::numbers::pi / 180 * manual[i] * result.jointSpeed / maximum;
            check(tcpBound <= cap + 1e-9, "commanded motion can exceed the TCP cap");
            check(result.jointSpeed > 0 && result.jointSpeed <= 10 && result.jointSpeed <= maximum, "joint limit bypassed");
            const auto longerTool = limitReturnTcpSpeed(start, end, manual, radius + 1000, cap, 250);
            check(!longerTool.error.empty() || longerTool.jointSpeed <= result.jointSpeed + 1e-9, "longer tool increased speed");
        }
    }
    check(accepted > 300, "insufficient representable speed cases");
    const auto limited = limitReturnTcpSpeed(start, JointPose{1, 0, 0, 0, 0, 0}, rates, 4330.5, 25, 250);
    const auto plan = planJointReturn(start, JointPose{1, 0, 0, 0, 0, 0}, JointPose{-180,-180,-180,-180,-180,-180},
                                     JointPose{180,180,180,180,180,180}, rates, limited.jointSpeed);
    check(limited.error.empty() && plan.error.empty() && plan.seconds < 300, "bounded speed cannot produce a short return plan");
    std::cout << accepted << " TCP-bound cases checked; failures=" << failures << '\n';
    return failures ? 1 : 0;
}
