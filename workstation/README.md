# STEP 打印作业台 · Windows x64

当前 C++20 / Qt 6.8.2 版本，Modbus TCP 重构完成于 2026-11-13。适用于 Windows 10/11 x64，启动无需 Python、Qt SDK 或管理员权限。厂家 STEP C++ SDK 通信已整体移除：上位机不再发送任何运动指令，运动控制（点动、启停、程序加载、回程、急停）一律在示教器操作。

上位机现在的三个职责：

1. **G 代码工具链**：打开、编辑、转换为 STEP 程序工程 ZIP（SRP/SRD + 行映射），送 STEPStudio 验证后由 STEPStudio 或 U 盘传入控制柜。
2. **轨迹预览**：纯本地几何视图，不依赖控制器回读。
3. **Modbus TCP 主站**：连接机器人从站（默认 `192.168.39.220:502`），轮询 DI/AI 寄存器、写保持寄存器，为将来 AC802 PLC 主站联动验证协议约定。

## 另一台电脑直接使用

1. 下载 `releases/` 下的安装 EXE，可用同目录 `SHA256SUMS.txt` 核对下载完整性。注意：`releases/` 中的旧安装包仍含 SDK 控制端，需用本分支源码重新打包后再分发。
2. 双击安装，默认装入当前用户的 `%LOCALAPPDATA%\Programs\STEP Print Workstation`，开始菜单有启动入口。
3. 打开程序，导入自己的 G 代码或使用安装目录 `examples/` 中的螺旋（luoxuan）、薄壁（baobi）示例。轨迹预览、原文、转换、工程编辑与导出无需 STEPStudio。

克隆本分支后，可直接运行 `workstation/releases/` 的安装 EXE；不需要先编译。安装包未进行商业代码签名，SHA-256 只验证完整性，不替代来源验证。

## 已包含与需补充内容

| 内容 | 是否包含 | 用途与补充方式 |
|---|---|---|
| 上位机、G 代码转换器 EXE | 包含 | Modbus TCP 版本，无厂家 SDK 依赖 |
| Qt 6.8.2 DLL、Windows 插件、MSVC x64 运行库 | 包含 | app-local，正常运行不要求安装开发工具 |
| 两份 G 代码示例、v4 原生模板、行映射与空跑转换功能 | 包含 | 模板很小，完整保留；工艺输出未接入 |
| STEPStudio 4.7.0 完整软件 | 自行补充 | 仅用于人工验证与导入程序工程 ZIP，上位机不连接它 |
| SR175-3200 机器人与控制柜 | 现场设备 | Modbus TCP 从站在示教器启用；实机联调需现场验收 |
| Qt SDK、MSVC、CMake、Ninja | 仅源码构建时补充 | Qt **6.8.2 MSVC2022 x64**，MSVC 2022 x64，CMake 3.24+ |
| 相机图像、实际工具/工件坐标、实机 IP 与柜号 | 自行补充 | 不上传个人采样、运行历史、授权文件和现场设备身份 |

数据目录配置见 `pc_control/workstation.paths.example.json`（当前只有 `data_root` 一个字段），也可用 `STEP_DATA_ROOT` 环境变量覆盖；默认保存在 `%LOCALAPPDATA%\STEPROBOTICS\StepWorkstation`。Modbus 连接仅允许控制柜所在私有网段（10.x / 172.16–31.x / 192.168.x）。

机器人侧 Modbus 从站启用路径：示教器「参数配置 → 系统配置 → 总线 → ModBus-TCP」，保存后必须点「重置」才生效。寄存器映射：Word1–4 打包 DI33–96，Word5–32 对应 AI5–32，支持 4X 保持寄存器（FC03/06/16）与 3X 输入寄存器（FC04）。详见 `pc_control/README.md`。

## 从源码构建

在 MSVC 2022 x64 开发终端中：

```powershell
cd workstation
.\build.ps1 -QtRoot 'C:\Qt\6.8.2\msvc2022_64'
.\Start-Workstation.cmd
```

脚本完成 CMake 配置（Ninja + Release）、构建、ctest，并把 EXE 与 Qt 运行库部署到 `pc_control/native`。开发依赖不存入仓库。

`pc_control/README.md` 是详细操作说明，`pc_control/CONVERSION_GUIDE.md` 介绍转换边界。默认空跑，未接入激光、送丝、挤出和保护气输出。

## 测试与打包

测试入口：`ctest --test-dir build --output-on-failure`。当前三个测试：`modbus_client`（QTcpServer 从站模拟器覆盖轮询/写入/异常/超时/迟到帧）、`gcode_job`（转换、圆弧离散、速度组、起点重编译、BOM、拒绝输入）、`robot_identity_aliases`。

打包脚本：`packaging/package.ps1`；安装脚本：`packaging/StepWorkstation.iss`，使用 Inno Setup 6.7.4 或兼容版本。发布脚本检查最终 EXE 小于 **50,000,000 字节**，并生成 SHA-256。源码、配置示例及安装包只出现在独立分支，默认分支未修改。

## 依赖来源与许可

- Qt 按动态 DLL 部署，许可和第三方声明随安装包提供在 `licenses/Qt-6.8.2/`，未对 Qt DLL 加密或静态嵌入上位机。[Qt 6.8 Windows 部署说明](https://doc.qt.io/qt-6.8/windows-deployment.html)。
- 对应 Qt 源码：[QtBase 6.8.2](https://github.com/qt/qtbase/tree/v6.8.2)，[Qt 6.8.2 完整源码归档](https://download.qt.io/archive/qt/6.8/6.8.2/single/)。本仓库包含上位机源码和构建说明，允许自行重编并替换动态依赖；第三方许可独立于本项目。
- STEPStudio、工作站授权和机器人模型遵循厂家许可，开发包和授权文件不在此仓库分发。[厂家资料入口](https://www.stepelectric.com/Download/1773178589210951680-12-12.html)。
- MSVC 运行库来自微软 x64 运行时；需要补装或修复时使用[微软官方支持页](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。
