# R6.4 FTP 回读收尾修复

适用于已安装 8192 字节批量传输后，在回读长度等于文件长度时仍报告 `16#F223`、错误阶段 `220` 的排查。2026-11-18 现场回执显示：固定入口配置有效、空闲条件满足，首文件上传和回读均为 22858 字节，但没有得到最终回读完成确认。本次整组作业没有成功，不应加载运行。

## 更新位置

只更新 `FB_FileSocketIo` 的实现区。声明、其他 PLC 对象、10/11 号绑定和上位机不需要因此重配。完整实现见 `plc/FB_FileSocketIo/implementation.txt`，完整源见 `plc/FB_FileSocketIo.st`。

在 `SysSocket.SysSockRecv` 后，`ERR_SOCK_WOULDBLOCK` 分支与普通错误分支之间新增：

```iecst
        ELSIF (udiNativeResult = CmpErrors.Errors.ERR_SOCK_CLOSED) AND (xiCount = 0) THEN
            xPeerClosed := TRUE;
```

已安装的 `CmpErrors.h` 将 `ERR_SOCK_CLOSED` 定义为 `16#0211`，注释为套接字正常关闭。旧代码将它作为普通接收失败，模拟此返回方式可复现同样的 `F223 / 220`。原现场回执未保存该底层码，因此尚不能断言现场根因已被完整证实；本次修复需由现场编译和重传确认。

完整实现还将 `udiNativeResult := UDINT#0;` 移到 `IF xSend OR xReceive THEN` 内部，使停止调用收发后仍可在线查看最后一次底层返回码和 `xiCount`。

## 校验条件

只将零字节加 `ERR_SOCK_CLOSED` 识别为正常数据流结束；通用错误、超时、shutdown、负数或不合理字节数仍失败。父功能块继续要求完整长度、逐字节匹配或 CRC32、FTP 下载完成应答 226/250，以及连接清理全部成功。正常数据流结束本身不会放行 11 号。

本地执行最终 ST 的 18 项批量与直传回归通过，包含正常关闭、0 字节文件、8192 字节边界、22858 字节文件、短读、多余数据、内容错误、426 结束、其他底层错误和错误诊断保留；配套自动入口回归结果见 `validation/eof_auto_st.log`。这是离线执行，不是 InoProShop 编译或实机重传结果。

## 现场应用

1. 保持机器人停止、断使能，关闭失败的上传窗口。
2. 离线修改并保存 `FB_FileSocketIo` 实现，编译取得 0 错误后由操作者按现场流程更新 PLC。
3. 在文件通道连接已清理的条件下，普通写入 `GVL_FileBridge.xReset` 为 TRUE，再写 FALSE；不要强制就绪或修改内部状态号。
4. 确认 `GVL_FileBridge.aStatus[2]=0`，`aStatus[49]=1`；重新导入同一份已验证短作业再上传。
5. 若仍报 F223，保留新回执，并查看 `PRG_FileBridge.fbFtp.fbDataIo.udiNativeResult` 与 `PRG_FileBridge.fbFtp.fbDataIo.xiCount` 的实际值。这两个值用于区分正常关闭与其他接收故障。

必须等全部文件和整组确认通过、11 号可用后，再由操作者重新加载、确认加载、使能和运行。
