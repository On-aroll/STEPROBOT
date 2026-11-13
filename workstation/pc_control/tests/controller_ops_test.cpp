#define NOMINMAX
#include <winsock2.h>
#include "RobotCom.hpp"
#include "../workstation_controller_ops.h"

#include <sstream>

using namespace STEPROBOTSDK;

struct Controller {
    OPERATIONMODE mode = eAutomaticExternal;
    PROGRAMSTATE state = eStop;
    int enabled = 0, modeRc = 0, unloadRc = 0, loadRc = 0;
    bool modeFeedback = true, unloadFeedback = true, loadFeedback = true, syntaxError = false;
    std::string project = "Existing.sr", program = "old_program";
    MessageData message{};
    std::vector<std::string> calls;

    Controller() { message.m_MessageType = static_cast<MESSAGETYPE>(int(eError) == 0 ? 1 : 0); }
    auto getOperationMode() { return mode; }
    auto getProgramState() { return state; }
    auto getMessageData() { return message; }
    auto getMotorEnableState() { return enabled; }
    auto getProjectName() { return project; }
    auto getProgramName() { return program; }
    int SetModeCmd(MODEKEY key, bool) {
        calls.push_back("mode");
        if (modeRc == 0 && modeFeedback) mode = key == MANUAL ? eManual : eAutomatic;
        return modeRc;
    }
    int EnableMotorCmd() { calls.push_back("enable"); enabled = 1; return 0; }
    int OverrideSetCmd(double) { calls.push_back("override"); return 0; }
    int ProgramKillCmd(const std::string& targetProject, const std::string& targetProgram, bool) {
        calls.push_back("unload:" + targetProject + '/' + targetProgram);
        if (unloadRc == 0 && unloadFeedback) { project.clear(); program.clear(); }
        return unloadRc;
    }
    int SDKSimulateProgramKill(bool) {
        calls.push_back("unload_sdk");
        if (unloadRc == 0 && unloadFeedback) { project.clear(); program.clear(); }
        return unloadRc;
    }
    int ProgramLoadCmd(const std::string& targetProject, const std::string& targetProgram, std::vector<int>& errors, bool) {
        calls.push_back("load:" + targetProject + '/' + targetProgram);
        if (syntaxError) errors.push_back(7);
        if (loadRc == 0 && loadFeedback) { project = targetProject + ".sr"; program = targetProgram; }
        return loadRc;
    }
};

int main() {
    int failures = 0, checks = 0;
    const auto check = [&](bool result, const char* reason) {
        ++checks;
        if (!result) { ++failures; std::cerr << "FAIL: " << reason << '\n'; }
    };
    const auto wait = [](auto ready) { return ready(); };
    std::ostringstream output;
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    {
        Controller c;
        check(StepControl::changeMode(c, true, true, wait), "loaded automatic program prevented manual mode");
        check(c.mode == eManual && c.program == "old_program" && c.project == "Existing.sr", "mode switch changed loaded program");
        check(c.enabled == 0 && c.calls == std::vector<std::string>{"mode"}, "mode switch requested enable, override or program mutation");
        c.calls.clear();
        check(StepControl::changeMode(c, true, true, wait) && c.calls.empty(), "already-manual mode produced a write");
        check(StepControl::changeMode(c, false, true, wait) && c.enabled == 0 && c.program == "old_program", "automatic mode was coupled to enable/unload");
    }
    for (const bool rejected : {true, false}) {
        Controller c;
        c.modeRc = rejected ? -9 : 0;
        c.modeFeedback = false;
        check(!StepControl::changeMode(c, true, true, wait), "reported successful mode change without actual feedback");
        check(c.enabled == 0 && c.program == "old_program" && c.calls.size() == 1, "failed mode switch had unrelated side effects");
    }
    {
        Controller c;
        check(!StepControl::changeMode(c, true, false, wait) && c.calls.empty(), "sent mode change while moving");
        c.message.m_MessageType = eError;
        check(!StepControl::changeMode(c, true, true, wait) && c.calls.empty(), "sent mode change with controller alarm");
    }
    {
        Controller c;
        check(StepControl::load(c, "NewJob.sr", "new_program", true, wait), "could not replace loaded program");
        check(c.calls == std::vector<std::string>{"unload:Existing/old_program", "load:NewJob/new_program"}, "load order or SDK project suffix normalization was wrong");
        check(c.mode == eAutomaticExternal && c.enabled == 0, "loading changed mode or enabled robot");
    }
    {
        Controller c;
        c.project = "SDK.sr"; c.program = "SDK";
        check(StepControl::unload(c, true, wait) && c.calls == std::vector<std::string>{"unload_sdk"}, "SDK task used wrong unload API");
        c.calls.clear();
        check(StepControl::unload(c, true, wait) && c.calls.empty(), "empty controller generated an unload request");
    }
    for (const bool rejected : {true, false}) {
        Controller c;
        c.unloadRc = rejected ? -3 : 0; c.unloadFeedback = false;
        check(!StepControl::load(c, "NewJob", "new_program", true, wait), "loaded new program without confirmed unload");
        check(c.calls == std::vector<std::string>{"unload:Existing/old_program"}, "load reached controller after unload failure");
    }
    {
        Controller c;
        check(!StepControl::load(c, "NewJob", "new_program", false, wait) && c.calls.empty(), "unloaded program while moving");
        c.loadFeedback = false;
        check(!StepControl::load(c, "NewJob", "new_program", true, wait), "accepted missing project/program load feedback");
    }
    {
        Controller c;
        c.syntaxError = true;
        check(!StepControl::load(c, "NewJob", "new_program", true, wait), "accepted syntax errors despite zero SDK return code");
    }
    const auto diagnostics = output.str();
    check(diagnostics.find("operation=mode stage=request rc=-9 target=manual mode=3 enabled=0") != std::string::npos,
        "mode request failure omitted SDK return/state details");
    check(diagnostics.find("operation=mode stage=feedback rc=0") != std::string::npos, "missing mode feedback timeout diagnostic");
    check(diagnostics.find("error_lines=7,") != std::string::npos, "missing controller syntax error line");
    std::cout.rdbuf(previousOutput);
    std::cout << "controller_ops_test: checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
