(* EtherCAT -9 welder: 32-byte RxPDO/TxPDO use symbolic channel mappings; no fixed IEC addresses. *)
(* ================================================================ *)
(* InoProShop 创建方式:                                              *)
(*   右键 Application → 添加对象 → 全局变量表(GVL)                    *)
(*   名称: GVL_Welders                                               *)
(*   说明: 奥太焊机 EtherCAT 通讯接口全局变量                         *)
(*   依赖: 无                                                        *)
(*   修订: 布尔变量使用独立%MW地址 (D300控制=%MW101~109)               *)
(*         (D308状态=%MW111~119), InoProShop不支持BOOL位打包         *)
(*         新增 %MW300/%MW308 作为Modbus从站打包字映射 (方案C)         *)
(* ================================================================ *)
(*
 * EtherCAT通讯协议 (奥太 2024.7.1)
 *
 * EtherCAT输出 (PLC→焊机): g_byWeldOut[0..31] → 地址-9焊机RxPDO (32字节)
 *   协议字节0~31 = PLC数组下标0~31 (一一对应)
 *
 * EtherCAT输入 (焊机→PLC): g_byWeldIn[0..31] ← 地址-9焊机TxPDO (32字节)
 *   协议字节0~31 = PLC数组下标0~31 (一一对应)
 *
 * 数据流:
 *   PC(Modbus写D300) → g_wWeldCtrlWord (打包字)
 *       → CommPoll.st拆位 → g_wWeldOn等独立BOOL
 *       → CommPoll.st组装 → g_byWeldOut → EtherCAT → 焊机
 *   焊机 → EtherCAT → g_byWeldIn
 *       → CommPoll.st解析 → g_bWeldArcOK等独立BOOL
 *       → CommPoll.st组装 → g_wWeldStatusWord → PC(Modbus读D308)
 *
 * Modbus寄存器布局:
 *   D300 (%MW300): 焊机控制输出标志位 (16位打包字, CommPoll双向映射)
 *     独立BOOL: g_wWeldOn(%MW101) ~ g_bGunCoolMode(%MW109)
 *   D302      : 焊机操作模式 (UINT)
 *   D304      : 焊机工作模式 (UINT)
 *   D306      : 通讯异常标志 (BYTE, 控制器填充)
 *   D308 (%MW308): 焊机输入状态标志位 (16位打包字, CommPoll组装)
 *     独立BOOL: g_bWeldArcOK(%MW111) ~ g_bWeldFault(%MW119)
 *   D400-D444 : 焊接参数 (各参数见注释)
 *   D500      : 焊机输入状态字 (16位打包, CommPoll从独立BOOL组装)
 *   D502-D518 : 焊机输入反馈值
 *
 * 注意: Modbus从站配置需包含 D300-D306 和 D308 映射范围
 *       D300=%MW300, D308=%MW308 均为打包字, PC按位操作
 * ================================================================ *)

(* ================================================================= *)
(* 焊机控制输出标志位 → 协议 Byte 0 / Byte 1 位域                     *)
(* Modbus: D300 (%MW300, 16位), bit0~8 对应9个布尔量                  *)
(* ================================================================= *)


(*
 * 协议 Byte 0 位域:
 *   bit0(1):  开始焊接       1=起弧 0=停弧 [危险, 高有效]
 *   bit1(2):  机器人准备好   上位机接收到焊机数据, 通讯正常, 高有效
 *   bit2(3):  检气           高有效
 *   bit3-4:   焊机操作模式   00=两步 01=四步 10=反复
 *   bit5-7:   焊机工作模式   000=直流恒流 001=直流脉冲 010=交流恒流
 *             011=交流脉冲 100=直流手弧焊
 *
 * 协议 Byte 1 位域:
 *   bit0(9):  热丝电源使能   1=ON 0=OFF
 *   bit1(10): 热丝工作模式   0=DC 1=AC
 *   bit2(11): 备用
 *   bit3(12): 点动送丝       高有效, 丝检时使用, 电平信号
 *   bit4(13): 点动抽丝       高有效, 丝检时使用, 电平信号
 *   bit5(14): 备用
 *   bit6(15): 高频有无       0=高频 1=提升
 *   bit7(16): 焊枪模式       0=水冷 1=气冷
 *)

(* --- 协议 Byte 11: 通讯异常标志 (控制器填充) --- *)
(*
 * 协议 Byte 11 位域:
 *   bit0(89):  机器人→CAN控制器通信异常 (控制器填充, 焊机显示E84)
 *             注: 500ms内无机器人准备好信号将触发此异常
 *   bit1-3:    预留
 *   bit4(93):  焊机→CAN控制器通信异常 (控制器填充, 焊机显示E85)
 *   bit5-7:    预留
 *)


