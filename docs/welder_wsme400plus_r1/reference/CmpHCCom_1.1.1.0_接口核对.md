# CmpHCCom 1.1.1.0 安装库接口核对

核对日期：2026-11-19。依据本机已经安装的 InoProShop 文件，只读提取编译库中内嵌的帮助，不含库实现代码。

来源：

- `C:/Inovance Control/InoProShop/CODESYS/Common/AM600 Data/ExtensionToolBoxItems.xml` 的「EtherCAT通信」分类：`ETC_COE_SdoRead`，FunctionBlock，Library=`CmpHCCom`，Description=`EtherCAT从站SDO读取`。同分类的通用 `ETC_CO_SdoRead4` 项位于 XML 注释内。
- `C:/Inovance Control/InoProShop/CODESYS/Repository/Managed Libraries/Inovance/CmpHCCom/1.1.1.0/CmpHCCom.compiled-library-ge33`。
- 库内 `__lmd__.auxiliary` 为 ZIP 文档包，其中 `info.html` 确认 DefaultNamespace=`CmpHCCom`、版本 1.1.1.0、`inhibit_onlinechange=True`。
- 同文档包 `lXe3qprre_I_ST-oEBn98Ub1OdQ/etc-coe-sdoread.html` 确认如下接口。

`FUNCTION_BLOCK ETC_COE_SdoRead`

| 方向 | 名称 | 类型 | 文档初值/说明 |
|---|---|---|---|
| Input | xExecute | BOOL | |
| Input | xAbort | BOOL | FALSE |
| Input | usiCom | USINT | 1；第一个 ETC 主站索引为 1 |
| Input | byAddressMode | BYTE | 0；文档未解释枚举含义 |
| Input | uiDevice | UINT | |
| Input | wIndex | WORD | |
| Input | bySubindex | BYTE | |
| Input | udiTimeOut | UDINT | 3000 |
| Input | ReadBuffer | __SYSTEM.AnyType | |
| Input | szSize | CAA.SIZE | 0 |
| Output | xDone | BOOL | |
| Output | xBusy | BOOL | |
| Output | xError | BOOL | |
| Output | eError | ETC_CO_ERROR | |
| Output | udiSdoAbortCode | UDINT | |
| Output | szDataRead | CAA.SIZE | |

后续现场 C0032 错误明确确认：当前 AC802 目标将 `szDataRead` 解析为 ULINT。因此诊断长度记录变量使用 ULINT，不能隐式赋给 UDINT。此结论来自现场编译器，补充了通用库文档中的 CAA.SIZE 别名信息。

编译库接口元数据还为 ReadBuffer 标记 `Input, NoCopy`，约束支持 BYTE/WORD/DWORD/LWORD、整数标量及相应元素数组。采用程序静态变量 `ARRAY[0..3] OF BYTE`，在整个异步请求期间保留同一接收区。

同机 1.1.0.1 的内置帮助没有 `byAddressMode` 参数；1.0.0.0/1.1.0.0 的浏览器缓存没有此读取块。本次交付明确针对 1.1.1.0，不将不同版本签名混用。

这些文件证明工具箱选择与公开接口，不能证明当前 PLC 运行时上的调用已成功，也不能据此确定现场超时根因。当前没有新版原生编译、下载或实机读取结果。

## 后续手册与现场补充

本机 `C:/Inovance Control/InoProShop/CODESYS/Online Help/zh-CHS/Command Manual.pdf`，6.5.2 节（印刷页 1092–1096）解释：`byAddressMode=0` 为顺序模式，地址例如 1001；1 为别名模式，需启用别名。AC800 的 `usiCom=1` 对应 EtherCAT C 口，2 对应 D 口。当前使用 C 口、顺序地址 1001 的参数得到手册支持。

现场随后已能执行 CmpHCCom 版本读取，`xReadBusySeen=TRUE`，但首项 `0x1018:01` 仍无 Done/Error，外层超时。上述“没有新版实机读取结果”为接口交付时的历史状态，现由此补充；完整映射仍未读出。
