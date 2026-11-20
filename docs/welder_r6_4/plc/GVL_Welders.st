(* Reused legacy EtherCAT PDO variables. No fixed addresses or retained process outputs.
   Old Dxxx comments describe the source only; PC uses HR7000 / HR7200. *)
VAR_GLOBAL
    (* EtherCAT输出 → 焊机输入数据区 (32字节) *)
    g_byWeldOut : ARRAY[0..31] OF BYTE;
    (* EtherCAT输入 ← 焊机输出数据区 (32字节) *)
    g_byWeldIn : ARRAY[0..31] OF BYTE;

    (*
     * EtherCAT 映射别名。
     * InoProShop 的 I/O 映射器要求变量是单一标识符，不接受数组下标；
     * CommPoll 在这些别名与协议数组之间逐周期同步，协议字节编号保持不变。
     *)
    g_byWeldOutMap01 : BYTE;
    g_byWeldOutMap02 : BYTE;
    g_byWeldOutMap03 : BYTE;
    g_byWeldOutMap04 : BYTE;
    g_byWeldOutMap05 : BYTE;
    g_byWeldOutMap06 : BYTE;
    g_byWeldOutMap07 : BYTE;
    g_byWeldOutMap08 : BYTE;
    g_byWeldOutMap09 : BYTE;
    g_byWeldOutMap10 : BYTE;
    g_byWeldOutMap11 : BYTE;
    g_byWeldOutMap12 : BYTE;
    g_byWeldOutMap13 : BYTE;
    g_byWeldOutMap14 : BYTE;
    g_byWeldOutMap15 : BYTE;
    g_byWeldOutMap16 : BYTE;
    g_byWeldOutMap17 : BYTE;
    g_byWeldOutMap18 : BYTE;
    g_byWeldOutMap19 : BYTE;
    g_byWeldOutMap20 : BYTE;
    g_byWeldOutMap21 : BYTE;
    g_byWeldOutMap22 : BYTE;
    g_byWeldOutMap23 : BYTE;
    g_byWeldOutMap24 : BYTE;
    g_byWeldOutMap25 : BYTE;
    g_byWeldOutMap26 : BYTE;
    g_byWeldOutMap27 : BYTE;
    g_byWeldOutMap28 : BYTE;
    g_byWeldOutMap29 : BYTE;
    g_byWeldOutMap30 : BYTE;
    g_byWeldOutMap31 : BYTE;
    g_byWeldOutMap32 : BYTE;

    g_byWeldInMap01 : BYTE;
    g_byWeldInMap02 : BYTE;
    g_byWeldInMap03 : BYTE;
    g_byWeldInMap04 : BYTE;
    g_byWeldInMap05 : BYTE;
    g_byWeldInMap06 : BYTE;
    g_byWeldInMap07 : BYTE;
    g_byWeldInMap08 : BYTE;
    g_byWeldInMap09 : BYTE;
    g_byWeldInMap10 : BYTE;
    g_byWeldInMap11 : BYTE;
    g_byWeldInMap12 : BYTE;
    g_byWeldInMap13 : BYTE;
    g_byWeldInMap14 : BYTE;
    g_byWeldInMap15 : BYTE;
    g_byWeldInMap16 : BYTE;
    g_byWeldInMap17 : BYTE;
    g_byWeldInMap18 : BYTE;
    g_byWeldInMap19 : BYTE;
    g_byWeldInMap20 : BYTE;
    g_byWeldInMap21 : BYTE;
    g_byWeldInMap22 : BYTE;
    g_byWeldInMap23 : BYTE;
    g_byWeldInMap24 : BYTE;
    g_byWeldInMap25 : BYTE;
    g_byWeldInMap26 : BYTE;
    g_byWeldInMap27 : BYTE;
    g_byWeldInMap28 : BYTE;
    g_byWeldInMap29 : BYTE;
    g_byWeldInMap30 : BYTE;
    g_byWeldInMap31 : BYTE;
    g_byWeldInMap32 : BYTE;

	(* D520 | 激光距离 (原始INT值, 从%IW50读取) *)
	(* --- 激光距离原始值 --- *)
	g_iLaserDistanceRaw : INT;
END_VAR

VAR_GLOBAL


(* D300 | 控制标志位打包字 (PC通过Modbus读写, CommPoll每周期双向映射) *)
g_wWeldCtrlWord : UINT := 0;

(* D300 bit0 | 独立BOOL | 开始焊接 1=起弧 0=停弧 [危险, 高有效] *)
g_wWeldOn : BOOL := FALSE;

