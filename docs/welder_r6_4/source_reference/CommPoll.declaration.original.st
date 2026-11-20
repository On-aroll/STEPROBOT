(* ================================================================ *)
(* InoProShop 创建方式:                                              *)
(*   右键 Application → 添加对象 → 程序(POU)                        *)
(*   名称: CommPoll, 语言: ST, 类型: PROGRAM                        *)
(*   任务分配: MainTask (1ms周期)                                    *)
(*   说明: 焊机EtherCAT I/O 周期打包/解析                            *)
(*   依赖GVL: GVL_Welder, GVL_CNC                                  *)
(* ================================================================ *)
(*
 * 职责划分:
 *   CNC_Main.st  → 仅设置布尔标志 (g_wWeldOn, g_wRobotReady等)
 *   CommPoll.st  → 每周期: D300拆位→独立BOOL→组装g_byWeldOut
 *                    独立BOOL→组装D308+解析g_byWeldIn
 *
 * 字节映射依据: 奥太EtherCAT通讯协议 2024.7.1
 * PLC数组下标 = 协议字节号 (0-based)
 * ================================================================ *)

PROGRAM CommPoll
VAR
    nTempU16 : UINT;
    nTempI16 : INT;
    byTemp   : BYTE;
    wCtrlPacked : UINT;
    wPrevCtrlPacked : UINT := 16#FFFF;
    bBootInit : BOOL := TRUE;
    rPrevWireAuto : REAL;
    lrPrevWireRun : LREAL;
    bWireParamInit : BOOL := TRUE;
END_VAR
