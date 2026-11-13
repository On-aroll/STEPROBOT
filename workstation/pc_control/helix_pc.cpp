#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include "RobotCom.hpp"
#include "workstation_motion.h"
#include "workstation_identity.h"
#include "workstation_controller_ops.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace STEPROBOTSDK;
using namespace std::chrono_literals;

struct Job {
    const char* project;
    const char* program;
};
constexpr Job Helix{"SR175_3200_Helix", "helix_25mms"};
constexpr Job Gcode{"GcodePreview", "gcode_path"};
constexpr Job Manual{nullptr, nullptr};
constexpr unsigned short VirtualPort = 30316;
constexpr unsigned short RealPort = 30312;

struct ManualProfile {
    double linear = 0;
    double returnLinear = 0, returnRadius = 0;
    double angular = 0;
    double joints[6]{};
    double hardwareJoints[6]{};
    double lower[6]{}, upper[6]{};
    bool valid = false;
};
static ManualProfile manualProfile;
static double appliedJogSpeed = 0;
static const char* appliedJogUnit = "none";
static bool returningHome = false, homePlanReady = false;
static bool panelMode = false;
static const char* homeReturnResult = "none";
static JointReturnPlan homePlan;
static unsigned long homePlanId = 0;
static std::chrono::steady_clock::time_point homePlanTime;
static std::string homePlanTool;
static double homePlanRadius = 0;
static const char* homePlanError = "none";
static bool switchedToManual = false;
static std::string sentProgramName;
static int sentLastMotionLine = 0;
static bool sentRunning = false;
static double sentTarget[3]{};
static bool sentLastMotionObserved = false;
static bool atRest(RobotComClient& client);
static bool autoMode(RobotComClient& client);

static ManualProfile readManualProfile(const char* filename) {
    ManualProfile profile;
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(filename, error);
    const auto native = path.wstring();
    const auto* configuredRoot = _wgetenv(L"STEP_WORKSTATION_RUN_ROOT");
    if (!configuredRoot || !*configuredRoot) return profile;
    const auto rootPath = std::filesystem::weakly_canonical(configuredRoot, error);
    if (error) return profile;
    const std::wstring root = rootPath.wstring() + L"\\";
    if (error || native.size() <= root.size() || _wcsnicmp(native.c_str(), root.c_str(), root.size()) != 0 ||
        path.filename() != "RobotParameter.xml" || std::filesystem::file_size(path, error) > 200000 || error) return profile;
    std::ifstream file(path);
    const std::string xml((std::istreambuf_iterator<char>(file)), {});
    const auto value = [&](const std::string& tag) {
        std::smatch match;
        if (!std::regex_search(xml, match, std::regex("<" + tag + ">([0-9.eE+-]+)</" + tag + ">"))) return 0.;
        try { return std::stod(match[1]); } catch (...) { return 0.; }
    };
    profile.linear = value("textBoxManual_Vpath");
    profile.returnLinear = std::min(profile.linear, value("textBoxReduceSpd_Cartpath"));
    if (xml.find("<textBoxrobotName>SR175-3200</textBoxrobotName>") != std::string::npos &&
        xml.find("<comboBoxSRCoupling>NO Axis</comboBoxSRCoupling>") != std::string::npos) {
        double radius = 0;
        for (const auto* tag : {"textBoxSRDistBase1_2X", "textBoxSRDistBase1_2Z", "textBoxSRDistBase2_3",
                                "textBoxSRDist3_4", "textBoxSRDistWrist1_2", "textBoxSRDistWrist2_TCP"}) {
            const auto length = value(tag);
            if (!std::isfinite(length) || length <= 0) { radius = 0; break; }
            radius += length;
        }
        if (radius > 0) profile.returnRadius = radius + std::abs(value("textBoxSRDistBase2_3Z")) +
            std::hypot(value("textBoxTDDist_Xpositon"), value("textBoxTDDist_Ypositon"), value("textBoxTDDist_Zpositon"));
    }
    profile.angular = value("textBoxManual_Vori");
    const auto percent = value("trackBarManualVelocity");
    profile.valid = std::isfinite(profile.linear) && profile.linear > 0 && profile.linear <= 250 &&
        value("textBoxReduceSpd_Cartpath") > 0 && value("textBoxReduceSpd_Cartpath") <= 250 &&
        std::isfinite(profile.angular) && profile.angular > 0 && profile.angular <= 180 && percent > 0 && percent <= 100;
    for (int i = 0; i < 6; ++i) {
        profile.hardwareJoints[i] = value("Joint" + std::to_string(i + 1) + "_Maxvelocity");
        profile.joints[i] = profile.hardwareJoints[i] * percent / 100;
        profile.lower[i] = value("Joint" + std::to_string(i + 1) + "_limitnegative");
        profile.upper[i] = value("Joint" + std::to_string(i + 1) + "_limitpositive");
        profile.valid = profile.valid && std::isfinite(profile.joints[i]) && profile.joints[i] > 0 && profile.joints[i] <= 360 &&
            std::isfinite(profile.lower[i]) && std::isfinite(profile.upper[i]) && profile.lower[i] < profile.upper[i];
    }
    return profile;
}

static bool verifiedVirtualListener(DWORD owner) {
    if (!owner) return false;
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (!size) return false;
    std::vector<unsigned char> storage(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(storage.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)) return false;
    bool command = false, feedback = false;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != owner) continue;
        const auto port = ntohs(static_cast<unsigned short>(row.dwLocalPort));
        command = command || port == VirtualPort;
        feedback = feedback || port == VirtualPort + 1;
    }
    if (!command || !feedback) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool found = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
    CloseHandle(process);
    const auto* configuredKernel = _wgetenv(L"STEP_VIRTUAL_KERNEL");
    if (!found || !configuredKernel || !*configuredKernel) return false;
    std::error_code error;
    const auto kernel = std::filesystem::weakly_canonical(configuredKernel, error);
    return !error && kernel.filename() == L"1-NonReal.exe" && kernel.parent_path().filename() == L"Environment" &&
        kernel.parent_path().parent_path().filename() == L"RobotController_PC" && std::filesystem::equivalent(path, kernel, error) && !error;
}

static bool isPrivateIPv4(const std::string& ip) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, ip.c_str(), &address) != 1) return false;
    const auto host = ntohl(address.S_un.S_addr);
    return (host >> 24) == 10 || (host >> 20) == 0xac1 || (host >> 16) == 0xc0a8;
}

static bool matchingProgram(RobotComClient& client, const Job& job) {
    if (!job.program) return false;
    const auto project = client.getProjectName();
    return (project == job.project || project == std::string(job.project) + ".sr") && client.getProgramName() == job.program;
}

static void printStatus(RobotComClient& client, const char* outcome = nullptr, bool busy = false) {
    if (client.ConnectStatus() != 0) {
        std::cout << "status=unknown reason=disconnected\n";
        return;
    }
    const auto pose = client.getCartPosWorld();
    const auto joints = client.getAxisPos();
    const auto msg = client.getMessageData();
    std::cout << "status=connected project=" << client.getProjectName() << " program=" << client.getProgramName()
              << " mode=" << int(client.getOperationMode()) << " enabled=" << client.getMotorEnableState()
              << " jogref=" << int(client.getJogRefSys())
              << " state=" << int(client.getProgramState()) << " line=" << client.getCurrentLine()
              << " command_busy=" << busy
              << " override=" << client.getOverride() << " alarm=" << msg.m_MessageID
              << " xyz=" << pose.cart[0] << ',' << pose.cart[1] << ',' << pose.cart[2]
              << " abc=" << pose.cart[3] << ',' << pose.cart[4] << ',' << pose.cart[5]
              << " speed=" << client.getActCartDynState().m_Vel;
    std::cout << " toolname=" << client.getToolName() << " home_returning=" << returningHome << " home_return_result=" << homeReturnResult
              << " motion_rest=" << atRest(client) << " home_plan_ready=" << homePlanReady << " home_plan_id=" << homePlanId
              << " home_plan_speed=" << homePlan.tcpSpeed << " home_plan_unit=mm_s home_plan_seconds=" << homePlan.seconds << " home_plan_waypoints=" << homePlan.waypoints.size()
              << " home_plan_error=" << homePlanError << " return_speed=" << appliedJogSpeed << " return_unit=" << appliedJogUnit;
    if (outcome) {
        std::cout << " outcome=" << outcome << " continuous=" << (client.getProgramMode() == eContinue)
                  << " error=" << (msg.m_MessageType == eError)
                  << " manual_linear=" << manualProfile.linear << " manual_return_linear=" << manualProfile.returnLinear << " manual_angular=" << manualProfile.angular
                  << " manual_profile=" << manualProfile.valid << " jogset=" << appliedJogSpeed << " jogunit=" << appliedJogUnit
                  << " refname=" << client.getTeachRefSysNam();
    }
    const auto base = client.getCartPosRobotBase();
    const auto user = client.getCartPosUserRef();
    std::cout << " base=";
    for (int i = 0; i < 6; ++i) std::cout << base.cart[i] << (i == 5 ? ' ' : ',');
    std::cout << "user=";
    for (int i = 0; i < 6; ++i) std::cout << user.cart[i] << (i == 5 ? ' ' : ',');
    std::cout << "manual_joints=";
    for (int i = 0; i < 6; ++i) std::cout << manualProfile.joints[i] << (i == 5 ? ' ' : ',');
    std::cout << " axes=";
    for (int i = 0; i < 6; ++i) std::cout << joints.m_Joint[i] << (i == 5 ? '\n' : ',');
}