(* ================================================================= *)
(* 焊接参数 Modbus 接收 (D400~D444)                                  *)
(* PC写入Modbus寄存器 → CommPoll.st读取并组装到g_byWeldOut             *)
//

(* --- 协议 Byte 20-22: 占空比 + 峰值电流 (交叉编排) --- *)
(*
 * 占空比: Byte20=低字节, Byte22 bit4-7=高4位 (12位)
 * 峰值电流: Byte21=低字节, Byte22 bit0-3=高4位 (12位)
 * g_wDutyCycle为REAL, 存实际百分比(如50.0=50.0%, 3.5=3.5%)
 * CommPoll内部 ×10 → 协议值1~999 → 0.1%~99.9%
 * 峰值电流 = [Byte22 bit3:0][Byte21]   范围50~4000 → 5.0~400.0A
 *)

 (* ================================================================= *)
(* 焊机输入状态标志位 → 协议输入 Byte 0 / Byte 1 位域                  *)
(* Modbus: D308 (%MW308, 16位), bit0~8 对应9个布尔量                  *)
(* 由 CommPoll.st 每周期从 g_byWeldIn 解析写入                        *)
(* ================================================================= *)
(*
 * 协议输入 Byte 0:
 *   bit0: 引弧成功     高有效
 *   bit1: 焊机准备好   高有效
 *   bit2: 预留
 *   bit3: 电流有无     高有效
 *   bit4-7: 预留
 *
 * 协议输入 Byte 1:
 *   bit0: 预留
 *   bit1: 焊枪开关异常   高有效
 *   bit2: 焊机热保护     高有效
 *   bit3: 缺水保护       高有效
 *   bit4: 热丝电源热保护 高有效
 *   bit5: 热丝电源通讯保护 高有效 (控制器填充)
 *   bit6-7: 预留
 * ================================================================= *)

 (* ================================================================= *)
(* 输入状态字 (打包, PC通过Modbus D500读取)                            *)
(* 由 CommPoll.st 每周期从各布尔标志位组装写入                          *)
(* ================================================================= *)
(*
 * g_wWeldInStatus 位域 (CommPoll从独立BOOL组装):
 *   bit0:  引弧成功         (g_bWeldArcOK)
 *   bit1:  焊机准备好       (g_bWeldReady)
 *   bit2:  焊枪开关异常     (g_bGunSwitchFault)
 *   bit3:  焊机热保护       (g_bWeldThermalProtect)
 *   bit4:  缺水保护         (g_bWaterProtect)
 *   bit5:  热丝电源热保护   (g_bWirePSThermal)
 *   bit6:  热丝电源通讯保护 (g_bWirePSCommFault)
 *   bit7:  总报警           (g_bWeldFault)
 *   bit8:  电流有无         (g_bWeldHasCurrent)
 *)


(* ================================================================= *)
(* 输入反馈值 (PC通过Modbus读取)                                       *)
(* 由 CommPoll.st 每周期从 g_byWeldIn 解析写入                        *)
(* ================================================================= *)
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

VAR_GLOBAL RETAIN PERSISTENT


(* D300 | 控制标志位打包字 (PC通过Modbus读写, CommPoll每周期双向映射) *)
g_wWeldCtrlWord    AT %MW300 : UINT := 0;

(* D300 bit0 | 独立BOOL | 开始焊接 1=起弧 0=停弧 [危险, 高有效] *)
g_wWeldOn AT %MW101 : BOOL := FALSE;

(* D300 bit1 | 独立BOOL | 机器人准备好, 高有效 *)
g_wRobotReady AT %MW102 : BOOL := FALSE;
(* D300 bit2 | 独立BOOL | 检气, 高有效 *)
g_wGasCheck AT %MW103 : BOOL := FALSE;

(* D300 bit3 | 独立BOOL | 热丝电源使能 1=ON 0=OFF *)
g_bHotWireEnable AT %MW104 : BOOL := FALSE;

(* D300 bit4 | 独立BOOL | 热丝工作模式 0=DC 1=AC *)
g_bHotWireACMode AT %MW105 : BOOL := FALSE;

(* D300 bit7 | 独立BOOL | 高频有无 0=高频 1=提升 *)
g_bHighFreqDisable AT %MW108 : BOOL := FALSE;

(* D300 bit8 | 独立BOOL | 焊枪模式 0=水冷 1=气冷 *)
g_bGunCoolMode AT %MW109 : BOOL := FALSE;


