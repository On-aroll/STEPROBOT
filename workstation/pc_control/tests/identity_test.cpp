#include "../workstation_identity.h"

#include <iostream>

int main() {
    int failures = 0;
    for (const auto name : {"SR175-3200", "SR175/3200"}) {
        if (!isSupportedStepRobot(name)) { ++failures; std::cerr << "Rejected supported model: " << name << '\n'; }
    }
    for (const auto name : {"", "SR175", "SR175/2700", "SR220/3200", "SR175/3200-unknown", "SR175/3200 "}) {
        if (isSupportedStepRobot(name)) { ++failures; std::cerr << "Accepted unsupported model: " << name << '\n'; }
    }
    std::cout << "robot_identity_aliases: failures=" << failures << '\n';
    return failures ? 1 : 0;
}
