# R6.4 仓库更新验证 · 2026-11-20

本目录保存本次准备提交的源码在独立 Git 检出目录中的构建、离线测试和现场历史回执摘要。运行环境为 Windows x64、Qt 6.8.2、MinGW 13.1、CMake/Ninja。测试不连接现场 PLC 或控制柜。

## 验证入口

| 验证 | 内容与日志 |
|---|---|
| Qt/C++ 与界面 | [完整 CTest](ctest.log)，共31项：Modbus、固定作业、文件传输、交付流程、微调、六轴姿态及普通/小/宽窗口 |
| 自动/微调/文件桥 ST | [101项](st_tests.log)：自动15、快速传输19、独立六轴8、微调46、旧焊机协议13 |
| 主桥 ST | [33项](main_st_regression.log)，主桥行为回归及手动会话隔离 |
| WSME 只读诊断 ST | [22项](welder_probe_tests.log)，实际源码配模拟只读 CoE 响应，不验证实机工艺协议 |
| SDK模式核心 | [C#核心测试](mode_core_tests.log)，三个目标模式、停稳/断使能限制、拒绝、状态变化和缺失回读；未调用厂家SDK |

仓库根目录可执行：

```powershell
python -m unittest discover -s workstation/pc_control/tests -p 'test_*_st.py' -v
python workstation/pc_control/tests/test_main_st_regression.py -v
python -m unittest discover -s docs/welder_wsme400plus_r1/tests -p 'test_*.py' -v
```

CMake/CTest 构建方法见[上位机说明](../../../workstation/README.md)。ST 测试解释执行实际 ST 源码和状态机，模拟原生网络功能块/扫描时钟；不是 InoProShop 编译器或真实 PLC 性能测试。SDK 模式程序本体和 `helix_pc` 需要本机厂商 SDK，本次验证其可独立运行的逻辑与 Qt 桥接口，未重新链接厂商库。

## 首次失败与修复

[首次 CTest 日志](ctest_initial_failure.log)保留了 `welder_and_mode_bridge` 的一次失败：请求超过3秒没有应用回执时，原 `tick()` 先决定心跳续租，再标记超时。同一次定时检查可能把最后一次续租先发出，导致“结果待确认后不再续租”断言失败。

现将超时判定放在续租判断之前，不改变请求超时阈值，也不自动重发工艺命令。[单项连续5次复验](welder_repeat.log)用于检查该边界，随后重新执行完整 CTest。该源码修正同时回写本机开发源文件；未替换现场正在运行的 EXE。最终结果以 `ctest.log` 及 `validation_manifest.json` 为准。

第二轮完整测试中，小窗口轨迹截图出现一次进程异常退出，见[该轮日志](ctest_graphics_failure.log)。Windows事件记录为 `Qt6Gui.dll 6.8.2 / 0xc0000005 / offset 0x1e325`，邻近导出符号为 `QImageData` 析构函数；仅凭模块/偏移不能确定根因。同一二进制随后[单项连续3次](trajectory_repeat.log)通过，再执行完整套件。此偶发图形崩溃仍待稳定复现，未修改图形代码或宣称已修复。

## 现场历史证据

[回执摘要](field_receipts_sanitized.json)保留5笔2026-11-20的记录、原回执SHA-256、每文件字节数/哈希、耗时及最终状态，省略账号密码、IP、会话号和本机路径。

| 时间（上海） | 路线 | 文件字节数合计 | 结果 | 总耗时 |
|---|---|---:|---|---:|
| 15:28:12 | PLC转传 | 44,936 | COMPLETE，4文件回读通过 | 4,703 ms |
| 16:45:14 | 电脑直传 + PLC回读 | 132,105 | COMPLETE，4文件回读通过 | 871 ms |
| 16:46:00 | 电脑直传 | 0（未完成文件） | FTP登录530，遗留会话 | 27 ms |
| 16:46:32 | 再次上传预检 | 0 | 状态900，E011，拒绝 | 1 ms |
| 16:48:05 | 电脑直传 + PLC回读 | 132,105 | COMPLETE，4文件回读通过 | 860 ms |

两条路线使用的作业不同，以上不是相同文件/相同条件下的速度对照实验。`COMPLETE` 只表示文件上传/校验，不表示机器人加载或运动。现场记录均早于最新连续上传恢复修复，不能据此宣称该修复已完成现场复验。

## 界面证据

以下均为本次离线截图；模型角度、参数和端点使用测试数据。模型网格本体不分发。

- [宽屏联动打印页](screenshots/linked_print_wide.png)：机器人操作、姿态/轨迹和焊机参数同页。
- [小窗口焊机页](screenshots/welder_small.png)：窄屏参数及姿态布局。
- [轨迹高亮](screenshots/trajectory_36mm_regression.png)：使用保留的旧36 mm点阵回归夹具；新尺寸文件见仓库根的 `dianzhen_R64_dryrun_36cm.nc`。
- [电脑直传配置](screenshots/direct_upload_config.png)：路线、密码显示按钮和固定11号；截去底部临时回执路径，测试密码始终遮挡。

源文件、示例、截图与本次构建的哈希见 `validation_manifest.json`。现场安装EXE的哈希单独标注，不能与本次从公开配置源码构建的EXE混用。
