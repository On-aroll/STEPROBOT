# PLC桥接R3：V1/V2构建与验证记录

日期：2026-11-16（Asia/Shanghai）。版本：2026.11.16-plc-bridge-v2-r3。

## 当前部署

- 安装目录：F:\STEPROBOTICS\STEP Print Workstation\STEP Print Workstation
- 当前运行文件：pc_control/native_plc_bridge_v2_r3/StepWorkstation.exe。
- 启动脚本及现有匹配的快捷方式指向R3；保留R2、R1和原始native目录。
- R2启动回退入口：Start-Workstation-PLC-R2.cmd。
- 本次安装文件备份：更新备份_20261116_PLC_R3。
- PLC交付包位于pc_control/plc_modbus_bridge_v2_gdi255；原C盘V1交付包原样保留。
- 上位机源码：F:\STEPROBOTICS\STEPROBOTICS_源码_20261115\STEPROBOTICS\STEPROBOT\workstation
- EXE SHA256：723490a2b6b2fa32ed98292690465fedb67efa9c52b8dca75a41d5668f0aa06f

## 本次功能

V2采用原厂GDI8选程序方式，编号1～255。机器人FC16输出从1WORD扩展为单一通道的2WORD：第一字保留系统控制命令、旧四槽位固定0；第二字DI49～56二进制程序号、DI57～64固定0。需要本地新增xGdiProgramMappingConfirmed确认。

上位机按读取的版本与能力限制程序号：V1仍1～4，V2才1～255；命令带对应协议版本。未知能力、会话期间版本改变均拒绝继续控制。新增GDI字与能力诊断，保留加载后人工核对与独立运行。

## 已完成的本地检查

- CTest 8组全部通过：robot_identity_aliases、gcode_job、modbus_client、plcjob_client、return_tcp_speed_limit、plc_bridge、plc_bridge_ui、plc_bridge_panel。
- Qt回环协议测试：2654项检查、0失败。涵盖所有255个GDI值的位解码，1/4/5/16/128/255报文、版本协商、V1回归、越界、映射未确认和STOP。
- UI测试：73项检查、0失败。实际触发V2编号16/255加载和255人工确认，验证旧V1限制、GDI字及本地许可禁用。另一次带截图检查为74项（额外一项是截图保存）。
- 交付ST源码扫描行为测试26项全部通过，直接解析并执行交付代码；其中逐一验证1～255，程序16加载前500ms稳定、加载独立于运行、映射丢失、旧版本拒绝和越界。此工具是严格ST子集解释器，不是IEC编译器或汇川运行时。
- 安装版在只有Windows系统PATH时独立启动自检退出0，stderr为空，启动测试使用隔离设置，不连接设备。
- 原生主窗口及高编号面板已渲染并人工查看；参考PLC_BRIDGE_UI.png、PLC_BRIDGE_UI-GDI255.png。
- 255行程序号核对表逐一检查位权一致；工程名、程序名为空待现场填写，未伪造绑定。

## 尚需现场完成

没有操作或下载实际PLC工程，没有修改示教器GDI配置，也没有连接现场PLC/机器人发送动作。实际IDE编译、通讯组态、程序名绑定和运动验证尚待用户按PLC_BRIDGE_V2.md完成。即使已安装R3，在旧V1 PLC上仍只能使用四槽。

SDK桥接及其原MSVC运行库沿用原版，本次没有改变SDK控制方案，也没有增加点动、焊机或送丝控制。所有网络测试仅使用本机回环模拟器。
