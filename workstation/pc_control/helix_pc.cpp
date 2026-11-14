#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include "RobotCom.hpp"
#include "workstation_identity.h"
#include "workstation_motion.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
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
static const char* homeReturnResult = "none";
static JointReturnPlan homePlan;
static unsigned long homePlanId = 0;
static std::chrono::steady_clock::time_point homePlanTime;
static std::string homePlanTool;
static double homePlanRadius = 0;
static const char* homePlanError = "none";
static bool atRest(RobotComClient& client);

// 工作区根优先取 UI 传入的环境变量 STEPROBOT_WORKSPACE；无该变量时回退老布局
// （exe 固定位于 <workspace>/RobotPrograms/<机型>/pc_control/，向上五级）。
static const std::wstring& workspaceRoot() {
    static const std::wstring root = [] {
        wchar_t buffer[32768]{};
        std::filesystem::path directory;
        const DWORD env = GetEnvironmentVariableW(L"STEPROBOT_WORKSPACE", buffer, 32768);
        if (env > 0 && env < 32768) {
            directory = std::wstring(buffer, env);
        } else {
            const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
            if (!length || length >= 32768) return std::wstring{};
            directory = std::filesystem::path(buffer);
            for (int depth = 0; depth < 5 && !directory.empty(); ++depth) directory = directory.parent_path();
        }
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(directory, error);
        return error || directory.empty() ? std::wstring{} : canonical.wstring() + L'\\';
    }();
    return root;
}

