#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using JointPose = std::array<double, 6>;

struct JointReturnPlan {
    JointPose start{}, target{};
    std::vector<JointPose> waypoints, samples;
    double speed = 0, tcpSpeed = 0, seconds = 0;
    std::string error;
};

struct ReturnSpeedLimit {
    double jointSpeed = 0;
    std::string error;
};

inline ReturnSpeedLimit limitReturnTcpSpeed(const JointPose& start, const JointPose& target,
                                          const JointPose& manualSpeed, double radiusMm,
                                          double requestedMmS, double manualMmS) {
    ReturnSpeedLimit result;
    if (!std::isfinite(radiusMm) || radiusMm <= 0 || !std::isfinite(manualMmS) || manualMmS <= 0 ||
        !std::isfinite(requestedMmS) || requestedMmS < .1 || requestedMmS > std::min(250., manualMmS)) {
        result.error = "TCP limit must be 0.1..250 mm/s and within the manual limit";
        return result;
    }
    double maximum = 0, movingRates = 0;
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(start[i]) || !std::isfinite(target[i]) || !std::isfinite(manualSpeed[i]) || manualSpeed[i] <= 0) {
            result.error = "invalid joint feedback or manual speed";
            return result;
        }
        maximum = std::max(maximum, manualSpeed[i]);
        if (std::abs(target[i] - start[i]) > 1e-8) movingRates += manualSpeed[i];
    }
    if (movingRates == 0) { result.error = "already at the return target"; return result; }
    // For revolute joints, |v_TCP| <= sum(radius_i * |omega_i|).
    // The full link-length sum plus tool length bounds every joint-to-TCP radius.
    const double boundMmS = radiusMm * std::numbers::pi / 180 * movingRates;
    const double percent = 100 * std::min({.9 * requestedMmS / boundMmS, 10. / maximum, 1.});
    const double quantized = percent < 1 ? std::floor(percent * 10) / 10 : std::floor(percent);
    if (quantized < .1) {
        result.error = "TCP limit is below the controller override resolution; motion was not started";
        return result;
    }
    result.jointSpeed = maximum * quantized / 100;
    return result;
}

inline JointReturnPlan planJointReturn(const JointPose& start, const JointPose& target, const JointPose& lower,
                                      const JointPose& upper, const JointPose& hardwareSpeed, double speed) {
    JointReturnPlan plan;
    plan.start = start;
    plan.target = target;
    plan.speed = speed;
    double distance = 0, maximum = 0;
    if (!std::isfinite(speed) || speed < .001 || speed > 10) { plan.error = "internal joint speed must be 0.001..10 deg/s"; return plan; }
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(start[i]) || !std::isfinite(target[i]) || !std::isfinite(lower[i]) || !std::isfinite(upper[i]) ||
            !std::isfinite(hardwareSpeed[i]) || hardwareSpeed[i] <= 0 || lower[i] >= upper[i] ||
            start[i] < lower[i] || start[i] > upper[i] || target[i] < lower[i] || target[i] > upper[i]) {
            plan.error = "start or target exceeds verified joint limits"; return plan;
        }
        distance = std::max(distance, std::abs(target[i] - start[i]));
        maximum = std::max(maximum, hardwareSpeed[i]);
    }
    if (speed > maximum) { plan.error = "return speed exceeds the verified manual joint speed"; return plan; }
    if (distance > 1024) { plan.error = "joint displacement exceeds planner bound"; return plan; }
    for (int i = 0; i < 6; ++i)
        plan.seconds = std::max(plan.seconds, std::abs(target[i] - start[i]) * maximum / (hardwareSpeed[i] * speed));
    const int count = std::clamp(static_cast<int>(std::ceil(distance / 15)), 1, 24);
    const int sampleCount = std::max(1, static_cast<int>(std::ceil(distance / 5)));
    const auto interpolate = [&](double u) {
        JointPose pose{};
        for (int i = 0; i < 6; ++i) pose[i] = start[i] + (target[i] - start[i]) * u;
        return pose;
    };
    for (int i = 0; i <= sampleCount; ++i) plan.samples.push_back(interpolate(static_cast<double>(i) / sampleCount));
    for (int i = 1; i <= count; ++i) plan.waypoints.push_back(interpolate(static_cast<double>(i) / count));
    plan.seconds += count * 10.0 + 5.0;
    if (plan.seconds > 300) plan.error = "estimated return exceeds 300 seconds; increase speed or shorten travel";
    return plan;
}
