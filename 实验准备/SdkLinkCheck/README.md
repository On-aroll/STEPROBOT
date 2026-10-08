# SdkLinkCheck —— SDK 链路自检（只读、不使能、不动）

阶段 1「现场链路联调」第 8 步用的工具。连接控制器 SDK 端口后读取全部只读状态，
再按周期轮询，验证链路稳定性并打印状态变化。

## 安全闸是怎么设计的

程序反射加载厂商 DLL，**只调用「名称以 `get` 开头 + 无参数 + 非 void 返回」的接口**，
再用一张窄排除表滤掉 `Cmd / Modify / Set / Enforce / Simulation / Load / Kill / Send` 等动作词。

这道闸可靠的原因：本 SDK 里所有会改变机器人状态的方法**要么带参数**（运动目标、端口号、变量名），
**要么以 `Cmd` 结尾** —— `EnableMotorCmd`、`SetModeCmd`、`JogToRobotAxisposCmd`、
`AllAlarmConfirmCmd`、`SDKSimulateProgramLoad` 全都落在闸外。
启动时会打印"检测到但已拒绝调用"的清单，供人工复核。

> 排除表里**不能**放 `Enable / Jog / Run / Stop / Alarm`。
> 这是实测踩到的坑：按子串匹配会误杀 `getMotorEnableState()`（阶段 1 最需要读的字段）
> 和 `getJogRefSys()`。桩件冒烟测试就是为了盯住这条。

## 用之前

```powershell
winget install --id Microsoft.DotNet.SDK.8 --exact   # 已验证：8.0.425
```

> 安装器会把 `C:\Program Files\dotnet` 写进**机器级 PATH**（已验证），但**只有安装之后新开的终端**才生效。
> 装之前就已经打开的窗口会仍然提示找不到 `dotnet`，重开窗口即可，不必手动改 PATH。

本工程**编译期不引用厂商 DLL**，所以现在就能 `dotnet build` 通过。
目标框架为 `net8.0`：厂商手册 1.3 要求 .NET 7「或更高」，而实测本机运行时只有
5.0.3 / 6.0.23 / 8.0.31，`net7.0` 产物能编译但启动即报
`You must install or update .NET to run this application`（默认 roll-forward 不跨大版本）。
已另加 `RollForward=LatestMajor`，换台只有更新运行时的电脑也能跑。

## 用法

```powershell
dotnet build

# 拿到 DLL 后：离线核对接口签名，不需要机器人、不需要网络（推荐先做这一步）
# 把 Robot-SDK-CSharp.dll（连同其原生依赖）放进 libs\
dotnet run -- --dump

# 到现场：验证链路，轮询 5 分钟
dotnet run -- --ip 192.168.39.220 --port 30312 --seconds 300

# 只读 30 秒、200ms 周期
dotnet run -- --seconds 30 --interval 200
```

## 读输出

- `init` 返回码：`0` 正常 / `-1` 建立失败（多为示教器占用 30312 或网段不对）/
  `-2` 数据收发异常（怀疑控制器固件低于 V1.8.0.20240531_0712）/ `-3` 通讯断开
- `--dump` 输出每行前的 `读` 标记 = 该接口会进入白名单被真实调用
- 首轮快照打印每个 getter 的**真实字段名与值** —— 手册没写结构体字段名，
  这份输出就是"C# 侧字段实测清单"，务必截图存档
- 之后只打印**变化事件**，长时间跑也不刷屏
- 退出码：`0` 链路稳定 / `1` 有调用异常 / `2` 缺 DLL 或找不到类型 / `3` 网络不通 / `4` init 失败

## 冒烟测试（不依赖机器人，随时可跑）

`__test__\` 下有一个桩件 SDK，方法名与真厂商产物一致，用来验证本工具自身的
白名单/黑名单行为。用法见 [__test__/说明.md](./__test__/说明.md)。**桩件 DLL 绝不能带到现场当真货用。**

## 已知边界

- 单位（毫米 / 度）与 `m_Mode` 的含义在文档中未官方确认，首轮快照请对照实际姿态手工核一次
- 预检会先建立再关闭一个裸 TCP 连接；若紧接着的 `init` 返回 −1/−2，程序自动重试一次
- 若厂商 DLL 是 32 位，把 csproj 的 `PlatformTarget` 改成 `x86`
- 桩件只覆盖"方法名分类"和"渲染"两层，真 DLL 的原生依赖缺失（`DllImport` 找不到
  配套 .dll）这类问题只有现场才能暴露
