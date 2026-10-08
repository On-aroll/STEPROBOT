// 桩件 SDK —— 仅用于冒烟测试 SdkLinkCheck 工具自身，不是厂商产物。
// 命名空间/类名/方法名刻意与《SDK使用说明 C# V2.3》一致，用来验证：
//   反射能否发现类型、只读白名单能否跑通、Enable/Jog/Set/Cmd 是否确实被拒绝、
//   结构体与数组能否正确渲染、状态变化能否被差分打印出来。
using System;
using System.Collections.Generic;
using System.Linq;

namespace StepRobotSDKCSharp
{
    public struct JointsPosStrcut { public double[] m_jointData; }

    public struct RobotCartPos { public double[] cart; public uint m_Mode; }

    public struct AXISPOS { public JointsPosStrcut m_Joint; public JointsPosStrcut m_AuxJoint; }

    public struct MessageData
    {
        public int m_MessageType;
        public int m_MessageID;
        public string m_MessageString;
        public long m_MessageTime;
    }

    public struct RobotDI { public ulong[] m_DI; }

    public enum OPERATIONMODE { eManual = 0, eManualHigh, eAutomatic, eAutomaticExternal }

    public enum REFSYS { eJoints = 0, eWorld, eBase, eTool, eCustom, eGo, eTTS }

    public class RobotComClientCSharp
    {
        private readonly object Gate = new();
        private int _state = 0;                       // 0 = 未使能，与阶段 1 现场一致
        private double[] _joint = { 7.02, -12.5, 30, 0, 90, -180 };
        private double _override = 15.0;
        private int _tick;

        public int init(string ip, int port) => 0;
        public string SDKVersion() => "V2.3-STUB";

        // ---- 只读接口：SdkLinkCheck 应当全部调用到 ----
        public string getProjectName() => "project0";
        public string getProgramName() => "program0";
        public string getToolName() => "Default";
        public OPERATIONMODE getOperationMode() => OPERATIONMODE.eManual;
        public REFSYS getJogRefSys() => REFSYS.eJoints;
        public int getMotorEnableState() { lock (Gate) return _state; }
        public double getOverride() { lock (Gate) return _override; }
        public int getCurrentLine() { lock (Gate) return ++_tick % 7; }   // 制造变化事件，验证差分打印
        public AXISPOS getAxisPos()
        {
            lock (Gate)
            {
                return new AXISPOS
                {
                    m_Joint = new JointsPosStrcut { m_jointData = (double[])_joint.Clone() },
                    m_AuxJoint = new JointsPosStrcut { m_jointData = new double[] { 0, 0, 0, 0, 0, 0 } }
                };
            }
        }
        public RobotCartPos getCartPosWorld() => new RobotCartPos
        {
            cart = new double[] { 800.11, 10.22, 1027.33, 0, 90, -180 },
            m_Mode = 0
        };
        public RobotDI getRobotDI() => new RobotDI { m_DI = new ulong[] { 0x15, 0, 0, 0 } };
        public MessageData getMessageData() => new MessageData
        {
            m_MessageType = 2,
            m_MessageID = 4001,
            m_MessageString = "机器人没有上使能",
            m_MessageTime = 0
        };

        // ---- 命令接口：SdkLinkCheck 绝不允许调用；上位机适配层在 --allow-motion 下才应触达 ----
        public int EnableMotorCmd() { lock (Gate) _state = _state == 0 ? 1 : 0; Log("EnableMotorCmd", $"-> {_state}"); return 0; }
        public int SetModeCmd(int mode) { Log("SetModeCmd", mode); return 0; }
        public int OverrideSetCmd(double ori) { lock (Gate) _override = ori; Log("OverrideSetCmd", ori); return 0; }
        public int JogToPositionExitCmd() { Log("JogToPositionExitCmd"); return 0; }
        public int AllAlarmConfirmCmd() { Log("AllAlarmConfirmCmd"); return 0; }
        public int ProgramLoadCmd(string p, string q, List<int> e) { Log("ProgramLoadCmd", p); return 0; }

        public int JogToRobotAxisposCmd(int type, JointsPosStrcut v, string t, string r)
        {
            var cmd = v.m_jointData ?? Array.Empty<double>();
            lock (Gate) _joint = (double[])cmd.Clone();
            Log("JogToRobotAxisposCmd",
                $"type={type} tool={t} ref={r} joint=[" +
                string.Join(",", cmd.Take(6).Select(d => d.ToString("0.##"))) + "]");
            return 0;
        }

        private static void Log(string what, object? arg = null)
            => Console.WriteLine($"        [桩件调用] {what}{(arg is null ? "" : "(" + arg + ")")}");
    }
}
