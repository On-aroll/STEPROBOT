// ============================================================================
//  STEP (新时达) 机器人 C++ SDK 上手骨架 —— 上位机运动控制研究用
//
//  设计原则：默认"零运动"。
//    mode=probe   只读链路/状态/位置，机械臂不会动（第一次务必先跑这个）
//    mode=run     加载示教器上已有的程序并启动（需人工二次确认，先低速）
//    mode=jog     绝对位置点动到给定关节角（需人工三次确认）
//
//  接口签名全部核对自 SR.SDK_C++_X64_Win_V2.4.1.20250801/RobotCom.hpp
//  命名空间：STEPROBOTSDK
//
//  编译：Visual Studio 2022 + "使用 C++ 的桌面开发"，x64 / Release
//        链接 Robot-SDK.lib（Release）或 Robot-SDKd.lib（Debug）
//
//  字符集注意：本文件带 UTF-8 BOM；SDK 头文件是 GBK/ANSI，
//              不要给 SDK 头文件加 /utf-8 源字符集，也不要把它另存为 UTF-8。
//              为彻底避免控制台乱码，下面所有输出字符串都用 ASCII。
// ============================================================================

#include "RobotCom.hpp"
#include "write_path.h"   // 内部已 include ControlMsgData.hpp / EasyTcpClient.hpp

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using STEPROBOTSDK::RobotComClient;
using STEPROBOTSDK::MODEKEY;
using STEPROBOTSDK::PROGRAMSTATE;
using STEPROBOTSDK::AXISPOS;
using STEPROBOTSDK::RobotCartPos;

// ----------------------------------------------------------------------------
// 配置区：按现场实际情况改
// ----------------------------------------------------------------------------
static const char*    ROBOT_IP     = "127.0.0.1";  // 下载地址.txt / SDK 说明书 2.1.3
static const u_short  ROBOT_PORT   = 30312;             // SDK 固定端口，不可更改
static const int      CLIENT_LIMIT = 1;                 // 同一时刻只允许 1 个客户端连 30312

// 示教器里必须已经存在这些名字，否则加载/点动会失败
static const std::string PROJECT_NAME = "project1.sr";  // 工程名（.sr 目录）
static const std::string PROGRAM_NAME = "program0";     // 程序名
static const std::string TOOL_NAME    = "Default";      // 工具名
static const std::string REF_NAME     = "WORLD";        // 参考坐标系名

static const double SAFE_OVERRIDE_FIRST_RUN = 50.0;   // 虚拟控制器上 50% 安全     // 第一次跑给 10%，确认没问题再提速
static const int    POLL_TIMES   = 10;                  // 说明书范式：最多 10 次
static const int    POLL_INTERVAL_MS = 50;              // 每次 50ms

// ----------------------------------------------------------------------------
// 小工具
// ----------------------------------------------------------------------------
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static const char* ProgStateName(PROGRAMSTATE s) {
    switch (s) {
        case PROGRAMSTATE::eRun:    return "RUN";
        case PROGRAMSTATE::ePause:  return "PAUSE";
        case PROGRAMSTATE::eStop:   return "STOP";
        default:                    return "RETURN/OTHER";
    }
}

static void DumpRcResult(const char* tag, int ret) {
    // 说明书 2.x 的通用约定：0 正常，-1 执行失败，-2 数据收发异常，-3 通讯断开
    std::printf("[%-18s] ret=%d", tag, ret);
    if (ret == 0)        std::printf("   (OK / 注意: 对某些命令 0 只表示\"执行中\", 需回读状态确认)\n");
    else if (ret == -1)  std::printf("   (EXEC FAILED: 参数错误或状态不允许)\n");
    else if (ret == -2)  std::printf("   (SEND/RECV ERROR)\n");
    else if (ret == -3)  std::printf("   (COMM DISCONNECTED)\n");
    else                 std::printf("   (SEE SDK DOC)\n");
}

static void DumpJoints(const char* tag, const double j[6]) {
    std::printf("%s J1..J6 = %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f\n",
                tag, j[0], j[1], j[2], j[3], j[4], j[5]);
}

static void DumpCart(const char* tag, const double c[6]) {
    std::printf("%s X  Y  Z  A  B  C  = %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f (mm/deg)\n",
                tag, c[0], c[1], c[2], c[3], c[4], c[5]);
}

// 轮询等待某个条件成立（说明书示例统一用这个 10x50ms 范式）
template <typename Cond>
static bool WaitFor(Cond cond, const char* what) {
    for (int i = 0; i < POLL_TIMES; ++i) {
        if (cond()) { std::printf("  [wait] %s ok (after %d ms)\n", what, (i + 1) * POLL_INTERVAL_MS); return true; }
        sleep_ms(POLL_INTERVAL_MS);
    }
    std::printf("  [wait] %s TIMEOUT after %d ms\n", what, POLL_TIMES * POLL_INTERVAL_MS);
    return false;
}