static const char* startBlocker(RobotComClient& client, const Job& job, bool* completionArmed = nullptr) {
    const bool workflow = matchingProgram(client, job);
    if (!workflow && client.getProgramName().empty())
        return "no program is loaded; pick one with loadproj or on the pendant";
    if (workflow) {
        bool saved = false;
        AXISPOS home{};
        SDynamicPercent dynamic{};
        if (client.VariableBoolReadCmd(job.project, job.program, "homeSaved", saved) != 0 || !saved)
            return "no saved home; use the pendant capture_home procedure";
        if (client.VariableAxisposReadCmd(job.project, job.program, "pHome", home) != 0)
            return "could not read pHome";
        if (&job == &Helix && (client.VariableDynamicReadCmd(job.project, job.program, "dynPath", dynamic) != 0 ||
            !std::isfinite(dynamic.m_SegmentDynamic.m_Vel) || std::abs(dynamic.m_SegmentDynamic.m_Vel - 25.0) > 0.001))
            return "could not verify the 25 mm/s dynPath setting";
        const auto joints = client.getAxisPos();
        for (int i = 0; i < 6; ++i) {
            if (!std::isfinite(joints.m_Joint[i]) || !std::isfinite(home.m_Joint[i]) ||
                std::abs(joints.m_Joint[i] - home.m_Joint[i]) > 0.1)
                return "actual joints do not match pHome";
        }
        if (completionArmed) {
            bool alreadyDone = true;
            *completionArmed = client.VariableBoolReadCmd(job.project, job.program, "simDone", alreadyDone) == 0 && !alreadyDone;
        }
    }
    if (!autoMode(client) || client.getProgramMode() != eContinue || client.getMotorEnableState() != 1)
        return "local AUT, continuous mode and local servo enable are required";
    if (client.getProgramState() != eStop || client.getCurrentLine() < 1)
        return "program must be stopped; set the entry line before a new run";
    if (client.getMessageData().m_MessageType == eError) return "controller reports an error";
    if (client.ConnectStatus() != 0 || !autoMode(client) ||
        client.getProgramMode() != eContinue || client.getMotorEnableState() != 1 || client.getProgramState() != eStop ||
        client.getCurrentLine() < 1 || client.getMessageData().m_MessageType == eError)
        return "controller state changed during preflight";
    return nullptr;
}

// The panel owns a pipe and queues only STOP while START is being checked.
// Reading one command at a time keeps a queued STOP visible to PeekNamedPipe.
static bool readPanelCommand(std::string& command) {
    command.clear();
    char byte = 0;
    DWORD count = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &count, nullptr) && count == 1) {
        if (byte == '\n') return true;
        if (byte != '\r' && command.size() < 512) command.push_back(byte);
    }
    return false;
}

static bool panelStartAllowed() {
    char queued[8]{};
    DWORD count = 0;
    if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), queued, sizeof queued, &count, nullptr, nullptr)) return false;
    const auto line = [&](const char* word, size_t length) {
        return count >= length + 1 && std::memcmp(queued, word, length) == 0 &&
            (queued[length] == '\n' || queued[length] == '\r');
    };
    return !(line("stop", 4) || line("quit", 4) || line("estop", 5));
}

static bool atRest(RobotComClient& client);

