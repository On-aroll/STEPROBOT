# STEP 打印作业台 · Windows x64

当前 C++20 / Qt 版本，2026-11-11。适用于 Windows 10/11 x64，启动无需 Python、Qt SDK 或管理员权限。

## 另一台电脑直接使用

1. 下载 `releases/StepWorkstation-2026.11.11-win64-Setup.exe`，可用同目录 `SHA256SUMS.txt` 核对下载完整性。
2. 双击安装，默认装入当前用户的 `%LOCALAPPDATA%\Programs\STEP Print Workstation`，开始菜单有启动和配置入口。
3. 打开程序，导入自己的 G 代码或使用安装目录 `examples/` 中的螺旋、薄壁示例。轨迹预览、原文、转换、工程编辑与导出无需 STEPStudio。
4. 需要厂家虚拟控制器运行时，先补齐下表中的 STEPStudio 和工作站，再运行安装目录 `Configure-STEPStudio.cmd`。重开工作台后连接仿真。

克隆本分支后，可直接运行 `workstation/releases/` 的安装 EXE；不需要先编译。安装包未进行商业代码签名，SHA-256 只验证完整性，不替代来源验证。

## 已包含与需补充内容

| 内容 | 是否包含 | 用途与补充方式 |
|---|---|---|
| 上位机、G 代码转换器、STEP SDK 控制端 EXE | 包含 | 当前已验证版本；SDK 已链接到控制端 |
| Qt 6.8.2 DLL、Windows 插件、MSVC x64 运行库 | 包含 | app-local，正常运行不要求安装开发工具 |
| 两份 G 代码示例、v4 原生模板、行映射与空跑转换功能 | 包含 | 模板很小，完整保留；工艺输出未接入 |
| STEPStudio 4.7.0 完整软件 | 自行补充 | `studio_root` 指向直接包含 `RobotController_PC` 的目录；不包含 300+ MB 安装包和完整软件树 |
| SR175-3200 仿真工作站 | 自行补充 | `simulation_seed` 指向同时包含 `home/`、`SRSystemFile/` 的已验证工作站；保留厂家授权与配置，按厂家许可迁移 |
| STEP C++ SDK x64 开发包 | 仅重新编译控制端时补充 | 使用 `SR.SDK_C++_X64_Win_V2.4.1.20250801`；目录至少含 `RobotCom.hpp`、`ControlMsgData.hpp`、`EasyTcpClient.hpp`、`Robot-SDK.lib` |
| Qt SDK、MSVC、CMake、Ninja | 仅源码构建时补充 | Qt **6.8.2 MSVC2022 x64**，MSVC 2022 x64，CMake 3.24+ |
| 相机图像、实际工具/工件坐标、实机 IP 与柜号 | 自行补充 | 不上传个人采样、运行历史、授权文件和现场设备身份 |

配置文件位于 `pc_control/workstation.paths.json`，示例见同目录 `.example.json`。配置路径可含空格；工作站只在隔离副本中运行，不修改原始 seed。也可设置 `STEP_STUDIO_ROOT`、`STEP_SIMULATION_SEED`、`STEP_DATA_ROOT` 环境变量覆盖配置。数据默认保存在 `%LOCALAPPDATA%\STEPROBOTICS\StepWorkstation`，起点配置使用 Qt 的用户配置目录，不写入 Git。

为兼容厂家运行时的路径长度限制，实际仿真工作副本单独保存在 `%TEMP%\STEPWS`，优先使用 Windows 短路径别名；不改原始 seed。可用 `STEP_SIMULATION_RUN_ROOT` 环境变量指定其他可写的短路径。普通实验归档仍存于上述用户数据目录。

没有仿真依赖时，程序仍可预览、转换、编辑和导出；点击仿真连接会明确提示缺少配置，不会偷偷降级为轨迹动画。实机仍按现有 SDK 只读连接和单独启停授权机制处理，点动与返回起点仅限核验过的本机仿真。

## 从源码构建

在 MSVC 2022 x64 开发终端中：

```powershell
cd workstation
.\build.ps1 -QtRoot 'C:\Qt\6.8.2\msvc2022_64' -SdkRoot 'D:\SDK\SR.SDK_C++_X64_Win_V2.4.1.20250801'
.\Start-Workstation.cmd
```

未提供 SDK 时可构建界面、转换器和测试，但不能重新构建控制端，`helix_pc.exe` 不会伪造为模拟实现。开发依赖不存入仓库。

`pc_control/README.md` 是详细操作说明，`pc_control/CONVERSION_GUIDE.md` 介绍转换边界。默认空跑，未接入激光、送丝和保护气输出。G 代码源行高亮需要控制器回读和同版本源行映射；STEPStudio 修改程序后需重新核对映射。回程末端速度默认 25 mm/s，上限 250 mm/s，并受手动配置进一步约束；这是上限而非恒速。

## 打包与验证

打包脚本：`packaging/package.ps1`；安装脚本：`packaging/StepWorkstation.iss`，使用 Inno Setup 6.7.4 或兼容版本。发布脚本检查最终 EXE 小于 **50,000,000 字节**，并生成 SHA-256。源码、配置示例及安装包只出现在独立分支，默认分支未修改。

测试入口：`ctest --test-dir build --output-on-failure`、`StepWorkstation.exe --smoke`。后者只做离线检查；`--home-smoke` 等仿真检查会启动独立厂家虚拟控制器，必须先配置 STEPStudio/seed 并确保专用端口 30316/30317 空闲。虚拟验证不能代替实机验收。

## 依赖来源与许可

- Qt 按动态 DLL 部署，许可和第三方声明随安装包提供在 `licenses/Qt-6.8.2/`，未对 Qt DLL 加密或静态嵌入上位机。[Qt 6.8 Windows 部署说明](https://doc.qt.io/qt-6.8/windows-deployment.html)。
- 对应 Qt 源码：[QtBase 6.8.2](https://github.com/qt/qtbase/tree/v6.8.2)，[Qt 6.8.2 完整源码归档](https://download.qt.io/archive/qt/6.8/6.8.2/single/)。本仓库包含上位机源码和构建说明，允许自行重编并替换动态依赖；第三方许可独立于本项目。
- STEPStudio、STEP SDK、工作站授权和机器人模型遵循厂家许可，开发包和授权文件不在此仓库分发。[厂家资料入口](https://www.stepelectric.com/Download/1773178589210951680-12-12.html)。
- MSVC 运行库来自微软 x64 运行时；需要补装或修复时使用[微软官方支持页](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。
