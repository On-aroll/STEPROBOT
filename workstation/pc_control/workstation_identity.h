#pragma once

#include <string_view>

inline constexpr bool isSupportedStepRobot(std::string_view name) {
    return name == "SR175-3200" || name == "SR175/3200";
}