static bool Confirm(const char* question) {
    std::printf("\n>>> %s\n    输入大写 YES 并回车继续，其它任意输入取消: ", question);
    std::fflush(stdout);
    char buf[16] = {0};
    if (!std::fgets(buf, sizeof(buf), stdin)) return false;
    buf[strcspn(buf, "\r\n")] = 0;
    bool ok = (std::strcmp(buf, "YES") == 0);
    if (!ok) std::printf("    -> cancelled.\n");
    return ok;
}

static void ShowLastError(RobotComClient& rc) {
    STEPROBOTSDK::MessageData m = rc.getMessageData();
    std::printf("    last message: id=%d src=%s msg=", m.m_MessageID, m.m_MessageSource);
    std::printf("%s\n", m.m_MessageString);   // 若这里乱码: 中文提示经 GBK 传输, 见 README "乱码" 一节
}

// ----------------------------------------------------------------------------
// 1) probe —— 只读，安全，第一个要跑通的东西
// ----------------------------------------------------------------------------
static int Probe(RobotComClient& rc) {
    int r = rc.init(ROBOT_IP, ROBOT_PORT);
    DumpRcResult("init", r);
    if (r != 0) {
        std::printf("  连接失败排查顺序:\n"
                    "    1) PC 与控制器是否同网段 (PC 设 192.168.39.x / 255.255.255.0)\n"
                    "    2) PowerShell: Test-NetConnection 192.168.39.220 -Port 30312\n"
                    "    3) 是否已有别的客户端占用 30312 (只能有 1 个; 关掉另一个程序重试)\n"
                    "    4) 控制器软件版本需 >= V1.8.0.20240531_0712\n");
        return 1;
    }

    std::printf("\n== SDK / 控制器标识 ==\n");
    std::printf("  SDKVersion       : %s\n", rc.SDKVersion().c_str());
    std::printf("  ConnectStatus    : %d\n", rc.ConnectStatus());
    std::printf("  project on teach : %s\n", rc.getProjectName().c_str());
    std::printf("  program on teach : %s\n", rc.getProgramName().c_str());
    std::printf("  tool             : %s\n", rc.getToolName().c_str());
    std::printf("  teach refsys     : %s\n", rc.getTeachRefSysNam().c_str());

    std::printf("\n== 运行状态 ==\n");
    std::printf("  motorEnableState : %d  (0=OFF 1=ON)\n", rc.getMotorEnableState());
    std::printf("  operationMode    : %d  (0=eManual 1=eManualHigh 2=eAutomatic 3=eAutomaticExternal)\n",
                (int)rc.getOperationMode());
    std::printf("  programState     : %s\n", ProgStateName(rc.getProgramState()));
    std::printf("  override         : %.2f %%\n", rc.getOverride());
    std::printf("  currentLine      : %d\n", rc.getCurrentLine());
    std::printf("  runTime(lifeTime): %u / %d\n", rc.getRobotRunningTime(), rc.getLifeTime());

    std::printf("\n== 位置（只读，不会动） ==\n");
    AXISPOS ap = rc.getAxisPos();
    DumpJoints("  AXISPOS ", ap.m_Joint);
    RobotCartPos cp = rc.getCartPosWorld();
    DumpCart("  CART(world) ", cp.cart);
    std::printf("  pose mode        : %u\n", cp.m_Mode);

    std::printf("\n  (本模式不发送任何运动/使能命令)\n");
    return 0;
}

