# cpp-starter —— 已在本机实测通过的 C++ 最小工程

> **2026-09-29 恢复验收：** 本工程已重新编译链接通过，仅验证构建，未重新连接控制器。当前仿真研究请优先使用 [C# 上位机](../../实验准备/上位机/快速开始.md) 的本机只读入口；本探针源码仍保留旧的实体控制器目标地址，以下连接记录属于历史结果。

这不是给你读的框架，是**已经编译并链接成功、已经运行过**的探针程序。它证明了一件事：
你这台电脑的 MSVC 工具链和厂商静态库 `Robot-SDK.lib` 是**能接上的**，剩下的问题只在于机器人本身通不通。

## 一、怎么编译（一条命令）

双击或在终端里运行：

```
research\cpp-starter\build_msvc.bat
```

它做的事情等价于：

```
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /W3 /EHsc /std:c++17 /MD ^
   /I "<SDK目录>" ..\src\main.cpp /Fe:robot_probe.exe /Foobj\ ^
   /link "<SDK目录>\Robot-SDK.lib" ws2_32.lib
```

产物：`research\cpp-starter\build\robot_probe.exe`（约 700 KB）

## 二、三个必须照做的编译条件（都是实测踩出来的）

| 条件 | 为什么 |
|---|---|
| **`/MD`（动态运行库 Release）** | 不加就报 `LNK2038: 检测到"RuntimeLibrary"的不匹配项: 值"MD_DynamicRelease"不匹配值"MT_StaticRelease"`，紧跟着一堆 `LNK2005 已经在 libcpmt.lib 中定义`。厂商库是用 **/MD** 编的，你也必须用 /MD。Debug 版对应的 `Robot-SDKd.lib` 才用 `/MDd`。 |
| **`main.cpp` 存成 UTF-8 带 BOM** | 带 BOM 后 MSVC 按 UTF-8 读我们自己的文件（中文注释正常），而 SDK 那三个头文件是 GBK 无 BOM，仍按系统 ANSI 代码页读——两边都对。**不要给整个工程加 `/utf-8`**，那会把 GBK 头文件的中文注释读成非法字节。 |
| **必须链接 `ws2_32.lib`** | SDK 内部走 Winsock（`EasyTcpClient.hpp` 里有 `#pragma comment(lib,"ws2_32.lib")`，但静态库场景下自己写上更稳）。 |

另外会看到几条 `warning C4251`（`m_ip` / `stopRequested` / `active_buffer` 需要 dll 接口）——**厂商头文件自身的问题，忽略即可**，不影响链接和运行。

## 三、已经验证过的运行结果（机器人未上电，这是预期）

```
STEP ROBOT C++ starter | mode=probe | target=192.168.39.220:30312
ERROR: SOCKET 连接失败:...          ← 厂商库内部打印的中文（GBK）
Task timed out!                     ← 厂商库内部超时提示
[init              ] ret=-4
连接失败排查顺序: ...
done, exit=1
```

两个信息量很大的点：

1. **`init` 返回 `-4`，而说明书只写了 0 / -1 / -2 / -3。** 也就是说文档的返回值表不全，`-4` 实测对应"socket 连不上"（本例里机器人没上电、也没接网线）。以后遇到陌生返回码，别假设文档全，实测为准。
2. **`init()` 会阻塞好几秒才失败**，不是立即返回。将来做上位机控制循环时，连接检测不能放在实时性敏感的循环里。

## 四、三种运行模式（默认那个绝不动机器）

```
robot_probe.exe probe     只读：连接状态、SDK版本、使能状态、模式、关节/笛卡尔位置。机械臂不会动。
robot_probe.exe run       加载示教器上已有的程序并启动，会要求你输入大写 YES 二次确认，并先把倍率压到 10%。
robot_probe.exe jog       绝对位置点动到 main.cpp 里的 TARGET_J，需要三次确认。
```

要改工程名/程序名/工具名/坐标系名，都在 `src\main.cpp` 顶部的"配置区"，那几个名字**必须先在示教器里存在**，否则加载会失败并返回错误行号。

## 五、等机器人固定好、上电之后的验收顺序

1. PC 网线直连控制器，PC 设静态 IP `192.168.39.x`；
2. `Test-NetConnection 192.168.39.220 -Port 30312` 先看 TCP 通不通（跟 SDK 无关的第一步）；
3. 跑 `robot_probe.exe probe`，此时应能看到 SDK 版本号、当前工程名/程序名、关节角和 `getCartPosWorld()` 的真实读数——**把这些读数抄下来，它们是后面校准你自己仿真的基准数据**；
4. 再谈 `run` / `jog`。