static bool issue(RobotComClient& client, MODEKEY key, bool panel = false) {
    const bool starting = key == START;
    if (starting && panel && !panelStartAllowed()) {
        std::cout << "start_cancelled=stop_or_panel_closed\n";
        return false;
    }
    const int rc = client.SetModeCmd(key, false);
    std::cout << (starting ? "start" : "stop") << "_request_rc=" << rc << '\n';
    int stable = 0;
    for (int i = 0; rc == 0 && i < 60 && client.ConnectStatus() == 0; ++i) {
        if (starting && panel && !panelStartAllowed()) {
            std::cout << "start_interrupted=stop_or_panel_closed\n";
            return false;
        }
        const auto state = client.getProgramState();
        if (starting && state == eRun) {
            std::cout << "start_confirmed=running\n";
            return true;
        }
        if (!starting) {
            stable = atRest(client) ? stable + 1 : 0;
            if (stable >= 3) {
                std::cout << "stop_confirmed=" << (state == ePause ? "paused" : "stopped") << '\n';
                return true;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    std::cout << "result=unconfirmed check_local_pendant\n";
    return false;
}

template <typename Check>
static bool waitFor(RobotComClient& client, Check check) {
    for (int i = 0; i < 30 && client.ConnectStatus() == 0; ++i) {
        if (check()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

static bool isManual(RobotComClient& client) {
    const auto mode = client.getOperationMode();
    return mode == eManual || mode == eManualHigh;
}

static bool autoMode(RobotComClient& client) {
    const auto mode = client.getOperationMode();
    return mode == eAutomatic || mode == eAutomaticExternal;
}

// 返回起点在手动和自动模式下都可用：自动时先由上位机切到手动，完成后再切回。
static bool ensureManualMode(RobotComClient& client) {
    if (isManual(client)) return true;
    if (!autoMode(client) || !atRest(client) || client.getMessageData().m_MessageType == eError) return false;
    if (client.SetModeCmd(MANUAL, false) != 0 || !waitFor(client, [&] { return isManual(client); })) return false;
    switchedToManual = true;
    if (!client.getMotorEnableState()) {
        if (client.EnableMotorCmd() != 0 || !waitFor(client, [&] { return client.getMotorEnableState() == 1; })) return false;
        std::this_thread::sleep_for(350ms);
    }
    return client.getMotorEnableState() == 1;
}

static void restoreAutoMode(RobotComClient& client) {
    if (!switchedToManual) return;
    switchedToManual = false;
    if (!atRest(client) || client.getMessageData().m_MessageType == eError) {
        std::cout << "auto_mode_restore=skipped_not_safe mode=" << int(client.getOperationMode()) << '\n';
        return;
    }
    const bool restored = client.SetModeCmd(AUTO, false) == 0 && waitFor(client, [&] { return autoMode(client); });
    std::cout << "auto_mode_restored=" << (restored ? 1 : 0) << " mode=" << int(client.getOperationMode()) << '\n';
}

static bool atRest(RobotComClient& client) {
    const auto state = client.getProgramState();
    const auto* data = client.getSDKStruct();
    if (!data || (state != eStop && state != ePause) ||
        !(std::abs(client.getActCartDynState().m_Vel) < 0.01)) return false;
    for (int i = 0; i < 6; ++i) {
        if (!(std::abs(data->m_JointDynState[i].m_Vel) < 0.01)) return false;
    }
    return true;
}

static bool setOverride(RobotComClient& client, double value) {
    return (std::abs(client.getOverride() - value) < 0.01 || client.OverrideSetCmd(value) == 0) &&
        waitFor(client, [&] { return std::abs(client.getOverride() - value) < 0.01; });
}

static bool setVirtualMode(RobotComClient& client, const Job& job, bool manual) {
    if (!atRest(client)) return false;
    const auto inMode = [&] { return manual ? isManual(client) : autoMode(client); };
    if (!inMode() && (client.SetModeCmd(manual ? MANUAL : AUTO, false) || !waitFor(client, inMode))) return false;
    if (!setOverride(client, manual ? 5 : (&job == &Gcode ? 100 : 20))) return false;
    appliedJogSpeed = 0;
    if (!client.getMotorEnableState()) {
        if (client.EnableMotorCmd() || !waitFor(client, [&] { return client.getMotorEnableState() == 1; })) return false;
        std::this_thread::sleep_for(350ms);
    }
    return inMode() && client.getMotorEnableState() == 1;
}

static bool setFrame(RobotComClient& client, const char* name, REFSYS reference) {
    const auto confirmed = [&] { return client.getJogRefSys() == reference; };
    const auto rc = confirmed() ? 0 : client.JogRefSystemCmd(name);
    const bool ok = rc == 0 && waitFor(client, confirmed);
    if (!ok) std::cout << "jog_frame_unconfirmed name=" << name << " rc=" << rc << " expected=" << int(reference)
                       << " observed=" << int(client.getJogRefSys()) << " ref=" << client.getTeachRefSysNam() << '\n';
    if (ok) appliedJogSpeed = 0;
    return ok;
}

static bool loadNativeProgram(RobotComClient& client, const Job& job, const char* name) {
        std::vector<int> errors;
        const int rc = client.ProgramLoadCmd(job.project, name, errors, true);
        std::this_thread::sleep_for(500ms);
        if (rc || !errors.empty()) {
            std::cout << "load=" << name << " rc=" << rc << " error_lines=";
            for (int line : errors) std::cout << line << ',';
            std::cout << " message=" << client.getMessageData().m_MessageID << '\n';
        }
        return rc == 0 && errors.empty() && client.getProgramName() == name;
}

static bool captureAndLoad(RobotComClient& client, const Job& job) {
    const auto load = [&](const char* name) { return loadNativeProgram(client, job, name); };
    const auto initial = client.getAxisPos();
    if (!load("capture_home")) return false;
    const int startRc = client.SetModeCmd(START, false);
    if (startRc) { std::cout << "capture_fail: start rc=" << startRc << '\n'; return false; }
    const auto started = std::chrono::steady_clock::now();
    // START 返回 0 不代表已起跑：起跑竞态下状态仍是 eStop，只等 eStop 会立刻误判完成。
    // 必须等到 homeSaved 置位且程序回到停止态，才认为采集真正结束。
    bool saved = false;
    for (int i = 0; i < 160 && client.ConnectStatus() == 0; ++i) {
        client.VariableBoolReadCmd(job.project, "capture_home", "homeSaved", saved);
        if (saved && client.getProgramState() == eStop) break;
        std::this_thread::sleep_for(50ms);
    }
    const auto settle = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (settle > 1500) std::cout << "capture_slow: settle_ms=" << settle << '\n';
    if (!saved || client.getProgramState() != eStop) {
        std::cout << "capture_fail: capture_home 未完成 saved=" << saved << " state=" << client.getProgramState()
                  << " line=" << client.getCurrentLine() << " settle_ms=" << settle << '\n';
        return false;
    }
    AXISPOS home{};
    if (int rc = client.VariableAxisposReadCmd(job.project, "capture_home", "pHome", home); rc) {
        std::cout << "capture_fail: pHome rc=" << rc << '\n';
        return false;
    }
    const auto current = client.getAxisPos();
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(initial.m_Joint[i]) || !std::isfinite(home.m_Joint[i]) ||
            !std::isfinite(current.m_Joint[i]) || std::abs(home.m_Joint[i] - initial.m_Joint[i]) > 0.001 ||
            std::abs(current.m_Joint[i] - initial.m_Joint[i]) > 0.001) {
            std::cout << "capture_fail: joint_mismatch axis=" << i << " initial=" << initial.m_Joint[i]
                      << " home=" << home.m_Joint[i] << " current=" << current.m_Joint[i] << '\n';
            return false;
        }
    }
    if (int rc = client.ProgramKillCmd(job.project, "capture_home", true); rc) {
        std::cout << "capture_fail: kill rc=" << rc << '\n';
        return false;
    }
    std::this_thread::sleep_for(500ms);
    if (!load(job.program)) return false;
    if (const auto* blocker = startBlocker(client, job)) {
        std::cout << "Refused: " << blocker << '\n';
        return false;
    }
    return true;
}

static bool prepareVirtual(RobotComClient& client, const Job& job) {
    const auto programName = client.getProgramName();
    if (!programName.empty() || client.getProgramState() != eStop) {
        // 上次准备的 capture_home 可能残留运行态（起跑竞态下准备已返回失败）。沙箱内
        // 只会是我们自己的程序，杀掉后继续，而不是直接拒绝整个准备。
        std::cout << "prepare_recover: busy program=" << programName << " state=" << client.getProgramState() << '\n';
        client.ProgramKillCmd(job.project, programName.empty() ? "capture_home" : programName, true);
        std::this_thread::sleep_for(400ms);
        if (!client.getProgramName().empty() || client.getProgramState() != eStop) {
            std::cout << "prepare_fail: busy_after_kill program=" << client.getProgramName()
                      << " state=" << client.getProgramState() << '\n';
            return false;
        }
    }
    int rc = client.ProgramRunModeCmd(0);
    if (rc) { std::cout << "prepare_fail: runmode rc=" << rc << '\n'; return false; }
    rc = client.ToolSetCmd("Default");
    if (rc) { std::cout << "prepare_fail: toolset rc=" << rc << '\n'; return false; }
    if (!setVirtualMode(client, job, !job.program)) { std::cout << "prepare_fail: set_virtual_mode\n"; return false; }
    return job.program ? captureAndLoad(client, job) : setFrame(client, "JOINTS", eJoints);
}

static bool pulseJog(RobotComClient& client, REFSYS frame, int axis, bool positive, double requested = 0) {
    if (!isManual(client) || client.getJogRefSys() != frame || client.getMotorEnableState() != 1 ||
        !atRest(client) ||
        client.getMessageData().m_MessageType == eError) return false;
    homeReturnResult = "none";
    if (requested > 0) {
        if (!manualProfile.valid || !std::isfinite(requested)) return false;
        const bool linear = frame != eJoints && axis < 3;
        const double base = frame == eJoints ? manualProfile.joints[axis] : linear ? manualProfile.linear : manualProfile.angular;
        if (requested > base || (linear && requested > 250)) return false;
        const double raw = requested / base * 100;
        const double ratio = raw < 1 ? std::floor(raw * 10 + 1e-9) / 10 : std::floor(raw + 1e-9);
        if (ratio < .1 || !setOverride(client, ratio)) return false;
        appliedJogSpeed = base * client.getOverride() / 100;
        appliedJogUnit = linear ? "mm_s" : "deg_s";
        if (!(appliedJogSpeed > 0 && appliedJogSpeed <= requested + 1e-6)) return false;
    } else if (!(client.getOverride() > 0 && client.getOverride() <= 10)) return false;
    const int rc = client.SetModeCmd(static_cast<MODEKEY>((positive ? POSITIVE1 : NEGATIVE1) + axis), false);
    double peakTCP = 0, peakAxis = 0;
    bool safe = true;
    // In panel mode the jog keeps running until the GUI releases the button
    // (which queues "stop", visible to panelStartAllowed) or 10 s elapse.
    for (int i = 0; rc == 0; ++i) {
        std::this_thread::sleep_for(10ms);
        const auto* snapshot = client.getSDKStruct();
        const auto speed = std::abs(client.getActCartDynState().m_Vel);
        safe = snapshot && client.ConnectStatus() == 0 && std::isfinite(speed) && speed <= 250 && isManual(client);
        if (!safe) break;
        peakTCP = std::max(peakTCP, speed);
        peakAxis = std::max(peakAxis, std::abs(snapshot->m_JointDynState[axis].m_Vel));
        if (requested > 0 && i % 10 == 9) printStatus(client, "none", true);
        if (!panelStartAllowed()) break;
        if (!panelMode && i >= 30) break;
        if (panelMode && i >= 1000) { std::cout << "jog_hold_timeout\n"; break; }
    }
    const int stop = client.SetModeCmd(MSTOP, false);
    int stable = 0;
    const bool settled = waitFor(client, [&] {
        stable = atRest(client) ? stable + 1 : 0;
        return stable >= 3;
    });
    if (stop || !settled) {
        client.SetModeCmd(MSTOP, false);
        if (client.getMotorEnableState() == 1) client.EnableMotorCmd();
        return false;
    }
    std::cout << "jog_applied=" << appliedJogSpeed << " unit=" << appliedJogUnit << " peak_tcp_mm_s=" << peakTCP
              << " peak_axis_deg_s=" << peakAxis << '\n';
    return rc == 0 && safe && client.getMessageData().m_MessageType != eError;
}

static bool planReturnHome(RobotComClient& client, double speed, const JointPose& target) {
    homePlanError = "none";
    homePlanReady = false;
    if (!manualProfile.valid || !ensureManualMode(client) || !atRest(client) || client.getMotorEnableState() != 1 ||
        client.getMessageData().m_MessageType == eError || client.getToolName().empty()) {
        homePlanError = "manual_mode_or_feedback";
        std::cout << "home_plan_error=" << homePlanError << '\n';
        return false;
    }
    const auto actual = client.getAxisPos();
    JointPose start{};
    for (int i = 0; i < 6; ++i) start[i] = actual.m_Joint[i];
    Tool tool{};
    if (manualProfile.returnRadius <= 0 || client.VariableToolReadCmd(client.getToolName(), tool) != 0 ||
        !std::isfinite(tool.X) || !std::isfinite(tool.Y) || !std::isfinite(tool.Z)) {
        homePlanError = "return_geometry_or_tool_unavailable";
        std::cout << "home_plan_error=" << homePlanError << '\n';
        return false;
    }
    const JointPose manualSpeed{manualProfile.joints[0], manualProfile.joints[1], manualProfile.joints[2],
                               manualProfile.joints[3], manualProfile.joints[4], manualProfile.joints[5]};
    homePlanRadius = manualProfile.returnRadius + std::hypot(tool.X, tool.Y, tool.Z);
    const auto limited = limitReturnTcpSpeed(start, target, manualSpeed, homePlanRadius, speed, manualProfile.returnLinear);
    if (!limited.error.empty()) {
        homePlanError = "tcp_limit_or_override_resolution";
        std::cout << "home_plan_error=" << homePlanError << '\n';
        std::cerr << "Return plan refused: " << limited.error << '\n';
        return false;
    }
    homePlan = planJointReturn(start, target, JointPose{manualProfile.lower[0], manualProfile.lower[1], manualProfile.lower[2], manualProfile.lower[3], manualProfile.lower[4], manualProfile.lower[5]},
        JointPose{manualProfile.upper[0], manualProfile.upper[1], manualProfile.upper[2], manualProfile.upper[3], manualProfile.upper[4], manualProfile.upper[5]},
        manualSpeed, limited.jointSpeed);
    homePlan.tcpSpeed = speed;
    if (!homePlan.error.empty()) {
        homePlanError = "joint_limits_or_speed";
        std::cout << "home_plan_error=" << homePlanError << '\n';
        std::cerr << "Return plan refused: " << homePlan.error << '\n';
        homePlanReady = false;
        return false;
    }

    homePlanTool = client.getToolName();
    std::string points;
    std::vector<JointPose> previewPoints{homePlan.start};
    previewPoints.insert(previewPoints.end(), homePlan.waypoints.begin(), homePlan.waypoints.end());
    for (const auto& waypoint : previewPoints) {
        SDKInstructKinematics input{};
        for (int i = 0; i < 6; ++i) input.joint[i] = waypoint[i];
        std::memcpy(input.m_ToolName, homePlanTool.c_str(), std::min(homePlanTool.size(), sizeof input.m_ToolName - 1));
        std::memcpy(input.m_RefName, "WORLD", 6);
        KinematicsInfo output{};
        if (client.TransSDKGetKinematics(input, output) != 0) {
            homePlanError = "cartesian_preview_failed";
            std::cout << "home_plan_error=" << homePlanError << '\n';
            homePlanReady = false;
            return false;
        }
        const double tcp[]{output.xyzabc.m_X, output.xyzabc.m_Y, output.xyzabc.m_Z,
                           output.xyzabc.m_A, output.xyzabc.m_B, output.xyzabc.m_C};
        for (double value : tcp) if (!std::isfinite(value)) {
            homePlanError = "cartesian_preview_invalid";
            std::cout << "home_plan_error=" << homePlanError << '\n';
            homePlanReady = false;
            return false;
        }
        if (!points.empty()) points.push_back('~');
        for (double value : waypoint) points += std::to_string(value) + ',';
        for (int i = 0; i < 6; ++i) points += std::to_string(tcp[i]) + (i == 5 ? "" : ",");
    }
    ++homePlanId;
    homePlanReady = true;
    homePlanTime = std::chrono::steady_clock::now();
    homeReturnResult = "planned";
    std::cout << "home_plan=" << homePlanId << '|' << homePlan.tcpSpeed << '|' << homePlan.seconds << '|' << homePlanTool << '|' << points << '\n';
    return true;
}

static bool executeReturnHome(RobotComClient& client, unsigned long id) {
    if (!homePlanReady || id != homePlanId || std::chrono::steady_clock::now() - homePlanTime > 60s ||
        !ensureManualMode(client) || !atRest(client) || client.getMotorEnableState() != 1 || client.getMessageData().m_MessageType == eError ||
        client.getToolName() != homePlanTool) {
        std::cout << "home_return_error=precondition_changed mode=" << int(client.getOperationMode())
                  << " enabled=" << client.getMotorEnableState() << " alarm=" << client.getMessageData().m_MessageID
                  << " tool_match=" << (client.getToolName() == homePlanTool) << " plan_ready=" << homePlanReady
                  << " plan_id=" << homePlanId << " request_id=" << id << '\n';
        return false;
    }
    const auto initial = client.getAxisPos();
    Tool currentTool{};
    if (client.VariableToolReadCmd(homePlanTool, currentTool) != 0 ||
        !std::isfinite(currentTool.X) || !std::isfinite(currentTool.Y) || !std::isfinite(currentTool.Z) ||
        manualProfile.returnRadius + std::hypot(currentTool.X, currentTool.Y, currentTool.Z) > homePlanRadius + 1e-6) {
        std::cout << "home_return_error=tool_geometry_changed\n";
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(initial.m_Joint[i]) || std::abs(initial.m_Joint[i] - homePlan.start[i]) > .1) {
            std::cout << "home_return_error=start_changed axis=" << i + 1 << " actual=" << initial.m_Joint[i]
                      << " planned=" << homePlan.start[i] << '\n';
            return false;
        }
    }

    double maximum = 0;
    for (double value : manualProfile.joints) maximum = std::max(maximum, value);
    const double priorOverride = client.getOverride();
    const double ratio = homePlan.speed / maximum * 100;
    if (!(maximum > 0 && ratio >= .1 - 1e-9 && ratio <= 100 && setOverride(client, ratio)) ||
        !(client.getOverride() > 0 && client.getOverride() <= ratio + 1e-9)) {
        std::cout << "home_return_error=override_rejected requested=" << ratio << " observed=" << client.getOverride()
                  << " maximum_joint_speed=" << maximum << '\n';
        setOverride(client, priorOverride);
        return false;
    }

    homePlanReady = false;
    homeReturnResult = "running";
    returningHome = true;
    appliedJogSpeed = homePlan.tcpSpeed;
    appliedJogUnit = "mm_s";
    printStatus(client, "none", true);
    const auto overallDeadline = std::chrono::steady_clock::now() + 300s;
    bool arrived = homePlan.waypoints.empty(), safe = true, interrupted = false;
    for (const auto& waypoint : homePlan.waypoints) {
        if (!panelStartAllowed()) { interrupted = true; break; }
        const auto current = client.getAxisPos();
        double segmentDistance = 0;
        for (int i = 0; i < 6; ++i) segmentDistance = std::max(segmentDistance, std::abs(waypoint[i] - current.m_Joint[i]));
        const auto segmentDeadline = overallDeadline;
        JointsPos target{};
        for (int i = 0; i < 6; ++i) target[i] = waypoint[i];
        const int rc = client.JogToRobotAxisposCmd(ePTP, target, homePlanTool, "WORLD");
        if (rc != 0) {
            std::cout << "home_return_error=ptp_command_rejected rc=" << rc << '\n';
            safe = false;
            break;
        }
        int stable = 0;
        bool reached = false;
        while (std::chrono::steady_clock::now() < segmentDeadline && client.ConnectStatus() == 0) {
            if (!panelStartAllowed()) { interrupted = true; break; }
            const auto* snapshot = client.getSDKStruct();
            const auto actual = client.getAxisPos();
            bool atTarget = snapshot != nullptr;
            bool velocitySafe = snapshot != nullptr;
            for (int i = 0; i < 6 && snapshot; ++i) {
                velocitySafe = velocitySafe && std::isfinite(snapshot->m_JointDynState[i].m_Vel) &&
                    std::abs(snapshot->m_JointDynState[i].m_Vel) <= homePlan.speed + .1;
                atTarget = atTarget && std::isfinite(actual.m_Joint[i]) && std::abs(actual.m_Joint[i] - waypoint[i]) <= .02 &&
                    std::abs(snapshot->m_JointDynState[i].m_Vel) < .05;
            }
            safe = snapshot && isManual(client) && client.getOverride() > 0 && client.getOverride() <= ratio + 1e-9 && client.getMotorEnableState() == 1 &&
                client.getMessageData().m_MessageType != eError && velocitySafe &&
                std::isfinite(client.getActCartDynState().m_Vel) && std::abs(client.getActCartDynState().m_Vel) <= homePlan.tcpSpeed;
            if (!safe) {
                double peak = 0;
                for (int i = 0; i < 6 && snapshot; ++i) peak = std::max(peak, std::abs(snapshot->m_JointDynState[i].m_Vel));
                std::cout << "home_return_error=motion_guard mode=" << int(client.getOperationMode())
                          << " enabled=" << client.getMotorEnableState() << " alarm=" << client.getMessageData().m_MessageID
                          << " peak_joint_speed=" << peak << " joint_limit_deg_s=" << homePlan.speed << " requested_tcp_mm_s=" << homePlan.tcpSpeed
                          << " tcp_speed=" << client.getActCartDynState().m_Vel << '\n';
                break;
            }
            stable = atTarget ? stable + 1 : 0;
            if (stable >= 3) { reached = true; break; }
            printStatus(client, "none", true);
            std::this_thread::sleep_for(100ms);
        }
        if (!reached && !interrupted && safe) {
            const auto timedOut = client.getAxisPos();
            std::cout << "home_return_error=segment_timeout distance_deg=" << segmentDistance
                      << " override=" << client.getOverride() << " target=";
            for (int i = 0; i < 6; ++i) std::cout << waypoint[i] << (i == 5 ? " actual=" : ",");
            for (int i = 0; i < 6; ++i) std::cout << timedOut.m_Joint[i] << (i == 5 ? '\n' : ',');
        }
        if (interrupted) std::cout << "home_return_error=stop_requested_during_segment\n";
        const int exitRc = reached && safe && !interrupted ? client.JogToPositionExitCmd() : -1;
        if (reached && safe && !interrupted && exitRc != 0)
            std::cout << "home_return_error=ptp_exit_rejected rc=" << exitRc << '\n';
        if (!reached || !safe || interrupted || exitRc != 0) {
            safe = safe && reached;
            break;
        }
    }
    if (!homePlan.waypoints.empty() && safe && !interrupted) {
        const auto final = client.getAxisPos();
        arrived = true;
        for (int i = 0; i < 6; ++i) arrived = arrived && std::isfinite(final.m_Joint[i]) && std::abs(final.m_Joint[i] - homePlan.target[i]) <= .02;
    }
    if (!arrived) {
        client.JogToPositionExitCmd();
        client.SetModeCmd(MSTOP, false);
    }
    const bool settled = waitFor(client, [&] { return atRest(client); });
    const bool overrideRestored = settled && setOverride(client, priorOverride);
    if (!arrived && !interrupted && safe) std::cout << "home_return_error=target_not_confirmed\n";
    if (!settled) std::cout << "home_return_error=stop_not_confirmed\n";
    if (settled && !overrideRestored) std::cout << "home_return_error=override_restore_failed\n";
    returningHome = false;
    restoreAutoMode(client);
    homeReturnResult = arrived && safe && overrideRestored ? "arrived" : interrupted && settled ? "stopped" : "unconfirmed";
    appliedJogSpeed = 0;
    appliedJogUnit = "none";
    std::cout << "home_return_result=" << homeReturnResult << '\n';
    return arrived && safe && overrideRestored;
}

static bool cancelHomePlan(unsigned long id) {
    if (!homePlanReady || id != homePlanId) return false;
    homePlanReady = false;
    homeReturnResult = "cancelled";
    return true;
}

// G-code 转换产物的 SDK 指令流逐行下发：无需任何文件传输，实机与虚拟走同一份转换结果。
struct SdkLine {
    char type = 0;  // T tool, R refsys, D dyn, L lin, W wait, E eof
    int line = 0;
    std::string name;
    double x = 0, y = 0, z = 0;
    int dynIndex = 0;
    bool blend = false;
    double dyn[12]{};
    int waitMs = 0;
};

static bool sendSdkProgram(RobotComClient& client, const std::string& path, bool run) {
    if (!run) {
        std::cout << "sendprog_error=stream_requires_sendrun\n";
        return false;
    }
    std::ifstream file(std::filesystem::u8path(path));
    if (!file) { std::cout << "sendprog_error=file_unreadable\n"; return false; }
    std::vector<SdkLine> program;
    int dynSeen = 0, previousLine = 0;
    std::string currentTool = "Default", currentRef = "WORLD", text;
    while (std::getline(file, text)) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (text.empty() || text[0] == '#' || text == "HOME") continue;
        std::istringstream input(text);
        std::string kind, trailing;
        SdkLine entry;
        // 新速度组的 DYN 与它生效的首条 LIN 同行号（GUI 生成约定），其余行号必须严格递增。
        if (!(input >> kind >> entry.line) || entry.line < previousLine || entry.line > 100000 ||
            program.size() >= 1250 ||
            (entry.line == previousLine && kind != "DYN" && (program.empty() || program.back().type != 'D'))) {
            std::cout << "sendprog_error=bad_stream line=" << text.substr(0, 40) << '\n';
            return false;
        }
        previousLine = entry.line;
        entry.type = kind[0];
        if (kind == "TOOL" || kind == "REFSYS") {
            if (!(input >> entry.name) || !std::regex_match(entry.name, std::regex("[A-Za-z][A-Za-z0-9_]{0,30}"))) {
                std::cout << "sendprog_error=bad_name\n";
                return false;
            }
            if (kind == "TOOL") currentTool = entry.name; else currentRef = entry.name;
        } else if (kind == "DYN") {
            for (double& value : entry.dyn)
                if (!(input >> value) || !std::isfinite(value)) { std::cout << "sendprog_error=bad_dyn\n"; return false; }
            if (!(entry.dyn[0] > 0 && entry.dyn[0] <= 25)) { std::cout << "sendprog_error=speed_out_of_range speed=" << entry.dyn[0] << '\n'; return false; }
        } else if (kind == "LIN") {
            if (!(input >> entry.x >> entry.y >> entry.z >> entry.dynIndex >> entry.blend) ||
                !std::isfinite(entry.x) || !std::isfinite(entry.y) || !std::isfinite(entry.z) ||
                std::abs(entry.x) > 1000 || std::abs(entry.y) > 1000 || std::abs(entry.z) > 1000 ||
                entry.dynIndex < 1 || entry.dynIndex > dynSeen) {
                std::cout << "sendprog_error=bad_lin\n";
                return false;
            }
            entry.name = currentTool + '|' + currentRef;
        } else if (kind == "WAIT") {
            if (!(input >> entry.waitMs) || entry.waitMs < 0 || entry.waitMs > 3600000) {
                std::cout << "sendprog_error=bad_wait\n";
                return false;
            }
        } else if (kind != "EOF") {
            std::cout << "sendprog_error=unknown_kind kind=" << kind << '\n';
            return false;
        }
        if (kind == "DYN") ++dynSeen;
        program.push_back(entry);
    }
    if (program.empty() || program.back().type != 'E') {
        std::cout << "sendprog_error=no_eof\n";
        return false;
    }
    if (!atRest(client) || client.getProgramState() != eStop || client.getMessageData().m_MessageType == eError) {
        std::cout << "sendprog_error=controller_not_ready state=" << int(client.getProgramState()) << '\n';
        return false;
    }
    if (client.getToolName() != "Default") {
        std::cout << "sendprog_error=default_tool_required\n";
        return false;
    }
    if (panelMode && !panelStartAllowed()) return false;
    const auto loaded = client.getProgramName();
    if (!loaded.empty()) {
        const auto project = client.getProjectName();
        const int unload = loaded == "SDK" && project == "SDK.sr" ? client.SDKSimulateProgramKill(true) :
            client.ProgramKillCmd(project, loaded, true);
        if (unload != 0 || !waitFor(client, [&] { return client.getProgramName().empty() && atRest(client); })) {
            std::cout << "sendprog_error=unload rc=" << unload << " program=" << client.getProgramName() << '\n';
            return false;
        }
    }
    // Prepare the stream in the mode that will execute it.
    if (!autoMode(client) && (!run || client.SetModeCmd(AUTO, false) != 0 ||
        !waitFor(client, [&] { return autoMode(client); }))) {
        std::cout << "sendprog_error=automatic_mode_required\n";
        return false;
    }
    if (client.getProgramMode() != eContinue && client.ProgramRunModeCmd(0) != 0) return false;
    const auto home = client.getCartPosWorld();
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(home.cart[i])) {
            std::cout << "sendprog_error=home_unavailable\n";
            return false;
        }
    }
    auto fail = [&](const char* stage, int rc, int line) {
        const auto message = client.getMessageData();
        std::cout << "sendprog_error=" << stage << " rc=" << rc << " line=" << line
                  << " msgid=" << message.m_MessageID << " msg=";
        for (int i = 0; i < 96 && message.m_MessageString[i]; ++i) std::cout << message.m_MessageString[i];
        std::cout << '\n';
        client.SetModeCmd(STOP, false);
        client.SetModeCmd(MSTOP, false);
        client.SDKSimulateProgramKill(true);
        sentRunning = false;
        return false;
    };
    sentRunning = false;
    int lastMotionLine = 0;
    int sent = 0, batches = 0, instructions = 0;
    const auto flush = [&] {
        const int count = client.NumOfPro;
        if (count == 0) return true;
        const int result = client.SDKSimulateProgramSend(sent);
        if (result != 0) {
            std::cout << "sendprog_batch_error rc=" << result << " ack_line=" << sent << " count=" << count << '\n';
            std::this_thread::sleep_for(100ms);
            return false;
        }
        ++batches;
        instructions += count;
        printStatus(client, "none", true);
        return true;
    };
    if (panelMode && !panelStartAllowed()) return false;
    int rc = client.SDKSimulateProgramLoad(true);
    if (rc != 0) return fail("load", rc, 0);
    if (!waitFor(client, [&] { return client.getProgramName() == "SDK"; })) return fail("load_feedback", -1, 0);
    sentProgramName = client.getProgramName();
    sentLastMotionObserved = false;
    for (const auto& entry : program) {
        if (entry.type == 'L') lastMotionLine = entry.line;
        if (entry.type == 'L') {
            sentTarget[0] = home.cart[0] + entry.x;
            sentTarget[1] = home.cart[1] + entry.y;
            sentTarget[2] = home.cart[2] + entry.z;
        }
    }
    sentLastMotionLine = lastMotionLine;
    if (panelMode && !panelStartAllowed()) return fail("cancelled", 0, 0);
    if (client.getMotorEnableState() != 1 &&
        (client.EnableMotorCmd() != 0 || !waitFor(client, [&] { return client.getMotorEnableState() == 1; })))
        return fail("servo_enable", -1, 0);
    std::ostringstream prepared;
    prepared << std::setprecision(15) << "sendprog_prepared observed=" << sentProgramName
             << " origin=" << home.cart[0] << ',' << home.cart[1] << ',' << home.cart[2] << '\n';
    std::cout << prepared.str();
    // SDK accepts streamed statements only while its empty SDK task is running.
    if (!issue(client, START, panelMode)) return fail("start_unconfirmed", -1, 0);
    sentRunning = true;
    printStatus(client, "none", true);
    for (const auto& entry : program) {
        if (panelMode && !panelStartAllowed()) return fail("cancelled", 0, entry.line);
        if (client.NumOfPro >= MAX_PRO_NUM && !flush()) return fail("send_batch", -1, entry.line);
        if (entry.type == 'L') lastMotionLine = entry.line;
        if (entry.type == 'T') {
            rc = client.SDKSimulateSetToolCmd(entry.name, entry.line);
        } else if (entry.type == 'R') {
            rc = client.SDKSimulateSetRefSysCmd(entry.name, entry.line);
        } else if (entry.type == 'D') {
            SDynamicPercent dyn{};
            const auto fill = [&](SubDynamic& group, const double* values) {
                group.m_Vel = values[0]; group.m_Acc = values[1]; group.m_Dec = values[2];
                group.m_Jerk = values[3]; group.m_Tjolt = values[4];
            };
            fill(dyn.m_SegmentDynamic, entry.dyn);
            fill(dyn.m_OriDynamic, entry.dyn + 5);
            // GUI 流只带关节组的速度/加速度（12 值），SDK 结构要求 15 值齐全；
            // 控制器把 0 值校验为“减速度/加加速度小于等于 0”直接拒绝，缺省三值
            // 用路径组同值补齐。SRP 语义：Dyn 为后续运动语句的动态默认值。
            dyn.m_JointPercent.m_Vel = entry.dyn[10];
            dyn.m_JointPercent.m_Acc = entry.dyn[11];
            dyn.m_JointPercent.m_Dec = entry.dyn[2];
            dyn.m_JointPercent.m_Jerk = entry.dyn[3];
            dyn.m_JointPercent.m_Tjolt = entry.dyn[4];
            rc = client.SDKSimulateDynProCmd(dyn, entry.line);
        } else if (entry.type == 'L') {
            const auto separator = entry.name.find('|');
            HMIPos target{};
            target.m_PosType = eCartPos;
            target.m_Pos.m_postyp = ePosCart;
            target.m_Pos.m_CartPos.m_X = home.cart[0] + entry.x;
            target.m_Pos.m_CartPos.m_Y = home.cart[1] + entry.y;
            target.m_Pos.m_CartPos.m_Z = home.cart[2] + entry.z;
            target.m_Pos.m_CartPos.m_A = home.cart[3];
            target.m_Pos.m_CartPos.m_B = home.cart[4];
            target.m_Pos.m_CartPos.m_C = home.cart[5];
            target.m_Pos.m_CartPos.m_Mode = home.m_Mode;
            SDKOverlap ovl{};
            ovl.m_OveTyp = eOVLREL;
            ovl.m_Percent = entry.blend ? 50 : 0;
            rc = client.SDKSimulateLinProCmd(target, -1, ovl, eOriNULL, entry.name.substr(0, separator),
                                             entry.name.substr(separator + 1), entry.line);
        } else if (entry.type == 'W') {
            rc = client.SDKSimulateWaitTimeProCmd(entry.waitMs, entry.line);
        } else if (entry.type == 'E') {
            rc = client.SDKSimulateEOFCmd(entry.line);
        }
        if (rc != 0) {
            std::cout << "sendprog_instruction=" << entry.type << " mode=" << int(client.getOperationMode())
                      << " buffered=" << client.NumOfPro << '\n';
            return fail("instruction", rc, entry.line);
        }
    }
    if (panelMode && !panelStartAllowed()) return fail("cancelled", 0, 0);
    if (!flush()) return fail("send", -1, 0);
    std::this_thread::sleep_for(400ms);
    const auto observed = client.getProgramName();
    sentProgramName = observed;
    sentLastMotionLine = lastMotionLine;
    std::cout << "sendprog_ok lines=" << sent << " observed=" << observed << " last_motion_line=" << lastMotionLine
              << " batches=" << batches << " instructions=" << instructions << '\n';
    return true;
}

