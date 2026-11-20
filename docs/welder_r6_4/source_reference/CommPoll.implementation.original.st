

(* Canonical wire parameters: reject NaN/infinity/negative values without
   converting them to integer timer values. No command/enable is generated. *)
IF NOT ((g_rWireStartDelay >= 0.0) AND (g_rWireStartDelay <= 60.0)) THEN
    g_rWireStartDelay := 0.0;
END_IF;
IF NOT ((g_rWireStopDelay >= 0.0) AND (g_rWireStopDelay <= 60.0)) THEN
    g_rWireStopDelay := 0.0;
END_IF;
IF NOT ((g_rWireJogSpeed >= 0.0) AND (g_rWireJogSpeed <= 1000.0)) THEN
    g_rWireJogSpeed := 0.0;
END_IF;
IF NOT ((g_rWireAutoSpeed >= 0.0) AND (g_rWireAutoSpeed <= 1000.0)) THEN
    g_rWireAutoSpeed := 0.0;
END_IF;
IF NOT ((g_fWireFeedSpeed >= 0.0) AND (g_fWireFeedSpeed <= 1000.0)) THEN
    g_fWireFeedSpeed := 0.0;
END_IF;
IF bWireParamInit THEN
    (* Preserve the existing 64-bit setting across restart. *)
    g_rWireAutoSpeed := LREAL_TO_REAL(g_fWireFeedSpeed);
    bWireParamInit := FALSE;
ELSIF g_fWireFeedSpeed <> lrPrevWireRun THEN
    g_rWireAutoSpeed := LREAL_TO_REAL(g_fWireFeedSpeed);
ELSIF g_rWireAutoSpeed <> rPrevWireAuto THEN
    g_fWireFeedSpeed := REAL_TO_LREAL(g_rWireAutoSpeed);
END_IF;
rPrevWireAuto := g_rWireAutoSpeed;
lrPrevWireRun := g_fWireFeedSpeed;
g_dwModbusSignature := 16#4D573031;
g_wModbusMapVersion := 1;
g_dwModbusCycle := g_dwModbusCycle + UDINT#1;
g_wModbusProbeEcho := g_wModbusProbe XOR UINT#16#A55A;

IF bBootInit THEN
    g_wWeldCtrlWord := 0;
    g_wWeldOn := FALSE;
    g_wRobotReady := FALSE;
    g_wGasCheck := FALSE;
    g_bHotWireEnable := FALSE;
    g_bHotWireACMode := FALSE;
    g_bHighFreqDisable := FALSE;
    g_bGunCoolMode := FALSE;
    wPrevCtrlPacked := 0;
    bBootInit := FALSE;
END_IF;


(* ================================================================ *)
(* D300 <-> 独立BOOL 双向合并                                         *)
(* 1. 从独立BOOL打包到临时变量                                        *)
(* 2. 若D300与上次打包值不同(PC写了D300), 则拆位D300→BOOL             *)
(* 3. 再从BOOL打包→D300 (PLC改BOOL也能反映到D300)                     *)
(* ================================================================ *)

wCtrlPacked := 0;
IF g_wWeldOn         THEN wCtrlPacked := wCtrlPacked OR 16#0001; END_IF;
IF g_wRobotReady     THEN wCtrlPacked := wCtrlPacked OR 16#0002; END_IF;
IF g_wGasCheck       THEN wCtrlPacked := wCtrlPacked OR 16#0004; END_IF;
IF g_bHotWireEnable  THEN wCtrlPacked := wCtrlPacked OR 16#0008; END_IF;
IF g_bHotWireACMode  THEN wCtrlPacked := wCtrlPacked OR 16#0010; END_IF;
IF g_bHighFreqDisable THEN wCtrlPacked := wCtrlPacked OR 16#0080; END_IF;
IF g_bGunCoolMode    THEN wCtrlPacked := wCtrlPacked OR 16#0100; END_IF;

