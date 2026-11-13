#pragma once

#include <iostream>
#include <string>
#include <vector>

// RobotCom.hpp is included by the caller. Keep the SDK calls here testable without a connection.
namespace StepControl {
inline std::string projectBase(std::string name) {
    if (name.ends_with(".sr")) name.resize(name.size() - 3);
    return name;
}

template<class Client>
void error(Client& client, const char* operation, const char* stage, int rc, const char* target = "none") {
    const auto message = client.getMessageData();
    std::cout << "control_error operation=" << operation << " stage=" << stage << " rc=" << rc
              << " target=" << target << " mode=" << int(client.getOperationMode())
              << " enabled=" << client.getMotorEnableState() << " state=" << int(client.getProgramState())
              << " project=" << client.getProjectName() << " program=" << client.getProgramName()
              << " msgid=" << message.m_MessageID << '\n';
}

template<class Client, class Wait>
bool changeMode(Client& client, bool manual, bool stopped, Wait wait) {
    using namespace STEPROBOTSDK;
    const auto target = manual ? "manual" : "auto";
    if (!stopped || client.getMessageData().m_MessageType == eError) {
        error(client, "mode", "not_stopped_or_alarm", -1, target);
        return false;
    }
    const auto matches = [&] {
        const auto mode = client.getOperationMode();
        return manual ? mode == eManual || mode == eManualHigh : mode == eAutomatic || mode == eAutomaticExternal;
    };
    if (!matches()) {
        const int rc = client.SetModeCmd(manual ? MANUAL : AUTO, false);
        if (rc != 0 || !wait(matches)) {
            error(client, "mode", rc != 0 ? "request" : "feedback", rc, target);
            return false;
        }
    }
    // Switching mode neither unloads the program nor enables the servos/changes override.
    std::cout << "mode_change_ok target=" << target << " mode=" << int(client.getOperationMode())
              << " enabled=" << client.getMotorEnableState() << " program=" << client.getProgramName() << '\n';
    return true;
}

template<class Client, class Wait>
bool unload(Client& client, bool stopped, Wait wait) {
    using namespace STEPROBOTSDK;
    if (!stopped || client.getMessageData().m_MessageType == eError) {
        error(client, "unload", "not_stopped_or_alarm", -1);
        return false;
    }
    const auto project = client.getProjectName(), program = client.getProgramName();
    if (!program.empty()) {
        const int rc = program == "SDK" && projectBase(project) == "SDK" ? client.SDKSimulateProgramKill(true) :
            client.ProgramKillCmd(projectBase(project), program, true);
        if (rc != 0 || !wait([&] { return client.getProgramName().empty(); })) {
            error(client, "unload", rc != 0 ? "request" : "feedback", rc);
            return false;
        }
    }
    std::cout << "unload_ok previous_project=" << project << " previous_program=" << program << '\n';
    return true;
}

template<class Client, class Wait>
bool load(Client& client, const std::string& project, const std::string& program, bool stopped, Wait wait) {
    if (!unload(client, stopped, wait)) return false;
    std::vector<int> errors;
    const int rc = client.ProgramLoadCmd(projectBase(project), program, errors, true);
    if (rc != 0 || !errors.empty()) {
        error(client, "load", "request", rc);
        std::cout << "loadproj_error rc=" << rc << " project=" << project << " program=" << program << " error_lines=";
        for (int line : errors) std::cout << line << ',';
        std::cout << '\n';
        return false;
    }
    if (!wait([&] { return client.getProgramName() == program &&
        projectBase(client.getProjectName()) == projectBase(project); })) {
        error(client, "load", "feedback", rc);
        return false;
    }
    std::cout << "loadproj_ok project=" << project << " program=" << program << '\n';
    return true;
}
}
