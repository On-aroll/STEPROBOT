using System;
using System.Net;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using StepRobotSDKCSharp;

namespace StepModeSwitch {
    sealed class SdkModeRobot : IModeRobot, IDisposable {
        readonly RobotComClientCSharp sdk = new RobotComClientCSharp();
        public SdkModeRobot(string ip) {
            if (sdk.init(ip, 30312) != 0) throw new InvalidOperationException("无法连接控制柜 SDK 端口 30312");
            var info = new RobotInfoStruct();
            if (sdk.RequestRobotInfoCmd(info) != 0) throw new InvalidOperationException("未能读取机器人身份");
            var model = Regex.Replace(info.m_Name ?? "", "[^a-z0-9]", "", RegexOptions.IgnoreCase).ToLowerInvariant();
            if (!model.StartsWith("sr1753200")) throw new InvalidOperationException("设备型号不匹配：" + info.m_Name);
            Thread.Sleep(300);
        }
        public bool Connected { get { return sdk.ConnectStatus() == 0; } }
        public bool Stopped { get { return sdk.getProgramState().ToString() == "eStop"; } }
        public bool Enabled { get { return sdk.getMotorEnableState() != 0; } }
        public int Mode { get {
            switch (sdk.getOperationMode().ToString()) {
                case "eManual": case "eManualHigh": return 1;
                case "eAutomatic": return 2;
                case "eAutomaticExternal": return 3;
                default: return 0;
            }
        } }
        public int SetMode(int target) { return sdk.SetModeCmd(target == 1 ? MODEKEY.MANUAL : target == 2 ? MODEKEY.AUTO : MODEKEY.AUTO_EXT); }
        public void Dispose() { sdk.close(); }
    }
    static class Program {
        static int Main(string[] args) {
            Console.OutputEncoding = new System.Text.UTF8Encoding(false);
            string nonce = ""; int target = 0;
            try {
                if (args.Length != 3) throw new ArgumentException("需要 IP、目标模式与请求编号");
                IPAddress address;
                if (!IPAddress.TryParse(args[0], out address) || address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
                    throw new ArgumentException("无效 IPv4 地址");
                if (!int.TryParse(args[1], out target) || target < 1 || target > 3) throw new ArgumentException("无效模式");
                nonce = args[2];
                if (!Regex.IsMatch(nonce, "^[a-f0-9]{32}$")) throw new ArgumentException("无效请求编号");
                int observed;
                using (var robot = new SdkModeRobot(address.ToString())) {
                    ModeSwitchCore.Switch(robot, target, Thread.Sleep);
                    observed = robot.Mode;
                }
                Console.WriteLine(new JavaScriptSerializer().Serialize(new { ok = true, target = target, observed = observed, nonce = nonce }));
                return 0;
            } catch (Exception e) {
                Console.WriteLine(new JavaScriptSerializer().Serialize(new { ok = false, target = target, nonce = nonce, error = e.Message }));
                return 1;
            }
        }
    }
}