IF g_wWeldCtrlWord <> wPrevCtrlPacked THEN
    g_wWeldOn         := (g_wWeldCtrlWord AND 16#0001) <> 0;
    g_wRobotReady     := (g_wWeldCtrlWord AND 16#0002) <> 0;
    g_wGasCheck       := (g_wWeldCtrlWord AND 16#0004) <> 0;
    g_bHotWireEnable  := (g_wWeldCtrlWord AND 16#0008) <> 0;
    g_bHotWireACMode  := (g_wWeldCtrlWord AND 16#0010) <> 0;
    g_bHighFreqDisable := (g_wWeldCtrlWord AND 16#0080) <> 0;
    g_bGunCoolMode    := (g_wWeldCtrlWord AND 16#0100) <> 0;
    wCtrlPacked := g_wWeldCtrlWord;
END_IF;

g_wWeldCtrlWord := wCtrlPacked;
wPrevCtrlPacked := wCtrlPacked;

(* ================================================================ *)
(* 焊机输出 (协议: 焊机的输入数据区, PLC写→焊机读)                    *)
(* g_byWeldOut[0..31] → EtherCAT输出 → 焊机                        *)
(* ================================================================ *)

(* ===== 协议 Byte 0: 控制位 ===== *)
(* bit0(1):  开始焊接 1=起弧 0=停弧                                  *)
(* bit1(2):  机器人准备好 高有效                                      *)
(* bit2(3):  检气 高有效                                              *)
(* bit3-4:   焊机操作模式 00=两步 01=四步 10=反复                     *)
(* bit5-7:   焊机工作模式 000=直流恒流 001=直流脉冲 010=交流恒流       *)
(*           011=交流脉冲 100=直流手弧焊                               *)
byTemp := 0;
IF g_wWeldOn THEN byTemp := byTemp OR 16#01; END_IF;
IF g_wRobotReady THEN byTemp := byTemp OR 16#02; END_IF;
IF g_wGasCheck THEN byTemp := byTemp OR 16#04; END_IF;
nTempU16 := LIMIT(UINT#0, g_wWeldOperMode, UINT#2);
byTemp := byTemp OR SHL(UINT_TO_BYTE(nTempU16), 3);
nTempU16 := LIMIT(UINT#0, g_wWeldWorkMode, UINT#4);
byTemp := byTemp OR SHL(UINT_TO_BYTE(nTempU16), 5);
g_byWeldOut[0] := byTemp;

(* ===== 协议 Byte 1: 辅助控制位 ===== *)
(* bit0(9):  热丝电源使能 1=ON 0=OFF                                  *)
(* bit1(10): 热丝工作模式 0=DC 1=AC                                   *)
(* bit2(11): 备用                                                     *)
(* bit3(12): 点动送丝 (已移除, 送丝通过轴控制) *)
(* bit4(13): 点动抽丝 (已移除, 送丝通过轴控制) *)
(* bit5(14): 备用                                                     *)
(* bit6(15): 高频有无 0=高频 1=提升                                   *)
(* bit7(16): 焊枪模式 0=水冷 1=气冷                                   *)
byTemp := 0;
IF g_bHotWireEnable THEN byTemp := byTemp OR 16#01; END_IF;
IF g_bHotWireACMode THEN byTemp := byTemp OR 16#02; END_IF;
IF g_bHighFreqDisable THEN byTemp := byTemp OR 16#40; END_IF;
IF g_bGunCoolMode THEN byTemp := byTemp OR 16#80; END_IF;
g_byWeldOut[1] := byTemp;

(* ===== 协议 Byte 2: 热丝频率 ===== *)
(* 范围: 20-100 对应20~100Hz (1字节) *)
g_byWeldOut[2] := UINT_TO_BYTE(LIMIT(UINT#20, g_wHotWireFreq, UINT#100));

(* ===== 协议 Byte 3: 热丝电流给定 ===== *)
(* 范围: 0-200 对应0~200A (1字节) *)
g_byWeldOut[3] := UINT_TO_BYTE(LIMIT(UINT#0, g_wHotWireCurrent, UINT#200));

(* ===== 协议 Byte 4-5: 给定电流 (2字节小端, 低字节在前) ===== *)
(* 范围: 40-4000 对应4.0~400.0A *)
nTempU16 := REAL_TO_UINT(LIMIT(4.0, g_wWeldCurrent, 400.0)*10.0);

g_byWeldOut[4] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[5] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 6: 送丝速度 (已移除, 送丝通过轴控制, 写0) ===== *)
g_byWeldOut[6] := 0;


(* ===== 协议 Byte 7: 起弧延迟送丝时间 (已移除, 写0) ===== *)
g_byWeldOut[7] := 0;

(* ===== 协议 Byte 8: 熄弧滞后送丝时间 (已移除, 写0) ===== *)
g_byWeldOut[8] := 0;

(* ===== 协议 Byte 9: 熄弧回抽丝长度 (已移除, 写0) ===== *)
g_byWeldOut[9] := 0;

(* ===== 协议 Byte 10: 点动送丝速度 (已移除, 送丝通过轴控制, 写0) ===== *)
g_byWeldOut[10] := 0;

(* ===== 协议 Byte 11: 通讯异常标志 (控制器填充) ===== *)
g_byWeldOut[11] := g_wCommErrorFlag;

(* ===== 协议 Byte 12: 慢送丝速度 (已移除, 送丝通过轴控制, 写0) ===== *)
g_byWeldOut[12] := 0;

(* ===== 协议 Byte 13-14: 前气时间 (2字节小端, 低字节在前) ===== *)
(* 范围: 1-999 对应0.01~9.99s *)
nTempU16 := REAL_TO_UINT(LIMIT(0.01, g_wFrontGasTime, 9.99)*100.0);
g_byWeldOut[13] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[14] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 15-16: 延气时间 (2字节小端, 低字节在前) ===== *)
(* 范围: 1-600 对应0.1~60.0s *)
nTempU16 := REAL_TO_UINT(LIMIT(0.1, g_wTailGasTime, 60.0)*10.0);
g_byWeldOut[15] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[16] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 17: 爬升时间 ===== *)
(* 范围: 1-100 对应0.1~10.0s (1字节) *)
g_byWeldOut[17] := UINT_TO_BYTE(REAL_TO_UINT(LIMIT(0.1, g_wRampUpTime, 10.0)*10.0));

(* ===== 协议 Byte 18: 衰减时间 ===== *)
(* 范围: 1-150 对应0.1~15.0s (1字节) *)
g_byWeldOut[18] := UINT_TO_BYTE(REAL_TO_UINT(LIMIT(0.1, g_wRampDownTime, 15.0)*10.0));

(* ===== 协议 Byte 19: 脉冲频率 ===== *)
(* 范围: 2-200 对应0.2~20.0Hz (1字节) *)
g_byWeldOut[19] := UINT_TO_BYTE(REAL_TO_UINT(LIMIT(0.2, g_wPulseFreq, 20.0)*10.0));

(* ===== 协议 Byte 20-22: 占空比 + 峰值电流 (交叉编排) ===== *)
(* 占空比: Byte20=低字节, Byte22 bit4-7=高字节低4位 (范围1-999, 12位) *)
(* 峰值电流: Byte21=低字节, Byte22 bit0-3=高字节低4位 (范围50-4000, 12位) *)
(* 占空比 = [Byte22 bit7:4][Byte20] *)
(* 峰值电流 = [Byte22 bit3:0][Byte21] *)
nTempU16 := REAL_TO_UINT(LIMIT(0.1, g_wDutyCycle, 99.9)*10.0);
g_byWeldOut[20] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[21] := UINT_TO_BYTE(UINT_TO_USINT(REAL_TO_UINT(LIMIT(5.0, g_wPeakCurrent, 400.0)*10.0) MOD 256));
byTemp := 0;
byTemp := byTemp OR SHL(UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256)) AND 16#0F, 4);
byTemp := byTemp OR (UINT_TO_BYTE(UINT_TO_USINT(REAL_TO_UINT(LIMIT(5.0, g_wPeakCurrent, 400.0)*10.0) / 256)) AND 16#0F);
g_byWeldOut[22] := byTemp;

(* ===== 协议 Byte 23-24: 基值电流 (2字节小端, 低字节在前) ===== *)
(* 范围: 50-4000 对应5.0~400.0A *)
nTempU16 := REAL_TO_UINT(LIMIT(5.0, g_wBaseCurrent, 400.0)*10.0);
g_byWeldOut[23] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[24] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 25-26: 起弧电流 (2字节小端, 低字节在前) ===== *)
(* 范围: 50-4000 对应5.0~400.0A *)
nTempU16 := REAL_TO_UINT(LIMIT(5.0, g_wArcStartCurrent, 400.0)*10.0);
g_byWeldOut[25] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[26] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 27-28: 收弧电流 (2字节小端, 低字节在前) ===== *)
(* 范围: 50-4000 对应5.0~400.0A *)
nTempU16 := REAL_TO_UINT(LIMIT(5.0, g_wArcEndCurrent, 400.0)*10.0);
g_byWeldOut[27] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 MOD 256));
g_byWeldOut[28] := UINT_TO_BYTE(UINT_TO_USINT(nTempU16 / 256));

(* ===== 协议 Byte 29: 交流频率 ===== *)
(* 范围: 40-250 对应40-250Hz (1字节) *)
g_byWeldOut[29] := UINT_TO_BYTE(LIMIT(UINT#40, g_wACFreq, UINT#250));

(* ===== 协议 Byte 30: 清理比例 ===== *)
(* 范围: 90-180 对应40~-50 (1字节) *)
nTempI16 := 130 - UINT_TO_INT(LIMIT(UINT#90, g_wCleanRatio, UINT#180));
g_byWeldOut[30] := INT_TO_BYTE(nTempI16);

(* ===== 协议 Byte 31: 交流偏置 ===== *)
(* 范围: 90-180 对应40~-50 (1字节) *)
nTempI16 := 130 - UINT_TO_INT(LIMIT(UINT#90, g_wACBias, UINT#180));
g_byWeldOut[31] := INT_TO_BYTE(nTempI16);

(* ================================================================ *)
(* 焊机输入 (协议: 焊机输出数据区, 焊机写→PLC读)                      *)
(* g_byWeldIn[0..31] ← EtherCAT输入 ← 焊机                          *)
(* ================================================================ *)

(* ===== 协议输入 Byte 0: 状态位 ===== *)
(* bit0: 引弧成功 高有效                                              *)
(* bit1: 焊机准备好 高有效                                            *)
(* bit2: 预留                                                         *)
(* bit3: 电流有无 高有效                                              *)
(* EtherCAT 输入映射别名 → 协议数组。保持原有 Byte 0..31 定义。 *)
g_byWeldIn[0] := g_byWeldInMap01;
g_byWeldIn[1] := g_byWeldInMap02;
g_byWeldIn[2] := g_byWeldInMap03;
g_byWeldIn[3] := g_byWeldInMap04;
g_byWeldIn[4] := g_byWeldInMap05;
g_byWeldIn[5] := g_byWeldInMap06;
g_byWeldIn[6] := g_byWeldInMap07;
g_byWeldIn[7] := g_byWeldInMap08;
g_byWeldIn[8] := g_byWeldInMap09;
g_byWeldIn[9] := g_byWeldInMap10;
g_byWeldIn[10] := g_byWeldInMap11;
g_byWeldIn[11] := g_byWeldInMap12;
g_byWeldIn[12] := g_byWeldInMap13;
g_byWeldIn[13] := g_byWeldInMap14;
g_byWeldIn[14] := g_byWeldInMap15;
g_byWeldIn[15] := g_byWeldInMap16;
g_byWeldIn[16] := g_byWeldInMap17;
g_byWeldIn[17] := g_byWeldInMap18;
g_byWeldIn[18] := g_byWeldInMap19;
g_byWeldIn[19] := g_byWeldInMap20;
g_byWeldIn[20] := g_byWeldInMap21;
g_byWeldIn[21] := g_byWeldInMap22;
g_byWeldIn[22] := g_byWeldInMap23;
g_byWeldIn[23] := g_byWeldInMap24;
g_byWeldIn[24] := g_byWeldInMap25;
g_byWeldIn[25] := g_byWeldInMap26;
g_byWeldIn[26] := g_byWeldInMap27;
g_byWeldIn[27] := g_byWeldInMap28;
g_byWeldIn[28] := g_byWeldInMap29;
g_byWeldIn[29] := g_byWeldInMap30;
g_byWeldIn[30] := g_byWeldInMap31;
g_byWeldIn[31] := g_byWeldInMap32;

g_bWeldArcOK      := (g_byWeldIn[0] AND 16#01) <> 0;
g_bWeldReady      := (g_byWeldIn[0] AND 16#02) <> 0;
g_bWeldHasCurrent     := (g_byWeldIn[0] AND 16#08) <> 0;

(* ===== 协议输入 Byte 1: 报警位 ===== *)
(* bit0: 预留                                                         *)
(* bit1: 焊枪开关异常 高有效                                          *)
(* bit2: 焊机热保护 高有效                                            *)
(* bit3: 缺水保护 高有效                                              *)
(* bit4: 热丝电源热保护 高有效                                        *)
(* bit5: 热丝电源通讯保护 高有效 (控制器填充)                          *)
g_bGunSwitchFault    := (g_byWeldIn[1] AND 16#02) <> 0;
g_bWeldThermalProtect := (g_byWeldIn[1] AND 16#04) <> 0;
g_bWaterProtect   := (g_byWeldIn[1] AND 16#08) <> 0;
g_bWirePSThermal  := (g_byWeldIn[1] AND 16#10) <> 0;
g_bWirePSCommFault:= (g_byWeldIn[1] AND 16#20) <> 0;

(* ===== 报警组合 ===== *)
g_bWeldFault := g_bWeldThermalProtect
              OR g_bGunSwitchFault
              OR g_bWaterProtect
              OR g_bWirePSThermal
              OR g_bWirePSCommFault;

IF g_bWeldFault THEN
    g_rWeldFaultCode := BYTE_TO_UINT(g_byWeldIn[1]);
ELSE
    g_rWeldFaultCode := 0;
END_IF;

IF NOT g_bWeldReady OR g_bWeldFault THEN
    g_wWeldOn := FALSE;
    g_wWeldCtrlWord := g_wWeldCtrlWord AND 16#FFFE;
    wPrevCtrlPacked := wPrevCtrlPacked AND 16#FFFE;
    g_byWeldOut[0] := g_byWeldOut[0] AND 16#FE;
END_IF;

(* ===== EtherCAT 输出映射别名同步 ===== *)
(* 放在未就绪/故障联锁之后，确保停弧命令在本周期立即写入PDO。 *)
g_byWeldOutMap01 := g_byWeldOut[0];
g_byWeldOutMap02 := g_byWeldOut[1];
g_byWeldOutMap03 := g_byWeldOut[2];
g_byWeldOutMap04 := g_byWeldOut[3];
g_byWeldOutMap05 := g_byWeldOut[4];
g_byWeldOutMap06 := g_byWeldOut[5];
g_byWeldOutMap07 := g_byWeldOut[6];
g_byWeldOutMap08 := g_byWeldOut[7];
g_byWeldOutMap09 := g_byWeldOut[8];
g_byWeldOutMap10 := g_byWeldOut[9];
g_byWeldOutMap11 := g_byWeldOut[10];
g_byWeldOutMap12 := g_byWeldOut[11];
g_byWeldOutMap13 := g_byWeldOut[12];
g_byWeldOutMap14 := g_byWeldOut[13];
g_byWeldOutMap15 := g_byWeldOut[14];
g_byWeldOutMap16 := g_byWeldOut[15];
g_byWeldOutMap17 := g_byWeldOut[16];
g_byWeldOutMap18 := g_byWeldOut[17];
g_byWeldOutMap19 := g_byWeldOut[18];
g_byWeldOutMap20 := g_byWeldOut[19];
g_byWeldOutMap21 := g_byWeldOut[20];
g_byWeldOutMap22 := g_byWeldOut[21];
g_byWeldOutMap23 := g_byWeldOut[22];
g_byWeldOutMap24 := g_byWeldOut[23];
g_byWeldOutMap25 := g_byWeldOut[24];
g_byWeldOutMap26 := g_byWeldOut[25];
g_byWeldOutMap27 := g_byWeldOut[26];
g_byWeldOutMap28 := g_byWeldOut[27];
g_byWeldOutMap29 := g_byWeldOut[28];
g_byWeldOutMap30 := g_byWeldOut[29];
g_byWeldOutMap31 := g_byWeldOut[30];
g_byWeldOutMap32 := g_byWeldOut[31];

(* ===== 协议输入 Byte 4-5: 实际焊接电压 (2字节小端, 0.1V分辨率) ===== *)
(* 范围: 0-1000 对应0~100.0V *)
nTempU16 := BYTE_TO_UINT(g_byWeldIn[4]) + BYTE_TO_UINT(g_byWeldIn[5]) * 256;
g_rWeldVoltageAct := UINT_TO_REAL(nTempU16) * 0.1;

(* ===== 协议输入 Byte 6-7: 实际焊接电流 (2字节小端, 0.1A分辨率) ===== *)
(* 范围: 0~400 对应0~400A *)
nTempU16 := BYTE_TO_UINT(g_byWeldIn[6]) + BYTE_TO_UINT(g_byWeldIn[7]) * 256;
g_rWeldCurrentAct := UINT_TO_REAL(nTempU16) * 0.1;

(* ===== 协议输入 Byte 8-9: 实际热丝焊接电流 (2字节小端, 10A分辨率) ===== *)
(* 范围: 0~20 对应0~200A *)
nTempU16 := BYTE_TO_UINT(g_byWeldIn[8]) + BYTE_TO_UINT(g_byWeldIn[9]) * 256;
g_rHotWireCurrentAct := UINT_TO_REAL(nTempU16) * 10.0;

(* ===== 协议输入 Byte 10-11: 送丝速度反馈 (已移除, 送丝通过轴控制) ===== *)

(* ===== 组装输入状态字 (PC通过D500读取) ===== *)
g_wWeldInStatus := 0;
IF g_bWeldArcOK      THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0001; END_IF;
IF g_bWeldReady      THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0002; END_IF;
IF g_bGunSwitchFault THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0004; END_IF;
IF g_bWeldThermalProtect THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0008; END_IF;
IF g_bWaterProtect   THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0010; END_IF;
IF g_bWirePSThermal  THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0020; END_IF;
IF g_bWirePSCommFault THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0040; END_IF;
IF g_bWeldFault      THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0080; END_IF;
IF g_bWeldHasCurrent     THEN g_wWeldInStatus := g_wWeldInStatus OR 16#0100; END_IF;

(* ================================================================ *)
(* D308 独立BOOL → 打包字 (PLC内部 → PC读D308)                         *)
(* CommPoll每周期从独立BOOL组装到g_wWeldStatusWord供PC读取             *)
(* ================================================================ *)
g_wWeldStatusWord := 0;
IF g_bWeldArcOK      THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0001; END_IF;
IF g_bWeldReady      THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0002; END_IF;
IF g_bWeldHasCurrent     THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0004; END_IF;
IF g_bGunSwitchFault THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0008; END_IF;
IF g_bWeldThermalProtect THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0010; END_IF;
IF g_bWaterProtect   THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0020; END_IF;
IF g_bWirePSThermal  THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0040; END_IF;
IF g_bWirePSCommFault THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0080; END_IF;
IF g_bWeldFault      THEN g_wWeldStatusWord := g_wWeldStatusWord OR 16#0100; END_IF;

(* ================================================================ *)
(* 激光距离读取和转换                                                *)
(* 从%IW50读取激光距离原始INT值, 转换为实际距离并存入寄存器            *)
(* 转换公式: 实际距离 = 原始值 / 100 + 50                             *)
(* D522 = g_rLaserDistance (REAL, PC可通过Modbus读取)                   *)
(* ================================================================ *)
g_rLaserDistance := INT_TO_REAL(g_iLaserDistanceRaw) / 100.0 + 50.0;