(* D302 | 协议Byte0 bit3-4 | 焊机操作模式 0=两步 1=四步 2=反复 *)
g_wWeldOperMode    AT %MW302 : UINT := 0;

(* D304 | 协议Byte0 bit5-7 | 焊机工作模式 0=直流恒流 1=直流脉冲 2=交流恒流 *)
(*                           3=交流脉冲 4=直流手弧焊 *)
g_wWeldWorkMode    AT %MW304 : UINT := 1;

(* D306 | 协议Byte11 | 通讯异常标志, 控制器填充, 默认0 *)
g_wCommErrorFlag   AT %MW306 : BYTE;

(* D414 | 协议Byte2 bit0-7 | 热丝频率, 范围20~100, 对应20~100Hz (直通) *)
g_wHotWireFreq     AT %MW414 : UINT := 50;


(* D416 | 协议Byte3 bit0-7 | 热丝电流给定, 范围0~200, 对应0~200A (直通) *)
g_wHotWireCurrent  AT %MW416 : UINT := 150;


(* D400 | 协议Byte4-5 | 给定电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围40~4000, 对应4.0~400.0A), 占D400-D401 *)
g_wWeldCurrent     AT %MW400 : REAL := 100.0;

(* Local wire-feed axis parameters. Speeds are mm/s; delays are seconds.
   D412 and D1202 set the same automatic speed; a changed D1202 has priority.
   D410 > 0 overrides D2104 for axis 4 jog; zero uses D2104 for compatibility. *)
g_rWireStartDelay  AT %MW404 : REAL := 0.0;
g_rWireStopDelay   AT %MW406 : REAL := 0.0;
g_rWireJogSpeed    AT %MW410 : REAL := 0.0;
g_rWireAutoSpeed   AT %MW412 : REAL := 0.0;


(* D420 | 协议Byte13-14 | 前气时间, 实际s, 例: 5.0表示5.00s *)
(* CommPoll内部 ×100 → 协议值(范围1~999, 对应0.01~9.99s), 占D420-D421 *)
g_wFrontGasTime    AT %MW420 : REAL := 0.01;

(* D422 | 协议Byte15-16 | 延气时间, 实际s, 例: 3.0表示3.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~600, 对应0.1~60.0s), 占D422-D423 *)
g_wTailGasTime     AT %MW422 : REAL := 0.2;

(* D424 | 协议Byte17 bit0-7 | 爬升时间, 实际s, 例: 5.0表示5.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~100, 对应0.1~10.0s), 占D424-D425 *)
g_wRampUpTime      AT %MW424 : REAL := 0.5;


(* D426 | 协议Byte18 bit0-7 | 衰减时间, 实际s, 例: 5.0表示5.0s *)
(* CommPoll内部 ×10 → 协议值(范围1~150, 对应0.1~15.0s) *)
g_wRampDownTime    AT %MW426 : REAL := 0.2;

(* D428 | 协议Byte19 bit0-7 | 脉冲频率, 实际Hz, 例: 5.0表示5.0Hz *)
(* CommPoll内部 ×10 → 协议值(范围2~200, 对应0.2~20.0Hz), 占D428-D429 *)
g_wPulseFreq       AT %MW428 : REAL := 0.5;

(* D430 | 协议Byte20+22 bit4-7 | 占空比, 实际百分比, 例: 50.0表示50.0% *)
(* CommPoll内部 ×10 → 协议值(范围1~999, 对应0.1%~99.9%), 占D430-D431 *)
g_wDutyCycle       AT %MW430 : REAL := 50.0;

(* D432 | 协议Byte21+22 bit0-3 | 峰值电流, 实际A, 例: 150.0表示150.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D432-D433 *)
g_wPeakCurrent     AT %MW432 : REAL := 100.0;


(* D434 | 协议Byte23-24 | 基值电流, 实际A, 例: 30.0表示30.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D434-D435 *)
g_wBaseCurrent     AT %MW434 : REAL := 30.0;

(* D436 | 协议Byte25-26 | 起弧电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D436-D437 *)
(* --- 协议 Byte 25-26: 起弧电流 (2字节小端, 低字节在前) --- *)
g_wArcStartCurrent AT %MW436 : REAL;

(* D438 | 协议Byte27-28 | 收弧电流, 实际A, 例: 100.0表示100.0A *)
(* CommPoll内部 ×10 → 协议值(范围50~4000, 对应5.0~400.0A), 占D438-D439 *)
(* --- 协议 Byte 27-28: 收弧电流 (2字节小端, 低字节在前) --- *)
g_wArcEndCurrent   AT %MW438 : REAL;