static ManualProfile readManualProfile(const std::filesystem::path& raw) {
    ManualProfile profile;
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(raw, error);
    const auto native = path.wstring();
    const auto& base = workspaceRoot();
    const std::wstring root = base.empty() ? base : base + L"tmp\\pendant_ui_runs\\";
    if (root.empty() || error || native.size() <= root.size() || _wcsnicmp(native.c_str(), root.c_str(), root.size()) != 0 ||
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

// 示教器参数文件缺失时的内置限速：点动速度取官方手册表 2-3 中 SR175-3200
// 关节额定速度的 10% 作为保守值，限位用官方关节行程表；
// 直线 100mm/s、回起点 50mm/s、臂展 3200mm，均在 T2 点动 250mm/s 上限之内。
static ManualProfile defaultManualProfile() {
    ManualProfile profile;
    const double hardware[] = {110, 100, 100, 170, 160, 220};
    const double lower[] = {-183, -60, -130, -220, -125, -360};
    const double upper[] = {183, 85, 80, 220, 125, 360};
    for (int i = 0; i < 6; ++i) {
        profile.hardwareJoints[i] = hardware[i];
        profile.joints[i] = hardware[i] / 10;
        profile.lower[i] = lower[i];
        profile.upper[i] = upper[i];
    }
    profile.linear = 100;
    profile.angular = 30;
    profile.returnLinear = 50;
    profile.returnRadius = 3200;
    profile.valid = true;
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
    const auto& base = workspaceRoot();
    const std::wstring kernel = base.empty() ? std::wstring{}
        : base + L"STEPStudio 4.7.0\\STEPStudio 4.7.0\\RobotController_PC\\Environment\\1-NonReal.exe";
    return found && !kernel.empty() && _wcsicmp(path, kernel.c_str()) == 0;
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

// 实机启动前柜内程序与作业不一致时：卸载柜内当前载入的程序，再加载作业目标程序。
// 工程与程序须已存在于柜内（SDK 不提供文件上传）；SDK 文档要求卸载/加载均在程序停止、
// 无运动状态下进行；命令返回 0 仅表示"执行中"，须轮询 getProgramName 确认结果。
static bool ensureProgramLoaded(RobotComClient& client, const Job& job) {
    if (client.ConnectStatus() != 0 || client.getProgramState() != eStop) return false;
    std::string loaded = client.getProjectName();
    if (loaded.size() > 3 && loaded.compare(loaded.size() - 3, 3, ".sr") == 0) loaded.resize(loaded.size() - 3);
    const auto current = client.getProgramName();
    if (!current.empty()) {
        if (client.ProgramKillCmd(loaded, current) != 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline && client.getProgramName() == current)
            std::this_thread::sleep_for(50ms);
        if (client.getProgramName() == current) return false;
    }
    std::vector<int> loadErrors;
    if (client.ProgramLoadCmd(job.project, job.program, loadErrors) != 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (matchingProgram(client, job)) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
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

static const char* startBlocker(RobotComClient& client, bool simulated, const Job& job, bool* completionArmed = nullptr) {
    if (!matchingProgram(client, job)) return "the expected project and program must be loaded";
    if (client.getOperationMode() != eAutomatic || client.getProgramMode() != eContinue || client.getMotorEnableState() != 1)
        return "local AUT, continuous mode and local servo enable are required";
    if (client.getProgramState() != eStop || client.getCurrentLine() < 1 || client.getCurrentLine() > 2)
        return "program must be stopped at its entry; reload on the pendant before a new run";
    if (!simulated && !(client.getOverride() > 0))
        return "physical start requires a positive override";
    if (client.getMessageData().m_MessageType == eError) return "controller reports an error";
    bool saved = false;
    AXISPOS home{};
    SDynamicPercent dynamic{};
    if (client.VariableBoolReadCmd(job.project, job.program, "homeSaved", saved) != 0 || !saved)
        return "no saved home; move to the start pose and press record home on the workstation";
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
    if (client.ConnectStatus() != 0 || !matchingProgram(client, job) || client.getOperationMode() != eAutomatic ||
        client.getProgramMode() != eContinue || client.getMotorEnableState() != 1 || client.getProgramState() != eStop ||
        client.getCurrentLine() < 1 || client.getCurrentLine() > 2 || client.getMessageData().m_MessageType == eError ||
        (!simulated && !(client.getOverride() > 0)))
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
    char queued[6]{};
    DWORD count = 0;
    return PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), queued, sizeof queued, &count, nullptr, nullptr) &&
        !(count >= 5 && (std::memcmp(queued, "stop", 4) == 0 || std::memcmp(queued, "quit", 4) == 0) &&
          (queued[4] == '\n' || queued[4] == '\r'));
}

static bool atRest(RobotComClient& client);

static bool issue(RobotComClient& client, MODEKEY key, bool panel = false) {
    const int rc = client.SetModeCmd(key, false);
    const bool starting = key == START;
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
    const auto inMode = [&] { return manual ? isManual(client) : client.getOperationMode() == eAutomatic; };
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
    if (!load("capture_home") || client.SetModeCmd(START, false)) return false;
    std::this_thread::sleep_for(1000ms);
    bool saved = false;
    AXISPOS home{};
    if (client.getProgramState() != eStop ||
        client.VariableBoolReadCmd(job.project, "capture_home", "homeSaved", saved) || !saved ||
        client.VariableAxisposReadCmd(job.project, "capture_home", "pHome", home)) return false;
    const auto current = client.getAxisPos();
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(initial.m_Joint[i]) || !std::isfinite(home.m_Joint[i]) ||
            !std::isfinite(current.m_Joint[i]) || std::abs(home.m_Joint[i] - initial.m_Joint[i]) > 0.001 ||
            std::abs(current.m_Joint[i] - initial.m_Joint[i]) > 0.001) return false;
    }
    if (client.ProgramKillCmd(job.project, "capture_home", true)) return false;
    std::this_thread::sleep_for(500ms);
    if (!load(job.program)) return false;
    if (const auto* blocker = startBlocker(client, true, job)) {
        std::cout << "Refused: " << blocker << '\n';
        return false;
    }
    return true;
}

static bool prepareVirtual(RobotComClient& client, const Job& job) {
    if (!client.getProgramName().empty() || client.getProgramState() != eStop ||
        client.ProgramRunModeCmd(0) || client.ToolSetCmd("Default")) return false;
    if (!setVirtualMode(client, job, !job.program)) return false;
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
    for (int i = 0; rc == 0 && i < 30; ++i) {
        std::this_thread::sleep_for(10ms);
        const auto* snapshot = client.getSDKStruct();
        const auto speed = std::abs(client.getActCartDynState().m_Vel);
        safe = snapshot && client.ConnectStatus() == 0 && std::isfinite(speed) && speed <= 250 && isManual(client);
        if (!safe) break;
        peakTCP = std::max(peakTCP, speed);
        peakAxis = std::max(peakAxis, std::abs(snapshot->m_JointDynState[axis].m_Vel));
        if (requested > 0 && i % 10 == 9) printStatus(client, "none", true);
        if (!panelStartAllowed()) break;
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
    if (!manualProfile.valid || !isManual(client) || !atRest(client) || client.getMotorEnableState() != 1 ||
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
        !isManual(client) || !atRest(client) || client.getMotorEnableState() != 1 || client.getMessageData().m_MessageType == eError ||
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

static bool virtualCommand(RobotComClient& client, const Job& job, const std::string& command) {
    if (command == "manual" || command == "auto") return setVirtualMode(client, job, command == "manual");
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
        return input >> id && !(input >> trailing) && cancelHomePlan(id);
    }
    if (command == "capture") {
        return matchingProgram(client, job) && client.getOperationMode() == eAutomatic && atRest(client) &&
            client.getMotorEnableState() == 1 && client.getMessageData().m_MessageType != eError &&
            client.ProgramKillCmd(job.project, job.program, true) == 0 &&
            waitFor(client, [&] { return client.getProgramName().empty(); }) && captureAndLoad(client, job);
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
    if (panel) --argc;
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
        std::cout << "helix_pc virtual VERIFIED_KERNEL_PID\n"
                     "helix_pc real CONTROLLER_IPV4\n"
                     "Connecting to SR175-3200 grants full start/stop, manual/auto, jog and home control.\n"
                     "Commands: status, start, stop, quit; manual/auto, jog, speed, planhome/returnhome/cancelhome,\n"
                     "loadprogram, recordhome A1..A6 (real robot only)\n"
                     "need tmp/pendant_ui_runs/RobotParameter.xml. No automatic start or resume.\n";
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
        if (argc == 4) manualProfile = readManualProfile(std::filesystem::path(argv[3]));
    } else {
        ip = argv[2];
        if (!isPrivateIPv4(ip)) { std::cerr << "Refused: use the confirmed private IPv4 of the cabinet.\n"; return 3; }
        // 实机手动点动/回起点的速度限幅优先取示教器参数文件(工作区固定位置)；
        // 缺失时落到内置官方默认限速，不阻塞实机操作。
        manualProfile = readManualProfile(workspaceRoot() + L"tmp\\pendant_ui_runs\\RobotParameter.xml");
    }
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
    // 控制权只看机型白名单：实机固件回读 "SR175/3200"（斜杠），仿真用 "SR175-3200"（连字符），
    // 两种写法都在白名单内，避免同一机型因写法差异被判只读。
    const bool control = isSupportedStepRobot(robotName);
    if (preparing) {
        const bool ready = control && prepareVirtual(client, job);
        std::cout << "virtual_prepared=" << (ready ? 1 : 0) << '\n';
        if (ready) printStatus(client);
        client.close();
        return ready ? 0 : 5;
    }
    std::cout << "robot=" << robotName << " cabinet=" << cabinetName << " serial=" << serial
              << " target=" << mode << " ip=" << ip << " access=" << (control ? "start_stop" : "read_only") << '\n';
    const bool profileFromFile = manualProfile.valid;
    if (!profileFromFile) manualProfile = defaultManualProfile();
    std::cout << "manual_profile_file=" << (profileFromFile ? "loaded" : "built_in_default") << '\n';
    const char* outcome = "none";
    bool observeEnd = false;
    bool completionArmed = false;
    const auto report = [&] {
        if (observeEnd && client.ConnectStatus() == 0 && matchingProgram(client, job) && atRest(client)) {
            observeEnd = false;
            bool completed = false;
            if (completionArmed && client.getProgramState() == eStop &&
                client.VariableBoolReadCmd(job.project, job.program, "simDone", completed) == 0 && completed &&
                client.ConnectStatus() == 0 && matchingProgram(client, job) && atRest(client) && client.getProgramState() == eStop) {
                outcome = "completed";
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
                if (!simulated && job.program && !matchingProgram(client, job)) {
                    std::cout << "auto_load=loading project=" << job.project << " program=" << job.program << '\n';
                    if (!ensureProgramLoaded(client, job))
                        blocker = "cabinet does not contain the expected project; store it once via USB or STEPStudio, then retry";
                    else
                        std::cout << "auto_load=loaded\n";
                }
                if (!blocker) blocker = startBlocker(client, simulated, job, panel && simulated ? &completionArmed : nullptr);
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
                } else { std::cout << "result=unconfirmed check_local_pendant\n"; outcome = "ended_unverified"; }
            } else if (matchingProgram(client, job)) {
                observeEnd = false;
                outcome = issue(client, STOP) ? "stopped" : "ended_unverified";
            } else {
                std::cout << "Refused: expected program is not loaded; use the local pendant.\n";
            }
        } else if (!simulated && command == "capture") {
            // capture 会清掉柜内程序再上传作业，只允许在本地仿真做；实机的程序切换走 loadprogram 或 start 时的自动加载。
            std::cout << "Refused: program upload runs in the local simulator only; start auto-loads the stored program.\n";
        } else if (!simulated && command == "loadprogram") {
            if (matchingProgram(client, job)) {
                std::cout << "load_program=already\n";
            } else if (client.getProgramState() != eStop || !atRest(client)) {
                std::cout << "Refused: cabinet program must be stopped before switching; stop it on the pendant first.\n";
            } else {
                std::cout << "auto_load=loading project=" << job.project << " program=" << job.program << '\n';
                if (ensureProgramLoaded(client, job)) std::cout << "load_program=ok\n";
                else std::cout << "Refused: cabinet does not contain the expected project; store it once via USB or STEPStudio, then retry.\n";
            }
        } else if (!simulated && command.rfind("recordhome ", 0) == 0) {
            // 实机起点闭环：把界面记录的起点写进柜内程序变量（先 pHome 后 homeSaved，
            // 失败时 homeSaved 保持 FALSE，柜内不会出现"声称有起点但没写"的中间态）。
            std::istringstream input(command.substr(11));
            JointPose target{};
            std::string trailing;
            bool parsed = true;
            for (double& value : target) if (!(input >> value)) parsed = false;
            if (input >> trailing) parsed = false;
            bool finite = parsed;
            for (double value : target) finite = finite && std::isfinite(value);
            if (!finite) {
                std::cout << "Refused: recordhome requires six finite joint values.\n";
            } else if (!matchingProgram(client, job)) {
                std::cout << "Refused: expected program is not loaded; press load program first.\n";
            } else if (client.getProgramState() != eStop || !atRest(client)) {
                std::cout << "Refused: robot must be fully stopped before recording the program home.\n";
            } else {
                AXISPOS home{};
                for (int i = 0; i < 6; ++i) home.m_Joint[i] = target[i];
                const int posRc = client.VariableAxisposModifyCmd(job.project, job.program, "pHome", home);
                const int flagRc = posRc == 0 ? client.VariableBoolModifyCmd(job.project, job.program, "homeSaved", true) : -1;
                if (posRc == 0 && flagRc == 0) std::cout << "program_home_recorded=1\n";
                else std::cout << "Refused: writing program home failed rc=" << (posRc != 0 ? posRc : flagRc)
                    << "; ensure the expected program is loaded and stopped.\n";
            }
        } else {
            const bool ok = virtualCommand(client, job, command);
            if (ok && command == "capture") {
                outcome = "none";
                observeEnd = false;
            }
            std::cout << "virtual_command_result=" << (ok ? "ok" : command.rfind("returnhome ", 0) == 0 && std::strcmp(homeReturnResult, "stopped") == 0 ? "cancelled" : "refused") << '\n';
        }
        report();
        if (panel && command == "stop") std::cout << "command_done=stop\n";
    }
    std::cout << "Closing SDK session. Use the local pendant to verify robot state.\n";
    client.close();
    return 0;
}
