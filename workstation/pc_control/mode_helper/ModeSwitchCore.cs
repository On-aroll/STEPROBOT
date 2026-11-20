using System;
using System.Diagnostics;

namespace StepModeSwitch {
    public interface IModeRobot {
        bool Connected { get; }
        bool Stopped { get; }
        bool Enabled { get; }
        int Mode { get; }
        int SetMode(int target);
    }
    public static class ModeSwitchCore {
        static void RequireRest(IModeRobot robot) {
            if (!robot.Connected || !robot.Stopped || robot.Enabled)
                throw new InvalidOperationException("机器人必须在线、停止且断使能");
        }
        public static void Switch(IModeRobot robot, int target, Action<int> wait) {
            if (target < 1 || target > 3) throw new ArgumentOutOfRangeException("target");
            RequireRest(robot);
            if (robot.Mode == target) return;
            if (robot.SetMode(target) != 0) throw new InvalidOperationException("控制柜拒绝模式切换");
            var timeout = Stopwatch.StartNew();
            while (timeout.ElapsedMilliseconds < 3000) {
                RequireRest(robot);
                if (robot.Mode == target) return;
                wait(50);
            }
            throw new TimeoutException("模式请求已发送，但没有读到目标模式；结果待确认");
        }
    }
}