// ----------------------------------------------------------------------------
// 2) run —— 加载示教器上已有的程序并启动（研究"上位机调度已有轨迹"时用）
//
//  调用顺序严格按 SDK 说明书 2.1.5 / 2.3.3 / 2.8.1 的示例序列：
//    Enable -> Load(轮询) -> SetMode(START) -> 轮询 eRun -> 观察行号 -> STOP -> Kill
//  两条硬约束（原文）：使能与其他操作间隔 >= 300ms；加载前当前必须无程序加载。
// ----------------------------------------------------------------------------
static int RunExistingProgram(RobotComClient& rc) {
    if (!Confirm("即将加载并启动示教器上的程序，机械臂会运动。现场已清场、急停可及？")) return 2;

    DumpRcResult("init", rc.init(ROBOT_IP, ROBOT_PORT));

    // 2-1 先压低倍率，第一次别按 100% 跑
    DumpRcResult("OverrideSetCmd", rc.OverrideSetCmd(SAFE_OVERRIDE_FIRST_RUN));

    // 2-2 上使能（上下使能同一函数，按当前状态翻转 -> 所以先看一眼当前状态）
    if (rc.getMotorEnableState() == 0) {
        DumpRcResult("EnableMotorCmd", rc.EnableMotorCmd());
        WaitFor([&] { return rc.getMotorEnableState() == 1; }, "motor enabled");
        sleep_ms(300);                                   // 说明书: 使能后与下一步间隔 300ms
    } else {
        std::printf("  motor already enabled, skip EnableMotorCmd\n");
    }

    // 2-3 加载程序
    std::vector<int> errLines;
    DumpRcResult("ProgramLoadCmd", rc.ProgramLoadCmd(PROJECT_NAME, PROGRAM_NAME, errLines));
    if (!errLines.empty()) {
        std::printf("    error lines:");
        for (int e : errLines) std::printf(" %d", e);     // 行号为 -1 表示错误过多, 需回示教器查看
        std::printf("\n");
        ShowLastError(rc);
        return 1;
    }
    WaitFor([&] { return rc.getProgramName() == PROGRAM_NAME; }, "program loaded");

    // 2-4 启动
    DumpRcResult("SetModeCmd(START)", rc.SetModeCmd(MODEKEY::START));
    WaitFor([&] { return rc.getProgramState() == PROGRAMSTATE::eRun; }, "program running");

    // 2-5 观察跑（这里换成你的采样/记录逻辑：行号、位置、IO、时间戳）
    for (int i = 0; i < 20 && rc.getProgramState() == PROGRAMSTATE::eRun; ++i) {
        AXISPOS a = rc.getAxisPos();
        std::printf("  t=%02d line=%4d ", i, rc.getCurrentLine());
        std::printf("J1=%8.3f J2=%8.3f J3=%8.3f\n", a.m_Joint[0], a.m_Joint[1], a.m_Joint[2]);
        sleep_ms(100);
    }

    // 2-6 收尾：STOP -> 轮询 eStop -> Kill（Kill 的返回值 0 才表示真卸完了）
    DumpRcResult("SetModeCmd(STOP)", rc.SetModeCmd(MODEKEY::STOP));
    WaitFor([&] { return rc.getProgramState() == PROGRAMSTATE::eStop; }, "program stopped");
    DumpRcResult("ProgramKillCmd", rc.ProgramKillCmd(PROJECT_NAME, PROGRAM_NAME));
    DumpRcResult("OverrideSetCmd(100)", rc.OverrideSetCmd(100.0));   // 别忘了恢复倍率
    return 0;
}