(* D440 | 协议Byte29 bit0-7 | 交流频率, 范围40~250, 对应40~250Hz (直通) *)
(* --- 协议 Byte 29: 交流频率 --- *)
g_wACFreq          AT %MW440 : UINT;

(* D442 | 协议Byte30 bit0-7 | 清理比例, 范围90~180, 对应40~-50 *)
(* 协议转换: Byte30 = -1 × D442 + 130 *)
(* 例: D442=90 → Byte30=40, D442=130 → Byte30=0, D442=180 → Byte30=-50 *)
(* --- 协议 Byte 30: 清理比例 --- *)
g_wCleanRatio      AT %MW442 : UINT;

(* D444 | 协议Byte31 bit0-7 | 交流偏置, 范围90~180, 对应40~-50 *)
(* 协议转换: Byte31 = -1 × D444 + 130 *)
(* 例: D444=90 → Byte31=40, D444=130 → Byte31=0, D444=180 → Byte31=-50 *)
(* --- 协议 Byte 31: 交流偏置 --- *)
g_wACBias          AT %MW444 : UINT;



(* D308 | 输入状态打包字 (PC通过Modbus读取, CommPoll每周期从独立BOOL组装) *)
g_wWeldStatusWord  AT %MW308 : UINT;

(* D308 bit0 | 独立BOOL | 引弧成功, 高有效 *)
g_bWeldArcOK AT %MW111 : BOOL;

(* D308 bit1 | 独立BOOL | 焊机准备好, 高有效 *)
g_bWeldReady AT %MW112 : BOOL;

(* D308 bit2 | 独立BOOL | 电流有无, 高有效 *)
g_bWeldHasCurrent AT %MW113 : BOOL;

(* D308 bit3 | 独立BOOL | 焊枪开关异常, 高有效 *)
g_bGunSwitchFault AT %MW114 : BOOL;

(* D308 bit4 | 独立BOOL | 焊机热保护, 高有效 *)
g_bWeldThermalProtect AT %MW115 : BOOL;

(* D308 bit5 | 独立BOOL | 缺水保护, 高有效 *)
g_bWaterProtect AT %MW116 : BOOL;

(* D308 bit6 | 独立BOOL | 热丝电源热保护, 高有效 *)
g_bWirePSThermal AT %MW117 : BOOL;

(* D308 bit7 | 独立BOOL | 热丝电源通讯保护, 高有效, 控制器填充 *)
g_bWirePSCommFault AT %MW118 : BOOL;

(* D308 bit8 | 独立BOOL | 组合报警, 包含焊枪开关/热保护/缺水/热丝电源故障 *)
g_bWeldFault AT %MW119 : BOOL;


(* D500 | 焊机输入状态字 (16位打包, 详见位域定义) *)
g_wWeldInStatus    AT %MW500 : UINT;

(* D502-D503 | 协议输入Byte4-5 | 实际焊接电压, 0.1V分辨率 *)
(* --- 协议输入 Byte 4-5: 实际焊接电压 (2字节小端, 0.1V分辨率) --- *)
(* 范围: 0~1000 → 0~100.0V *)
g_rWeldVoltageAct    AT %MW502 : REAL;

(* D506-D507 | 协议输入Byte6-7 | 实际焊接电流, 0.1A分辨率 *)
(* --- 协议输入 Byte 6-7: 实际焊接电流 (2字节小端, 0.1A分辨率) --- *)
(* 范围: 0~400 → 0~400A *)
g_rWeldCurrentAct    AT %MW506 : REAL;

(* D510-D511 | 协议输入Byte8-9 | 实际热丝焊接电流, 10A分辨率 *)
(* --- 协议输入 Byte 8-9: 实际热丝焊接电流 (2字节小端, 10A分辨率) --- *)
(* 范围: 0~20 → 0~200A *)
g_rHotWireCurrentAct AT %MW510 : REAL;

(* D518 | 协议输入Byte1原始值 | 报警码, 报警时记录协议输入Byte1原始值 *)
(* --- 报警原始字节 --- *)
g_rWeldFaultCode     AT %MW518 : UINT;



(* D522 | 激光距离 (实际距离mm, 原始值/100+50) *)
(* --- 激光距离实际值 --- *)
g_rLaserDistance AT %MW522 : REAL := 50.0;


(* ================================================================= *)
(* 协议输入预留字节 (协议保留, 当前未使用)                               *)
(*   协议输入 Byte 2-3: 预留                                          *)
(*   协议输入 Byte 12:  预留                                          *)
(* ================================================================= *)

END_VAR
