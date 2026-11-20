# STEP 打印作业台 · R6.4

当前源码为 2026-11-20 的 Qt/C++ 上位机：G 代码转换、PLC 控制、固定 10/11 号入口、PLC 批量转传与电脑 FTP 直传、微调/实验起点、三维姿态/规划轨迹，以及焊机和联动打印页面。完整状态及未完成项见[项目现状](../docs/项目现状-2026-11-20.md)。

`releases/` 内安装包是历史发布，不包含本次 R6.4 更新；本次提供源码及验证记录，未发布新安装包。现场安装版维持原运行目录，克隆仓库后请按以下步骤构建。

## 源码构建

依赖：Windows x64、CMake 3.24+、Ninja、Qt **6.8.2**（Widgets、Network、Test、OpenGLWidgets，含 private headers），以及与 Qt 套件匹配的 MinGW 13.1 或 MSVC 2022 x64。MSVC 需在开发终端运行；MinGW 需把对应编译器 `bin` 加入 PATH。本次仓库复验使用 Qt 6.8.2 / MinGW 13.1。

三维测试需要已取得使用许可的 `sr175.mesh`，不随公开仓库分发。先从本机保存的 R6.4 模型资源复制，脚本会核对 SHA-256：

```powershell
cd workstation
python .\pc_control\tools\prepare_assets.py --source 'D:\本机模型\sr175.mesh'
.\build.ps1 -QtRoot 'C:\Qt\6.8.2\mingw_64'
.\Start-Workstation.cmd
```

脚本配置 CMake、构建、运行完整 CTest，并部署到 `pc_control/native`。也可手动运行：

```powershell
cmake -S pc_control -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64
cmake --build build
ctest --test-dir build --output-on-failure
```

## 本机组件与配置

- PLC 控制、转换和两条 FTP 路线不依赖厂商 C++ SDK。SDK 直控/回程需要 `helix_pc.exe` 与配套 DLL；MSVC 构建时用 `-SdkDir` 指向含 `RobotCom.hpp` 和 `Robot-SDK.lib` 的 SDK 目录，或通过 `-SdkBridge` 提供本机已有桥程序，DLL 按许可自行部署。
- 模式切换组件源码在 `pc_control/mode_helper`。运行 `pc_control/tools/build_mode_helper.ps1 -Vendor 本机厂家DLL目录 -Output pc_control/native/mode_helper` 生成和测试；依赖厂商 C# SDK，不在仓库分发。
- 复制 `pc_control/workstation.local.ini.example` 为 EXE 同目录的 `workstation.local.ini`，在本机填写 `[ftp] password`。也可设置 `STEP_FTP_PASSWORD`，其优先级更高。上传密码默认遮挡，支持显示/隐藏，日常无需重复输入。配置文件被 Git 忽略。
- 模板 `SR175_3200_Helix_Ready_25mms_v4.zip` 保留在 `workstation`，启动器通过 `--control-dir` 正确定位模板、模型及桥组件。
- 数据路径见 `pc_control/workstation.paths.example.json`，可由 `STEP_DATA_ROOT` 覆盖。本机回执、运行历史和设备配置不随仓库分发。

## 文件上传与运行

上传窗口独立选择：上位机→PLC→控制柜，或上位机→控制柜。电脑直传使用 SDK 手册中的 FTP 协议，仍需 PLC 许可/回读和版本确认。顶部设备连接用于控制与姿态，上传窗口自行建立文件连接。

默认上传当前显示的 G 代码快照；换文件会同步更新原文、工程树与轨迹。日常只更新自动入口和轨迹程序，10 号手动服务、实验起点不被替换；随后执行登记、加载 11 号、确认加载、使能、运行。详见[当前上传说明](../docs/workflow_r6_4/电脑直传与当前G代码上传.md)与[现场流程](../docs/workflow_r6_4/使用与现场接入.md)。

当前焊机实物为 WSME-400-PLUS + ATR-EtherCAT，实际 PDO 与旧32字节适配不同；完整焊接联动未验收。见[新机型诊断](../docs/welder_wsme400plus_r1/README.md)。

## 验证与分发

本次验证记录见[2026-11-20](../docs/verification/2026-11-20/README.md)。CTest 使用回环模拟器和界面测试；ST 测试解释执行实际源码，均不连接设备。`pc_control/tests` 另含 ST 和模式组件测试。

旧 `packaging/` 脚本保留供历史发布追溯，不能据其旧版本号把 R6.4 标成已发布安装包。新版本发布前需核对模型/SDK许可、Qt运行库部署和独立机器运行。

Qt 动态链接，适用 Qt 及第三方许可；[Qt 6.8 部署说明](https://doc.qt.io/qt-6.8/windows-deployment.html)、[QtBase 6.8.2 源码](https://github.com/qt/qtbase/tree/v6.8.2)。STEPStudio、SDK 和机器人模型遵循厂家许可，授权文件不在仓库分发。