static bool remoteCommand(RobotComClient& client, const Job& job, const std::string& command) {
    const auto wait = [&](auto check) { return waitFor(client, check); };
    if (command == "manual" || command == "auto")
        return StepControl::changeMode(client, command == "manual", atRest(client), wait);
    if (command == "enable") {
        if (!atRest(client) || client.getMessageData().m_MessageType == eError) {
            StepControl::error(client, "enable", "not_stopped_or_alarm", -1);
            return false;
        }
        const int rc = client.getMotorEnableState() == 1 ? 0 : client.EnableMotorCmd();
        if (rc != 0 || !wait([&] { return client.getMotorEnableState() == 1; })) {
            StepControl::error(client, "enable", rc != 0 ? "request" : "feedback", rc);
            return false;
        }
        std::cout << "enable_ok enabled=1\n";
        return true;
    }
    if (command == "unload") {
        const bool ok = StepControl::unload(client, atRest(client), wait);
        if (ok) { sentRunning = false; sentProgramName.clear(); }
        return ok;
    }
    if (command.rfind("planhome_tcp ", 0) == 0) {
        std::istringstream input(command.substr(13));
        double speed = 0;
        JointPose target{};
        std::string trailing;
        if (!(input >> speed)) return false;
        for (double& value : target) if (!(input >> value)) return false;
        return !(input >> trailing) && planReturnHome(client, speed, target);
    }
    if (command.rfind("returnhome ", 0) == 0) {
        std::istringstream input(command.substr(11));
        unsigned long id = 0;
        std::string trailing;
        return input >> id && !(input >> trailing) && executeReturnHome(client, id);
    }
    if (command.rfind("cancelhome ", 0) == 0) {
        std::istringstream input(command.substr(11));
        unsigned long id = 0;
        std::string trailing;
        const bool cancelled = input >> id && !(input >> trailing) && cancelHomePlan(id);
        if (cancelled) restoreAutoMode(client);
        return cancelled;
    }
    if (command.rfind("loadproj ", 0) == 0 || command.rfind("runproj ", 0) == 0) {
        const bool run = command[0] == 'r';
        std::istringstream input(command.substr(run ? 8 : 9));
        std::string project, program, trailing;
        const std::regex name("[A-Za-z0-9_.-]{1,64}");
        if (!(input >> project >> program) || (input >> trailing) ||
            !std::regex_match(project, name) || !std::regex_match(program, name)) return false;
        if (!atRest(client) || client.getMessageData().m_MessageType == eError) return false;
        if (run && (!autoMode(client) &&
                    (client.SetModeCmd(AUTO, false) != 0 || !waitFor(client, [&] { return autoMode(client); })))) return false;
        if (run && client.getProgramMode() != eContinue && client.ProgramRunModeCmd(0) != 0) return false;
        if (panelMode && !panelStartAllowed()) {
            std::cout << "loadproj_error rc=-1 stage=cancelled\n";
            return false;
        }
        // A controller only allows one loaded foreground program. Loading does not start/enable it.
        const auto loadWait = [&](auto check) {
            return waitFor(client, [&] { return (!panelMode || panelStartAllowed()) && check(); });
        };
        if (!StepControl::load(client, project, program, atRest(client), loadWait)) return false;
        sentRunning = false;
        sentProgramName.clear();
        if (!run) return true;
        if (client.getMotorEnableState() != 1 &&
            (client.EnableMotorCmd() != 0 || !waitFor(client, [&] { return client.getMotorEnableState() == 1; }))) return false;
        return issue(client, START, panelMode);
    }
    if (command == "capture") {
        return matchingProgram(client, job) && client.getOperationMode() == eAutomatic && atRest(client) &&
            client.getMotorEnableState() == 1 && client.getMessageData().m_MessageType != eError &&
            client.ProgramKillCmd(job.project, job.program, true) == 0 &&
            waitFor(client, [&] { return client.getProgramName().empty(); }) && captureAndLoad(client, job);
    }
    if (command.rfind("sendprog ", 0) == 0 || command.rfind("sendrun ", 0) == 0) {
        const bool run = command.rfind("sendrun ", 0) == 0;
        std::string trailing;
        std::istringstream input(command.substr(run ? 8 : 9));
        std::string path;
        if (!(input >> std::quoted(path)) || (input >> trailing) || path.size() > 300) return false;
        return sendSdkProgram(client, path, run);
    }
    if (!isManual(client) || !atRest(client)) return false;
    if (command == "joints") return setFrame(client, "JOINTS", eJoints);
    if (command == "world") return setFrame(client, "WORLD", eWorld);
    if (command == "base") {
        const auto rc = client.ReferenceSystemSetCmd("ROBOTBASE");
        if (rc != 0 || !waitFor(client, [&] { return client.getTeachRefSysNam() == "ROBOTBASE"; })) {
            std::cout << "base_reference_unconfirmed rc=" << rc << " observed=" << client.getTeachRefSysNam() << '\n';
            return false;
        }
        return setFrame(client, "UserRef", eCustom);
    }
    if (command == "tool") return setFrame(client, "TOOL", eTool);
    if (command == "user") return setFrame(client, "UserRef", eCustom);
    if (command.rfind("user ", 0) == 0) {
        const auto name = command.substr(5);
        CartRefSys reference{};
        if (!std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_]{0,26}")) ||
            client.VariableRefReadCmd(name, reference) != 0 || client.ReferenceSystemSetCmd(name) != 0 ||
            !waitFor(client, [&] { return client.getTeachRefSysNam() == name; })) return false;
        return setFrame(client, "UserRef", eCustom);
    }
    if (command == "speed1" || command == "speed5" || command == "speed10")
        return setOverride(client, std::stoi(command.substr(5)));
    if (command.size() == 8 && command.compare(0, 4, "jog ") == 0 &&
        (command[4] == 'J' || command[4] == 'W') && command[5] == ' ' &&
        command[6] >= '1' && command[6] <= '6' && (command[7] == '+' || command[7] == '-')) {
        return pulseJog(client, command[4] == 'J' ? eJoints : eWorld, command[6] - '1', command[7] == '+');
    }
    std::istringstream input(command);
    std::string action, frame, direction, trailing;
    double speed = 0;
    if (input >> action >> frame >> direction >> speed && !(input >> trailing) && action == "jog" &&
        direction.size() == 2 && direction[0] >= '1' && direction[0] <= '6' && (direction[1] == '+' || direction[1] == '-') &&
        std::isfinite(speed) && speed > 0) {
        if (frame == "B" && client.getTeachRefSysNam() != "ROBOTBASE") return false;
        const REFSYS reference = frame == "J" ? eJoints : frame == "W" ? eWorld : frame == "B" ? eCustom : frame == "T" ? eTool : frame == "U" ? eCustom : eGo;
        return reference != eGo && pulseJog(client, reference, direction[0] - '1', direction[1] == '+', speed);
    }
    return false;
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    const bool panel = argc > 2 && std::string(argv[argc - 1]) == "--panel";
    if (panel) { --argc; panelMode = true; }
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
        std::cout << "helix_pc virtual VERIFIED_KERNEL_PID [RobotParameter.xml]\n"
                     "helix_pc real CONTROLLER_IPV4 [RobotParameter.xml]\n"
                     "Commands: status, start, stop, estop, quit, manual, auto, enable, unload, jog, frames, planhome_tcp, returnhome, cancelhome, loadproj, runproj, capture, sendprog, sendrun.\n";
        return 0;
    }
    const std::string mode = argv[1];
    const bool gcode = mode == "virtual-gcode" || mode == "virtual-prepare-gcode";
    const bool manual = mode == "virtual-manual" || mode == "virtual-prepare-manual";
    const bool preparing = mode == "virtual-prepare" || mode == "virtual-prepare-gcode" || mode == "virtual-prepare-manual";
    const bool simulated = mode == "virtual" || gcode || manual || preparing;
    const auto& job = manual ? Manual : (gcode ? Gcode : Helix);
    if ((simulated && argc != 3 && argc != 4) || (mode == "real" && argc != 3 && argc != 4) || (mode != "real" && !simulated)) return 2;
    std::string ip = "127.0.0.1";
    if (simulated) {
        const std::string id = argv[2];
        if (id.empty() || id.size() > 9 || id.find_first_not_of("0123456789") != std::string::npos ||
            !verifiedVirtualListener(static_cast<DWORD>(std::stoul(id)))) {
            std::cerr << "Refused: verified local virtual controller is absent.\n";
            return 3;
        }
    } else {
        ip = argv[2];
        if (!isPrivateIPv4(ip)) { std::cerr << "Refused: use the confirmed private IPv4 of the cabinet.\n"; return 3; }
    }
    if (argc == 4) manualProfile = readManualProfile(argv[3]);
    RobotComClient client;
    client.setInterfaceWaitTime(1500);
    const int rc = client.init(ip.c_str(), simulated ? VirtualPort : RealPort);
    if (rc != 0) { std::cerr << "SDK init failed: " << rc << '\n'; client.close(); return 4; }
    std::this_thread::sleep_for(1s);
    RobotInfoStruct robot{};
    ElecBoxInfoStruct cabinet{};
    if (client.RequestRobotInfoCmd(robot) != 0 || client.RequestElecBoxInfoCmd(cabinet) != 0) {
        std::cerr << "Refused: controller identity could not be read.\n";
        client.close(); return 4;
    }
    const std::string robotName(robot.m_Name, strnlen_s(robot.m_Name, sizeof robot.m_Name));
    const std::string cabinetName(cabinet.m_ElecBoxName, strnlen_s(cabinet.m_ElecBoxName, sizeof cabinet.m_ElecBoxName));
    const std::string serial(cabinet.m_ElecBoxSNo, strnlen_s(cabinet.m_ElecBoxSNo, sizeof cabinet.m_ElecBoxSNo));
    const bool control = isSupportedStepRobot(robotName) && (simulated ? cabinetName == "SRC2.7" : true);
    if (preparing) {
        const bool ready = control && prepareVirtual(client, job);
        if (!ready && !control)
            std::cout << "prepare_fail: identity robot=" << robotName << " cabinet=" << cabinetName << '\n';
        std::cout << "virtual_prepared=" << (ready ? 1 : 0) << '\n';
        if (ready) printStatus(client);
        // SDK 析构/关闭在后台线程收尾时可能堆损坏并把退出状态变成崩溃，准备结果已输出，直接硬退出。
        std::_Exit(ready ? 0 : 5);
    }
    std::cout << "robot=" << robotName << " cabinet=" << cabinetName << " serial=" << serial
              << " target=" << mode << " ip=" << ip << " access=" << (control ? "start_stop" : "read_only") << '\n';
    if (!control) std::cout << "control_disabled=model_or_virtual_identity_mismatch expected_robot=SR175-3200_or_SR175/3200\n";
    const char* outcome = "none";
    bool observeEnd = false;
    bool completionArmed = false;
    const auto report = [&] {
        const bool sentMatch = !sentProgramName.empty() && client.getProgramName() == sentProgramName;
        if (sentRunning && sentMatch && client.getProgramState() == eRun && client.getCurrentLine() >= sentLastMotionLine)
            sentLastMotionObserved = true;
        if ((observeEnd || sentRunning) && client.ConnectStatus() == 0 && atRest(client) &&
            (matchingProgram(client, job) || sentMatch)) {
            observeEnd = false;
            sentRunning = false;
            bool completed = false;
            if (completionArmed && client.getProgramState() == eStop &&
                client.VariableBoolReadCmd(job.project, job.program, "simDone", completed) == 0 && completed &&
                client.ConnectStatus() == 0 && matchingProgram(client, job) && atRest(client) && client.getProgramState() == eStop) {
                outcome = "completed";
            } else if (sentMatch && sentLastMotionObserved && client.getProgramState() == eStop &&
                       (client.getCurrentLine() == -1 || client.getCurrentLine() >= sentLastMotionLine)) {
                const auto actual = client.getCartPosWorld();
                const double error = std::hypot(actual.cart[0] - sentTarget[0], actual.cart[1] - sentTarget[1], actual.cart[2] - sentTarget[2]);
                outcome = std::isfinite(error) && error <= .1 ? "completed" : "ended_unverified";
            } else {
                outcome = "ended_unverified";
            }
        }
        printStatus(client, panel ? outcome : nullptr);
    };
    report();
    std::string command;
    const auto readCommand = [&] {
        if (panel) return readPanelCommand(command);
        std::cout << "helix> ";
        return static_cast<bool>(std::getline(std::cin, command));
    };
    while (readCommand()) {
        if (command == "status") {
            report();
            continue;
        } else if (command == "quit") {
            if (control && client.ConnectStatus() == 0 &&
                (client.getProgramState() == eRun || client.getProgramState() == eReturn)) {
                std::cout << "Refused: program is moving or returning; stop locally or use stop first.\n";
                continue;
            }
            break;
        } else if (!control || client.ConnectStatus() != 0) {
            std::cout << "Refused: control disabled or connection lost.\n";
            if (!panel) continue;
        } else if (command == "start") {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            completionArmed = false;
            const char* blocker = nullptr;
            if (!panel || panelStartAllowed()) {
                blocker = startBlocker(client, job, panel && simulated ? &completionArmed : nullptr);
            }
            if (panel && !panelStartAllowed()) {
                std::cout << "start_cancelled=stop_or_panel_closed\n";
            } else if (panel && std::chrono::steady_clock::now() >= deadline) {
                std::cout << "start_cancelled=preflight_timeout\n";
            } else if (blocker) {
                std::cout << "Refused: " << blocker << '\n';
            } else {
                outcome = "none";
                observeEnd = issue(client, START, panel) && panel;
            }
        } else if (command == "stop") {
            if (isManual(client)) {
                client.SetModeCmd(MSTOP, false);
                if (waitFor(client, [&] { return atRest(client); })) {
                    std::cout << "stop_confirmed=stopped\n";
                    if (returningHome || std::strcmp(homeReturnResult, "arrived") != 0) homeReturnResult = "stopped";
                    outcome = "stopped";
                    restoreAutoMode(client);
                } else { std::cout << "result=unconfirmed check_local_pendant\n"; outcome = "ended_unverified"; }
            } else if (!client.getProgramName().empty()) {
                observeEnd = false;
                sentRunning = false;
                outcome = issue(client, STOP) ? "stopped" : "ended_unverified";
            } else {
                std::cout << "Refused: no program is loaded; use the local pendant.\n";
            }
        } else if (command == "estop") {
            const int programStop = client.SetModeCmd(STOP, false);
            const int jogStop = client.SetModeCmd(MSTOP, false);
            int stable = 0;
            const bool settled = waitFor(client, [&] {
                stable = atRest(client) ? stable + 1 : 0;
                return stable >= 3;
            });
            observeEnd = false;
            sentRunning = false;
            outcome = settled ? "stopped" : "ended_unverified";
            if (returningHome || std::strcmp(homeReturnResult, "arrived") != 0) homeReturnResult = "stopped";
            std::cout << "estop_rc=" << programStop << ',' << jogStop
                      << " estop_confirmed=" << (settled ? "stopped" : "unconfirmed")
                      << " alarm=" << client.getMessageData().m_MessageID << '\n';
            if (panel) std::cout << "command_done=estop\n";
        } else {
            const bool ok = remoteCommand(client, job, command);
            if (ok && (command == "unload" || command.rfind("loadproj ", 0) == 0)) {
                outcome = "none";
                observeEnd = completionArmed = false;
            }
            if (ok && command == "capture") {
                outcome = "none";
                observeEnd = false;
            }
            if (ok && command.rfind("sendrun ", 0) == 0) outcome = "none";
            std::cout << "virtual_command_result=" << (ok ? "ok" : command.rfind("returnhome ", 0) == 0 && std::strcmp(homeReturnResult, "stopped") == 0 ? "cancelled" : "refused") << '\n';
        }
        report();
        if (panel && command == "stop") std::cout << "command_done=stop\n";
    }
    std::cout << "Closing SDK session. Use the local pendant to verify robot state.\n";
    client.close();
    return 0;
}