(* D300 bit1 | 独立BOOL | 机器人准备好, 高有效 *)
g_wRobotReady : BOOL := FALSE;
(* D300 bit2 | 独立BOOL | 检气, 高有效 *)
g_wGasCheck : BOOL := FALSE;

(* D300 bit3 | 独立BOOL | 热丝电源使能 1=ON 0=OFF *)
g_bHotWireEnable : BOOL := FALSE;

(* D300 bit4 | 独立BOOL | 热丝工作模式 0=DC 1=AC *)
g_bHotWireACMode : BOOL := FALSE;

(* D300 bit7 | 独立BOOL | 高频有无 0=高频 1=提升 *)
g_bHighFreqDisable : BOOL := FALSE;

(* D300 bit8 | 独立BOOL | 焊枪模式 0=水冷 1=气冷 *)
g_bGunCoolMode : BOOL := FALSE;


(* D302 | 协议Byte0 bit3-4 | 焊机操作模式 0=两步 1=四步 2=反复 *)
g_wWeldOperMode : UINT := 0;

(* D304 | 协议Byte0 bit5-7 | 焊机工作模式 0=直流恒流 1=直流脉冲 2=交流恒流 *)
(*                           3=交流脉冲 4=直流手弧焊 *)
g_wWeldWorkMode : UINT := 1;

(* D306 | 协议Byte11 | 通讯异常标志, 控制器填充, 默认0 *)
g_wCommErrorFlag : BYTE;

(* D414 | 协议Byte2 bit0-7 | 热丝频率, 范围20~100, 对应20~100Hz (直通) *)
g_wHotWireFreq : UINT := 50;


(* D416 | 协议Byte3 bit0-7 | 热丝电流给定, 范围0~200, 对应0~200A (直通) *)
g_wHotWireCurrent : UINT := 150;


(* D400 | 协议Byte4-5 | 给定电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围40~4000, 对应4.0~400.0A), 占D400-D401 *)
g_wWeldCurrent : REAL := 100.0;

(* Local wire-feed axis parameters. Speeds are mm/s; delays are seconds.
   D412 and D1202 set the same automatic speed; a changed D1202 has priority.
   D410 > 0 overrides D2104 for axis 4 jog; zero uses D2104 for compatibility. *)
g_rWireStartDelay : REAL := 0.0;
g_rWireStopDelay : REAL := 0.0;
g_rWireJogSpeed : REAL := 0.0;
g_rWireAutoSpeed : REAL := 0.0;


(* D420 | 协议Byte13-14 | 前气时间, 实际s, 例: 5.0表示5.00s *)
(* CommPoll内部 ×100 → 协议值(范围1~999, 对应0.01~9.99s), 占D420-D421 *)
g_wFrontGasTime : REAL := 0.01;

(* D422 | 协议Byte15-16 | 延气时间, 实际s, 例: 3.0表示3.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~600, 对应0.1~60.0s), 占D422-D423 *)
g_wTailGasTime : REAL := 0.2;

(* D424 | 协议Byte17 bit0-7 | 爬升时间, 实际s, 例: 5.0表示5.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~100, 对应0.1~10.0s), 占D424-D425 *)
g_wRampUpTime : REAL := 0.5;


(* D426 | 协议Byte18 bit0-7 | 衰减时间, 实际s, 例: 5.0表示5.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~150, 对应0.1~15.0s) *)
g_wRampDownTime : REAL := 0.2;

(* D428 | 协议Byte19 bit0-7 | 脉冲频率, 实际Hz, 例: 5.0表示5.0Hz *)
(* CommPoll内部 ×10 → 协议值(范围2~200, 对应0.2~20.0Hz), 占D428-D429 *)
g_wPulseFreq : REAL := 0.5;

(* D430 | 协议Byte20+22 bit4-7 | 占空比, 实际百分比, 例: 50.0表示50.0% *)
(* CommPoll内部 ×10 → 协议值(范围1~999, 对应0.1%~99.9%), 占D430-D431 *)
g_wDutyCycle : REAL := 50.0;

(* D432 | 协议Byte21+22 bit0-3 | 峰值电流, 实际A, 例: 150.0表示150.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D432-D433 *)
g_wPeakCurrent : REAL := 100.0;


(* D434 | 协议Byte23-24 | 基值电流, 实际A, 例: 30.0表示30.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D434-D435 *)
g_wBaseCurrent : REAL := 30.0;

(* D436 | 协议Byte25-26 | 起弧电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D436-D437 *)
(* --- 协议 Byte 25-26: 起弧电流 (2字节小端, 低字节在前) --- *)
g_wArcStartCurrent : REAL := 5.0;

