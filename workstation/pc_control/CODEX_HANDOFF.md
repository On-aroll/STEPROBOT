# CODEX 交接：--sdk-smoke sendrun DYN rc=-1 修复（2026-11-13）

## 现状：根因已定位，修复已实测验证，只差一行代码落地

- 目标流程（直发）：GUI 导入 G 代码 → helix_pc 转 SDK 流 → `SDKSimulateProgramLoad/SetTool/SetRefSys/DYN/LIN/EOF/ProgramSend` 直发控制器 → 运行滚动显示。
- 全链路只剩最后一环：sendrun 阶段 `SDKSimulateDynProCmd` 返回 -1（msgid=0，客户端本地拒绝，与数值/行号无关——同配置在新启动的控制器上 rc=0）。
- 根因（探针 4 实测）：captureAndLoad 结束后文本程序 helix_25mms 处于已加载态，SDKSimulate 流式会话被污染（Load=-1 或 Load=0/DYN=-1 两种表现都出现过）。**先 `ProgramKillCmd` 卸载该文本程序再开始流式，DYN rc=0，修复成功。**
- 探针 4 关键输出：
  ```
  helix_load rc=0 program=helix_25mms
  == A real-state stream ==  load rc=-1 → P4_real_dyn rc=1000（未修）
  == B unload then stream == unload rc=0 → load rc=0 → P4_unload_fix rc=0 program=SDK（修复成功）
  ```

## 唯一需要的代码改动