// ----------------------------------------------------------------------------
// 3) jog —— 绝对位置点动（说明书: 点动必须在手动模式下；用完必须 Exit）
//    研究"单点定位/重复精度"时最常用。目标角请按你机型的软限位改小再测。
// ----------------------------------------------------------------------------
static int JogDemo(RobotComClient& rc) {
    DumpRcResult("init", rc.init(ROBOT_IP, ROBOT_PORT));
    static const double TARGET_J[6] = { 90, 0, 0, 0, 0, 0 };   // J1 整臂回转 90 度，肉眼最明显   // 度；先改成离当前位姿很近的小角度
    if (!Confirm("即将做绝对位置点动，机械臂会运动到 TARGET_J。已确认工作空间无干涉？")) return 2;

    std::printf("  注意: 点动要求示教器处于手动模式(MANUAL)；关节目标值请自行核对软限位。\n");
    DumpRcResult("OverrideSetCmd", rc.OverrideSetCmd(SAFE_OVERRIDE_FIRST_RUN));

    if (rc.getMotorEnableState() == 0) {
        DumpRcResult("EnableMotorCmd", rc.EnableMotorCmd());
        WaitFor([&] { return rc.getMotorEnableState() == 1; }, "motor enabled");
        sleep_ms(300);
    }

    STEPROBOTSDK::JointsPos tgt;
    for (int i = 0; i < 6; ++i) tgt[i] = TARGET_J[i];
    DumpJoints("  target ", tgt);

    DumpRcResult("JogToRobotAxisposCmd",
                 rc.JogToRobotAxisposCmd(STEPROBOTSDK::ePTP, tgt, TOOL_NAME, REF_NAME));
    ShowLastError(rc);
    // 点动是"持续指令"，连接断开运动即中止 -> 必须原地等到到位
    for (int i = 0; i < 1800; ++i) {   // 最长 180s
        STEPROBOTSDK::AXISPOS a = rc.getAxisPos();
        double maxdiff = 0.0;
        for (int k = 0; k < 6; ++k) {
            double d = a.m_Joint[k] - tgt[k];
            if (d < 0) d = -d;
            if (d > maxdiff) maxdiff = d;
        }
        if (i % 10 == 0) DumpJoints("  move   ", a.m_Joint);
        if (maxdiff < 0.01) { std::printf("  [reached] after %d x 100ms\n", i); break; }
        sleep_ms(100);
    }
    DumpJoints("  now    ", rc.getAxisPos().m_Joint);

    DumpRcResult("JogToPositionExitCmd", rc.JogToPositionExitCmd());  // 不退出会占用后续示教/操作
    return 0;
}

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 4) write —— 用 TCP 沿"国庆快乐"字轮廓走一遍，并记录真实 TCP 轨迹
static int WriteCharacters(RobotComClient& rc) {
    DumpRcResult("init", rc.init(ROBOT_IP, ROBOT_PORT));
    DumpRcResult("OverrideSetCmd", rc.OverrideSetCmd(50.0));
    if (rc.getMotorEnableState() == 0) {
        DumpRcResult("EnableMotorCmd", rc.EnableMotorCmd());
        WaitFor([&] { return rc.getMotorEnableState() == 1; }, "motor enabled");
        sleep_ms(300);
    }

    std::vector<STEPROBOTSDK::RobotCartPos> trace;
    for (int i = 0; i < WRITE_PATH_N; ++i) {
        const WPt& w = WRITE_PATH[i];
        STEPROBOTSDK::CartPos tgt;
        tgt.m_X = w.x; tgt.m_Y = w.y; tgt.m_Z = w.z;
        tgt.m_A = WRITE_ORI[0]; tgt.m_B = WRITE_ORI[1]; tgt.m_C = WRITE_ORI[2];
        tgt.m_Mode = 0;
        STEPROBOTSDK::JogPositionType jt = w.pen ? STEPROBOTSDK::eLine
                                                 : STEPROBOTSDK::ePTP;
        DumpRcResult("Jog", rc.JogToRobotCartposCmd(jt, tgt, TOOL_NAME, REF_NAME));
        // 轮询到位（3mm），最多 7.2s
        double d = 1e9;
        for (int k = 0; k < 120; ++k) {
            RobotCartPos cp = rc.getCartPosWorld();
            double dx = cp.cart[0] - tgt.m_X;
            double dy = cp.cart[1] - tgt.m_Y;
            double dz = cp.cart[2] - tgt.m_Z;
            d = sqrt(dx*dx + dy*dy + dz*dz);
            if (d < 3.0) break;
            sleep_ms(60);
        }
        RobotCartPos cp = rc.getCartPosWorld();
        trace.push_back(cp);
        std::printf("  [%3d/%d] d=%.1fmm  X=%.0f Y=%.0f Z=%.0f\n",
                    i + 1, WRITE_PATH_N, d, cp.cart[0], cp.cart[1], cp.cart[2]);
    }

    // 轨迹落盘（真实 TCP），供 3D 视图高亮
    FILE* f = fopen("tcp_trace.json", "w");
    if (f) {
        std::fprintf(f, "{\"n\":%d,\"pts\":[", (int)trace.size());
        for (size_t i = 0; i < trace.size(); ++i) {
            std::fprintf(f, "%s[%.2f,%.2f,%.2f]", i ? "," : "",
                         trace[i].cart[0], trace[i].cart[1], trace[i].cart[2]);
        }
        std::fprintf(f, "]}");
        std::fclose(f);
        std::printf("  trace saved -> tcp_trace.json (%d pts)\n", (int)trace.size());
    }
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // 输出全是 ASCII，这一行只为万一要打印 SDK 版本串等
#endif
    const char* mode = (argc > 1) ? argv[1] : "probe";

    RobotComClient rc;
    rc.setInterfaceWaitTime(200);   // 接口等待时间(ms)，链路慢时可调大

    std::printf("STEP ROBOT C++ starter | mode=%s | target=%s:%u\n", mode, ROBOT_IP, ROBOT_PORT);
    std::printf("------------------------------------------------------------\n");

    int code = 1;
    if      (!std::strcmp(mode, "probe")) code = Probe(rc);
    else if (!std::strcmp(mode, "run"))   code = RunExistingProgram(rc);
    else if (!std::strcmp(mode, "jog"))   code = JogDemo(rc);
    else if (!std::strcmp(mode, "write")) code = WriteCharacters(rc);
    else {
        std::printf("usage: %s [probe|run|jog|write]\n", argv[0]);
        code = 2;
    }

    std::printf("\n------------------------------------------------------------\n");
    std::printf("done, exit=%d\n", code);
    rc.close();          // 说明书正文未列出 close 的语义, 头文件里有; 显式调用一次
    return code;
}