(* D438 | 协议Byte27-28 | 收弧电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D438-D439 *)
(* --- 协议 Byte 27-28: 收弧电流 (2字节小端, 低字节在前) --- *)
g_wArcEndCurrent : REAL := 5.0;

(* D440 | 协议Byte29 bit0-7 | 交流频率, 范围40~250, 对应40~250Hz (直通) *)
(* --- 协议 Byte 29: 交流频率 --- *)
g_wACFreq : UINT := 40;

(* D442 | 协议Byte30 bit0-7 | 清理比例, 范围90~180, 对应40~-50 *)
(* 协议转换: Byte30 = -1 × D442 + 130 *)
(* 例: D442=90 → Byte30=40, D442=130 → Byte30=0, D442=180 → Byte30=-50 *)
(* --- 协议 Byte 30: 清理比例 --- *)
g_wCleanRatio : UINT := 90;

(* D444 | 协议Byte31 bit0-7 | 交流偏置, 范围90~180, 对应40~-50 *)
(* 协议转换: Byte31 = -1 × D444 + 130 *)
(* 例: D444=90 → Byte31=40, D444=130 → Byte31=0, D444=180 → Byte31=-50 *)
(* --- 协议 Byte 31: 交流偏置 --- *)
g_wACBias : UINT := 90;



(* D308 | 输入状态打包字 (PC通过Modbus读取, CommPoll每周期从独立BOOL组装) *)
g_wWeldStatusWord : UINT;

(* D308 bit0 | 独立BOOL | 引弧成功, 高有效 *)
g_bWeldArcOK : BOOL;

(* D308 bit1 | 独立BOOL | 焊机准备好, 高有效 *)
g_bWeldReady : BOOL;

(* D308 bit2 | 独立BOOL | 电流有无, 高有效 *)
g_bWeldHasCurrent : BOOL;

(* D308 bit3 | 独立BOOL | 焊枪开关异常, 高有效 *)
g_bGunSwitchFault : BOOL;

(* D308 bit4 | 独立BOOL | 焊机热保护, 高有效 *)
g_bWeldThermalProtect : BOOL;

(* D308 bit5 | 独立BOOL | 缺水保护, 高有效 *)
g_bWaterProtect : BOOL;

(* D308 bit6 | 独立BOOL | 热丝电源热保护, 高有效 *)
g_bWirePSThermal : BOOL;

(* D308 bit7 | 独立BOOL | 热丝电源通讯保护, 高有效, 控制器填充 *)
g_bWirePSCommFault : BOOL;

(* D308 bit8 | 独立BOOL | 组合报警, 包含焊枪开关/热保护/缺水/热丝电源故障 *)
g_bWeldFault : BOOL;


(* D500 | 焊机输入状态字 (16位打包, 详见位域定义) *)
g_wWeldInStatus : UINT;

(* D502-D503 | 协议输入Byte4-5 | 实际焊接电压, 0.1V分辨率 *)
(* --- 协议输入 Byte 4-5: 实际焊接电压 (2字节小端, 0.1V分辨率) --- *)
(* 范围: 0~1000 → 0~100.0V *)
g_rWeldVoltageAct : REAL;

(* D506-D507 | 协议输入Byte6-7 | 实际焊接电流, 0.1A分辨率 *)
(* --- 协议输入 Byte 6-7: 实际焊接电流 (2字节小端, 0.1A分辨率) --- *)
(* 范围: 0~400 → 0~400A *)
g_rWeldCurrentAct : REAL;

(* D510-D511 | 协议输入Byte8-9 | 实际热丝焊接电流, 10A分辨率 *)
(* --- 协议输入 Byte 8-9: 实际热丝焊接电流 (2字节小端, 10A分辨率) --- *)
(* 范围: 0~20 → 0~200A *)
g_rHotWireCurrentAct : REAL;

(* D518 | 协议输入Byte1原始值 | 报警码, 报警时记录协议输入Byte1原始值 *)
(* --- 报警原始字节 --- *)
g_rWeldFaultCode : UINT;



(* D522 | 激光距离 (实际距离mm, 原始值/100+50) *)
(* --- 激光距离实际值 --- *)
g_rLaserDistance : REAL := 50.0;


(* ================================================================= *)
(* 协议输入预留字节 (协议保留, 当前未使用)                               *)
(*   协议输入 Byte 2-3: 预留                                          *)
(*   协议输入 Byte 12:  预留                                          *)
(* ================================================================= *)

END_VAR
