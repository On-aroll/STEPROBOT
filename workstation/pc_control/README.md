# R6.4 上位机源码 · 2026-11-20

构建与本机配置见[上一级说明](../README.md)，当前实现/现场验证边界见[项目现状](../../docs/项目现状-2026-11-20.md)。本目录为 GitHub 中的上位机源码入口。

| 模块 | 文件 |
|---|---|
| 界面及共享状态 | `workstation.cpp`、`workstation_ui.h`、`workstation_print.h`、`workstation_settings.h` |
| G代码转换、快照与多文件作业 | `workstation_job.*`、`workstation_converter.*`、`workstation_delivery.*`、`workstation_deliverydialog.*`、`gcode_converter.cpp` |
| Modbus与机器人主桥 | `workstation_modbus.*`、`workstation_plcbridge.*`、`workstation_plcjob.*`、`workstation_plcpanel.*` |
| 文件桥及双通道上传 | `workstation_filebridge.*`、`workstation_directftp.*`、`workstation_fileuploaddialog.*` |
| 微调、实验起点和六轴姿态 | `workstation_manualbridge.*`、`workstation_twin.*`、`assets/*.json` |
| 焊机与模式 | `workstation_welder.*`、`workstation_mode.*`、`mode_helper/` |
| 既有SDK会话、回程与回放 | `workstation_session.*`、`workstation_home.*`、`workstation_replay.*`、`helix_pc.cpp` |
| 离线验证 | `tests/`；CMake 注册完整 Qt/C++/界面测试，ST测试单独执行 |

PLC 配套源文件及安装步骤使用[R6.4 自动流程](../../docs/auto_job_r6_4/从这里开始.md)与[R6.3 手动服务](../../docs/manual_control_r6_3/先看这里.md)。本目录中的 `plc_modbus_bridge_v2_gdi255/` 和根目录协议 CSV 是历史基础接口资料，升级时不能覆盖当前配套版本。

典型流程：导入 G 代码→自动转换→选 PLC 转传或电脑 FTP 直传→PLC 回读确认→显示可用11号→登记、加载、确认、使能、运行。10号继续用于微调/起点；日常上传不改手动服务和起点。自动入口允许1～15个程序。

默认使用当前显示内容快照，支持多文件原文和轨迹同步、行映射、高亮与失败清理。三维模型显示真实六轴反馈，轨迹是本地规划值；缺失或过期数据会标记。SDK文件直传不等于切换机器人控制通道。

现场固定密码从 `STEP_FTP_PASSWORD` 或 EXE 同目录 `workstation.local.ini` 读取，示例见 `workstation.local.ini.example`。网格需用 `tools/prepare_assets.py` 从本机许可资源补齐；厂商SDK与网格不分发。

旧焊机32字节适配尚不适用于现场16字节PDO的新机型，SDK模式切换、最新连续上传错误恢复和完整焊接联动仍按[项目现状](../../docs/项目现状-2026-11-20.md)列出的范围继续现场验收。

```powershell
python -m unittest discover -s workstation/pc_control/tests -p 'test_*_st.py' -v
```

上述 ST 命令从仓库根目录执行。测试使用本地定时器、模拟报文和实际 ST 源码解释执行；不修改在线 PLC。新机型的只读诊断测试位于 `docs/welder_wsme400plus_r1/tests`。
