# STEP 打印作业台 · SR175-3200

**操作者请看 [使用说明.md](使用说明.md)**（三种连接模式的操作流程、Modbus 机器人侧配置、常见问题）；本 README 面向构建与开发。

在 Windows 资源管理器双击 `Open_Pendant.cmd`（或仓库根的 `Start-Workstation.cmd`）运行 `native/StepWorkstation.exe`，使用 C++20、Qt Widgets 和 MSVC x64。断电级急停始终在示教器实体按钮上。

一个程序，三种连接方式（工具栏下拉切换）：

| 模式 | 端口 | 用途 |
| --- | --- | --- |
| STEP 虚拟控制器 | 隔离端口 30316/30317 | 本机仿真：拉起 STEPStudio 内核执行 SRP/SRD，验证作业不碰实机 |
| 实机 STEP SDK | 控制柜 30312 | SDK 直连控制柜：点动、启停、程序加载、上位机起点与回程 |
| Modbus TCP | 从站 502 | 按系统信号位号直控（启动/停止/暂停/复位/加载/GDI 程序号），无实时回读 |

## 上位机做什么

1. **G 代码工具链**：打开 `.nc/.gcode` 或 STEP ZIP，查看原文与生成的 SRP/SRD 工程树，双击运动行设置起始行重新编译，从「工程 / 工具」导出完整 STEP 工程 ZIP（含源行映射与哈希），送 STEPStudio 验证后导入。详见 [转换与工程编辑说明](CONVERSION_GUIDE.md)。
2. **轨迹预览**：等轴 / XY / XZ 几何视图，计划路径 + 控制器回读轨迹叠加（SDK / 虚拟控制器模式）。
3. **手动点动**：SDK 与虚拟控制器模式下六轴 / WORLD / 基座 / 工具 / 用户坐标点动，各轴实时角度回读，TCP 上限 250 mm/s。Modbus 模式点动请在示教器操作。
4. **上位机起点与回程**：按控制器身份保存六轴起点，独立规划关节分段回程，TCP 限速并二次确认。
5. **Modbus TCP 主站**：机器人作为从站，写保持寄存器 Word1–4 控制 DI33–96；也是 AC802 PLC 联动的协议验证工具。

## Modbus DI 直控约定

手册 V1.8.0 第四章：Word1–4 打包 DI33–96（bit0 为 DI33），写保持寄存器地址 0–3 即整组写入。信号语义（位号在界面可配）：

- **启动 DI33 / 复位 DI36 / 加载 DI37**：脉冲信号（高 600 ms 自动回落）；「开始」先清停止/暂停电平再脉冲启动。
- **停止 DI34 / 暂停 DI35**：电平信号，保持期间无法启动；按钮为切换式（再点一次解除）。
- **程序号**：GDI 起始位号起 8 位（DI40–47）二进制写入，随后脉冲加载信号。
- 机器人侧需在示教器「配置 → 系统IO」绑定上述信号，GDI 组绑定目标程序，钥匙开关切到 **AUT EXT 外部自动**。运行状态无回读，以示教器为准。
- 连接仅允许控制柜所在私有网段（10.x / 172.16–31.x / 192.168.x）。

## 构建

在 MSVC 2022 x64 开发环境运行：

```powershell
powershell -File build.ps1 -QtRoot 'C:\Qt\6.8.2\msvc2022_64'
```

脚本执行 CMake 配置（Ninja + Release）、构建、ctest，并把 `StepWorkstation.exe`、`StepGcodeConverter.exe`、`helix_pc.exe` 与 Qt 运行库部署到 `pc_control/native`。Qt 固定 6.8.2 MSVC2022 x64（ZIP 读取接口与其绑定）。

`helix_pc.exe` 是 SDK 桥接进程（虚拟控制器准备 + 实机 attach），需部署在 `StepWorkstation.exe` 旁边；仓库无 vendor SDK 库时 CMake 跳过编译、部署现成 exe。

## 测试

| 测试 | 覆盖 |
| --- | --- |
| `modbus_client` | 内置 QTcpServer 从站模拟器：FC03/04 轮询、FC06/16 写入、异常响应翻译、响应超时、迟到响应按事务 ID 丢弃、未连接写入拒绝、通信计数 |
| `gcode_job` | G 代码方言、圆弧/螺旋离散、速度组复用、起点重编译、BOM、边界与拒绝输入 |
| `robot_identity_aliases` | SR175-3200 型号别名 |
| `return_tcp_speed_limit` | 回程 TCP 限速公式 |

## 软件范围

| 文件 | 作用 |
| --- | --- |
| `workstation.cpp` | 界面：工具栏连接区、工程树、源行视图、手动点动、运行控制、Modbus 信号组 |
| `workstation_session.h/.cpp` | SDK / 虚拟控制器会话：QProcess 编排 STEPStudio 内核与 helix_pc，回读协议解析 |
| `workstation_modbus.h/.cpp` | Modbus TCP 主站（MBAP 编解码、串行事务、轮询/写入、统计） |
| `workstation_motion.h` | 回程六轴规划与 TCP 限速 |
| `workstation_experiment.h/.cpp` | 实验试次记录（输入、trace、事件） |
| `workstation_replay.h/.cpp` | 历史试次回放与对比 |
| `workstation_home.h/.cpp` | 上位机起点持久化（按控制器身份隔离） |
| `workstation_job.h/.cpp` | G 代码后置转换、ZIP 生成、文件编辑校验、映射与几何恢复 |
| `workstation_converter.h/.cpp` | 独立转换窗口 |
| `gcode_converter.cpp` | C++ 命令行转换器 |
| `workstation_plot.h` | 轨迹视图绘制（计划 / 回读 / 回程预览叠加） |
| `workstation_paths.h` | 数据目录解析（备用） |
| `workstation_identity.h` | SR175-3200 型号别名识别 |

命令行转换器用法：

```powershell
.\native\StepGcodeConverter.exe --input 'D:\path\input.gcode' --output 'D:\path\program.zip'
```