文件：`D:\Desktop\STEPROBOTICS\workstation\pc_control\helix_pc.cpp`（以此为准；`D:\Desktop\STEPHelixWorkstation\pc_control\` 下如有源码副本是旧的，不要在那边改）

位置：`sendSdkProgram`（约 787 行），在 gate（`atRest && getProgramState()==eStop && getMessageData().m_MessageType!=eError`）通过之后、`SDKSimulateProgramLoad(true)` 之前插入：

```cpp
const auto loaded = client.getProgramName();
if (!loaded.empty()) {
    client.ProgramKillCmd(job.project, loaded, true);
    std::this_thread::sleep_for(600ms);
}
```

其余修复（DYN 关节组补值、capture START 竞态等待、prepare busy 恢复、fail 消息诊断）本段已完成，勿动。

## 构建

```
cmd //c "C:\Users\Bon-Voyage\AppData\Local\Temp\claude\run-build.cmd"
```

成功标志：输出含 `BUILD_SCRIPT_OK`。

## 部署规则（极易踩坑，上一轮就栽在这）

GUI（StepWorkstation.exe 在 `pc_control\native\`）从 **pc_control\ 父目录** 启动 helix_pc.exe（workstation.cpp:2025 用 applicationDirPath()/..）。所以必须：

```
copy D:\Desktop\STEPROBOTICS\workstation\build\helix_pc.exe D:\Desktop\STEPHelixWorkstation\pc_control\helix_pc.exe
```

只拷到 `native\` 无效。

## 测试

```
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\Bon-Voyage\AppData\Local\Temp\claude\run-sdk-smoke.ps1"
```

后台运行，等任务输出里的 `EXIT=` 行，期望 **EXIT=0**。日志看 panel.log 和 %TEMP%\STEPWS\qt-messages.log（GBK 输出用 `iconv -f GBK -t UTF-8`）。

通过后回归：把 ps1 里的 flag 依次改成 `--sim-smoke` / `--smoke` / `--manual-smoke` / `--home-smoke` 各跑一遍，全部期望 EXIT=0。

## 已知坑（自测时注意）

- 虚拟控制器每次启动只容忍约 3 次 SDKSimulate 会话重建（第 4 次 tool/refsys 开始 -1）；需要更多次就重启 1-NonReal.exe + 2-RobotMoveControl.exe。
- 文本加载与 simulate 会话有顺序敏感性：simulate 会话先跑会破坏后续 `ProgramLoadCmd`。
- GUI 超时退出有堆损坏（EXIT=-1073741819 但日志完整）：exit-0 路径先 session.disconnect()，大概率不复现；若复现不阻塞主流程，先放着。
- 手动启动虚拟控制器（探针用）：`STEPStudio 4.7.0\STEPStudio 4.7.0\RobotController_PC\Environment` 下 `1-NonReal.exe <station> 4` → 2.5s → `2-RobotMoveControl.exe <station> 4` → 3.5s；station 在 %TEMP%\STEPWS\cpp_*\station。
- 探针源码/构建批处理：`C:\Users\Bon-Voyage\AppData\Local\Temp\claude\probe\`（probe_dyn4.cpp 为最终版）。

## 收尾（EXIT=0 之后）

- 同步源码 `D:\Desktop\STEPROBOTICS\workstation\pc_control\` → `D:\Desktop\STEPHelixWorkstation\pc_control\`（若那边保留源码副本）。
- README/文档把「STEPStudio 传输」相关措辞改为直发流程（导入 G 代码 → 自动转换 → 直发 → 滚动显示）。
- 实机柜验证仍需用户上柜（虚拟控制器已全绿为准）。

## Codex 接手结果（2026-11-13，以上单行修复判断已由完整运行验证修正）

- 仅卸载旧程序不足以修复完整流程。`SDKSimulate*ProCmd` 返回 0 只证明本地缓冲构造成功；实际接收还须检查 `SDKSimulateProgramSend`。
- 隔离探针验证：SDK 任务停在 `state=2,line=-1` 时发送，即使只有 EOF 也被拒收；先 START 空 SDK 任务再发送，同一组 DYN/LIN/EOF 返回 0 并回读到运动。探针记录位于 `D:\Desktop\STEPROBOTICS\reports\codex-motion-fix-20261113\protocol-probe\`。
- 正式流程已改成：完整解析校验 → 核对静止/工具 → 卸载旧程序 → 自动模式 → 装载并确认 SDK 任务 → START → 按 `MAX_PRO_NUM=10` 分批发送 → EOF。停止检查覆盖发送过程和 START 之前，失败时请求 STOP/MSTOP 并卸载流。
- 补齐 SDKOverlap 的有效类型；重复使用旧 F 值时重新发送 DYN；使用独立临时文件及 UTF-8/带引号的路径，避免多个工作台共用一个指令文件。
- SDK 实际程序名为 SDK，界面按本次发送内容绑定源行，原点取控制端确认值；完成需要观察到末段运行、停止与实际终点，兼容正常 EOF 后行号变成 -1。
- Session 停止会清除排队操作，等待 STOP/ESTOP 确认期间拒绝新操作。新增进程管道回归测试：原 Session 复现 12 项失败，修复后通过。析构前断开进程回调，避免反馈缓冲销毁后回调访问。
- 四项 CTest 及 `--sdk-smoke`、`--smoke`、`--sim-smoke`、`--manual-smoke`、`--home-smoke` 均通过。SDK 测试包含短路径、F 值恢复、重复/分批圆弧以及运动中 STOP+MSTOP；记录仍标为 simulation。
- 补齐已知程序在外部自动模式下的启动按钮条件，与 SDK 后端的自动模式判断一致；实际柜端模式仍待现场验证。
- 最终测试清单位于 `D:\Desktop\STEPROBOTICS\reports\codex-motion-fix-20261113\release-smoke-results.json`。本轮未连接或控制实机，实机的柜型、固件、手动参数和现场运动/停止仍需验收。
- 源码主目录仍为 `D:\Desktop\STEPROBOTICS\workstation\pc_control`。当前桌面入口为 `D:\Desktop\STEPHelixWorkstation\Start-Workstation.cmd`，控制端实际路径仍为其 `pc_control\helix_pc.exe`。

## 实机只读型号兼容修复（2026-11-13 15:07）

- 真实控制器返回 `SR175/3200 / SRC4L6`；原判断只接受 `SR175-3200`，误将相同型号的官方名称变体限制为只读。现明确接受这两个名称，其他型号仍只读。
- 已部署父目录和 native 目录中的 helix_pc.exe；5 项 CTest 通过，含型号白名单回归。
- 15:07:27 通过原工作台重连真实控制器，界面显示“实机已连接”，日志显示“已授权”；手动/自动按钮可用。伺服仍未使能、TCP 0 mm/s，未发送使能或运动指令。此结果仅证明身份识别和连接限制解除，实机运动尚未验收。
- 记录：`D:\Desktop\STEPHelixWorkstation\MODEL_ALIAS_FIX_20261113.json`。上文虚拟运动回归对应型号修复前的 helper，未冒称本次重新完成实机运动测试。

## 程序加载与模式切换（2026-11-13，截图问题后续）

- “柜内当前程序与左侧不一致”是上位机的名称核对提示；截图中柜内程序名为空，不能据此认定加载冲突。厂家 SPad V1.8.1 手册的程序管理章节确认同一时间只加载一个程序，更换需先关闭旧程序。
- 实机作业匹配不再写死旧螺旋名称。导入有效新作业时，仅在确认停稳且没有其他操作时请求卸载旧程序；新增“加载当前程序”和“卸载程序”。加载先确认卸载完成，再核对新工程/程序回读，不使能、不启动；不同本地作业与柜内程序禁止误启动。
- SDK ProgramLoadCmd 使用无 `.sr` 后缀的工程名；getter 返回 `.sr`，现统一处理。首次虚拟回归曾以 `.sr` 直接加载失败，规范名称后同一流程通过，失败记录保留在 reports/codex-program-lifecycle-20261113。
- 模式按钮只发模式请求，保留加载程序，不改倍率、不请求使能。新增独立“使能伺服”。失败记录包含 SDK 返回码、失败阶段、实际模式、使能状态、程序名和消息号；界面保留具体原因，通用 refused 不覆盖它。实机此前只有 refused，无法追溯具体是哪一步，不能声称已确定为柜内程序导致。
- 6 项 CTest 通过；controller_program_modes 对真实控制操作模板做 26 项断言，含带加载程序切模式、禁用隐式使能、卸载失败禁止加载、空回读/语法错误及诊断信息。
- 最终二进制通过厂家隔离虚拟控制器的 --control-smoke、--sdk-smoke、--sim-smoke；覆盖导入自动卸载、显式加载 capture_home、工程名匹配、带程序手动/自动切换、直发分批运动/终点/停止、原生程序与多坐标点动。
- 原生 ZIP 导入电脑不等于传入控制柜。SDK 公共接口未提供原生 ZIP 文件导入，仍用 STEPStudio/U 盘；“加载当前程序”只加载柜内已有文件。G 代码保持直发流程。
- 本次不对实机执行卸载、模式切换、使能或运动；部署重启只重连读取身份和状态。实机手动请求是否被柜面/示教器模式选择允许，仍须现场看新版回读，不能绕过硬件控制条件。
