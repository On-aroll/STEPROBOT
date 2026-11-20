PROGRAM PRG_WelderBridge
VAR
    aFrame : ARRAY[0..31] OF WORD;
    aParam : ARRAY[0..19] OF WORD;
    uiI : UINT;
    wCheck : WORD;
    uiScan : UINT;
    wPreviousHeartbeat : WORD;
    xHeartbeatSeen : BOOL;
    xHeartbeatEdge : BOOL;
    fbWatchdog : TON;
    xHostFresh : BOOL;
    xBasePermit : BOOL;
    xArcPermit : BOOL;
    xParametersValid : BOOL;
    xWasRunning : BOOL;
    xJobEnded : BOOL;
    udiOwner : UDINT;
    udiCurrentOwner : UDINT;
    udiRequest : UDINT;
    udiLastRequest : UDINT;
    udiRequestOwner : UDINT;
    udiRequestBoot : UDINT;
    udiBoot : UDINT;
    wResult : WORD;
    wError : WORD;
    wOperation : WORD;
    wControl : WORD;
    wApplied : WORD;
END_VAR

// Call once per scan, after PRG_RobotBridge. No other task writes the welder PDOs.
udiBoot := GVL_RobotBridgeRetain.udiBootCounter;
udiCurrentOwner := SHL(WORD_TO_UDINT(GVL_RobotBridge.aHostStatus[15]),16) OR WORD_TO_UDINT(GVL_RobotBridge.aHostStatus[16]);
IF udiCurrentOwner <> udiOwner THEN
    udiOwner := udiCurrentOwner; udiLastRequest := 0; wControl := 0;
    xHeartbeatSeen := FALSE; wPreviousHeartbeat := GVL_WelderBridge.wHeartbeat;
END_IF;
xHeartbeatEdge := GVL_WelderBridge.wHeartbeat <> wPreviousHeartbeat;
IF xHeartbeatEdge THEN
    wPreviousHeartbeat := GVL_WelderBridge.wHeartbeat; xHeartbeatSeen := TRUE;
END_IF;
fbWatchdog(IN := NOT xHeartbeatEdge, PT := T#1200ms);
xHostFresh := xHeartbeatSeen AND NOT fbWatchdog.Q AND GVL_RobotBridge.xHostRegistered AND GVL_RobotBridge.xHostAlive AND (udiOwner <> 0);
xBasePermit := xHostFresh AND GVL_WelderBridge.xMappingConfirmed AND GVL_WelderBridge.xWeldPermit AND GVL_WelderBridge.xPdoOperational
    AND GVL_RobotBridge.xRobotLinkOK AND GVL_RobotBridge.xRemotePermit
    AND NOT GVL_RobotBridge.xStopInterlock AND NOT GVL_RobotBridge.xRobotEmergency AND NOT GVL_RobotBridge.xRobotAlarm;
xArcPermit := xBasePermit AND GVL_RobotBridge.xRobotExternalAuto AND NOT GVL_RobotBridge.xRobotPaused;
xJobEnded := xWasRunning AND NOT GVL_RobotBridge.xRobotRunning;
xWasRunning := GVL_RobotBridge.xRobotRunning;
IF NOT xBasePermit THEN wControl := 0; END_IF;
IF NOT xArcPermit OR xJobEnded THEN wControl := wControl AND WORD#16#0006; END_IF;

// Snapshot the whole FC16 frame, then check checksum, identity and monotonic request.
FOR uiI := 0 TO 31 DO aFrame[uiI] := GVL_WelderBridge.aCommand[uiI]; END_FOR;
udiRequest := SHL(WORD_TO_UDINT(aFrame[2]),16) OR WORD_TO_UDINT(aFrame[3]);
udiRequestOwner := SHL(WORD_TO_UDINT(aFrame[4]),16) OR WORD_TO_UDINT(aFrame[5]);
udiRequestBoot := SHL(WORD_TO_UDINT(aFrame[6]),16) OR WORD_TO_UDINT(aFrame[7]);
wCheck := WORD#16#A55A;
FOR uiI := 0 TO 30 DO wCheck := wCheck XOR aFrame[uiI]; END_FOR;
IF (aFrame[0] = WORD#16#5742) AND (aFrame[1] = WORD#1) AND (wCheck = aFrame[31])
    AND (udiRequestOwner = udiOwner) AND (udiOwner <> 0) AND (udiRequestBoot = udiBoot)
    AND (udiRequest > udiLastRequest) THEN
    udiLastRequest := udiRequest; wOperation := aFrame[8]; wResult := 2; wError := 0;
    IF wOperation = WORD#3 THEN
        wControl := 0; wResult := 1;
    ELSIF NOT xBasePermit THEN
        wError := 1;
    ELSIF wOperation = WORD#1 THEN
        IF (aFrame[9] AND WORD#16#FFF0) <> 0 THEN wError := 2;
        ELSIF ((aFrame[9] AND WORD#16#0009) <> 0) AND NOT xArcPermit THEN wError := 3;
        ELSIF ((aFrame[9] AND WORD#16#0009) <> 0) AND (((aFrame[9] AND WORD#2) = 0) OR NOT GVL_Welders.g_bWeldReady OR GVL_Welders.g_bWeldFault) THEN wError := 4;
        ELSIF ((aFrame[9] AND WORD#1) <> 0) AND ((aFrame[9] AND WORD#4) <> 0) THEN wError := 5;
        ELSE wControl := aFrame[9]; wResult := 1;
        END_IF;
    ELSIF wOperation = WORD#2 THEN
        IF wControl <> 0 THEN wError := 6;
        ELSE
            FOR uiI := 0 TO 19 DO aParam[uiI] := aFrame[10+uiI]; END_FOR;
            xParametersValid := TRUE;
            IF (aParam[0] < WORD#0) OR (aParam[0] > WORD#2) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[1] < WORD#0) OR (aParam[1] > WORD#4) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[2] < WORD#40) OR (aParam[2] > WORD#4000) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[3] < WORD#20) OR (aParam[3] > WORD#100) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[4] < WORD#0) OR (aParam[4] > WORD#200) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[5] < WORD#1) OR (aParam[5] > WORD#999) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[6] < WORD#1) OR (aParam[6] > WORD#600) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[7] < WORD#1) OR (aParam[7] > WORD#100) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[8] < WORD#1) OR (aParam[8] > WORD#150) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[9] < WORD#2) OR (aParam[9] > WORD#200) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[10] < WORD#1) OR (aParam[10] > WORD#999) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[11] < WORD#50) OR (aParam[11] > WORD#4000) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[12] < WORD#50) OR (aParam[12] > WORD#4000) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[13] < WORD#50) OR (aParam[13] > WORD#4000) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[14] < WORD#50) OR (aParam[14] > WORD#4000) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[15] < WORD#40) OR (aParam[15] > WORD#250) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[16] < WORD#90) OR (aParam[16] > WORD#180) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[17] < WORD#90) OR (aParam[17] > WORD#180) THEN xParametersValid := FALSE; END_IF;
            IF (aParam[18] > WORD#7) OR (aParam[19] <> 0) THEN xParametersValid := FALSE; END_IF;
            IF NOT xParametersValid THEN wError := 7;
            ELSE
                GVL_Welders.g_wWeldOperMode := WORD_TO_UINT(aParam[0]);
                GVL_Welders.g_wWeldWorkMode := WORD_TO_UINT(aParam[1]);
                GVL_Welders.g_wWeldCurrent := UINT_TO_REAL(WORD_TO_UINT(aParam[2])) / 10.0;
                GVL_Welders.g_wHotWireFreq := WORD_TO_UINT(aParam[3]);
                GVL_Welders.g_wHotWireCurrent := WORD_TO_UINT(aParam[4]);
                GVL_Welders.g_wFrontGasTime := UINT_TO_REAL(WORD_TO_UINT(aParam[5])) / 100.0;
                GVL_Welders.g_wTailGasTime := UINT_TO_REAL(WORD_TO_UINT(aParam[6])) / 10.0;
                GVL_Welders.g_wRampUpTime := UINT_TO_REAL(WORD_TO_UINT(aParam[7])) / 10.0;
                GVL_Welders.g_wRampDownTime := UINT_TO_REAL(WORD_TO_UINT(aParam[8])) / 10.0;
                GVL_Welders.g_wPulseFreq := UINT_TO_REAL(WORD_TO_UINT(aParam[9])) / 10.0;
                GVL_Welders.g_wDutyCycle := UINT_TO_REAL(WORD_TO_UINT(aParam[10])) / 10.0;
                GVL_Welders.g_wPeakCurrent := UINT_TO_REAL(WORD_TO_UINT(aParam[11])) / 10.0;
                GVL_Welders.g_wBaseCurrent := UINT_TO_REAL(WORD_TO_UINT(aParam[12])) / 10.0;
                GVL_Welders.g_wArcStartCurrent := UINT_TO_REAL(WORD_TO_UINT(aParam[13])) / 10.0;
                GVL_Welders.g_wArcEndCurrent := UINT_TO_REAL(WORD_TO_UINT(aParam[14])) / 10.0;
                GVL_Welders.g_wACFreq := WORD_TO_UINT(aParam[15]);
                GVL_Welders.g_wCleanRatio := WORD_TO_UINT(aParam[16]);
                GVL_Welders.g_wACBias := WORD_TO_UINT(aParam[17]);
                GVL_Welders.g_bHotWireACMode := (aParam[18] AND WORD#1) <> 0;
                GVL_Welders.g_bHighFreqDisable := (aParam[18] AND WORD#2) <> 0;
                GVL_Welders.g_bGunCoolMode := (aParam[18] AND WORD#4) <> 0;
                wResult := 1;
            END_IF;
        END_IF;
    ELSE wError := 8;
    END_IF;
END_IF;
GVL_Welders.g_wWeldOn := (wControl AND WORD#1) <> 0;
GVL_Welders.g_wRobotReady := (wControl AND WORD#2) <> 0;
GVL_Welders.g_wGasCheck := (wControl AND WORD#4) <> 0;
GVL_Welders.g_bHotWireEnable := (wControl AND WORD#8) <> 0;
PRG_WelderIo();
// Same-scan PDO input interlock; never restore an arc after a fault clears.
IF NOT GVL_Welders.g_bWeldReady OR GVL_Welders.g_bWeldFault THEN
    wControl := wControl AND WORD#16#0006;
    GVL_Welders.g_wWeldOn := FALSE; GVL_Welders.g_bHotWireEnable := FALSE;
    GVL_Welders.g_byWeldOutMap01 := GVL_Welders.g_byWeldOutMap01 AND BYTE#16#FE;
    GVL_Welders.g_byWeldOutMap02 := GVL_Welders.g_byWeldOutMap02 AND BYTE#16#FE;
END_IF;
wApplied := wControl;
GVL_Welders.g_wWeldCtrlWord := WORD_TO_UINT(wApplied);
uiScan := uiScan + UINT#1;
GVL_WelderBridge.aStatus[0] := WORD#16#5742;
GVL_WelderBridge.aStatus[1] := 1;
GVL_WelderBridge.aStatus[2] := UINT_TO_WORD(uiScan);
GVL_WelderBridge.aStatus[3] := 1;
IF GVL_WelderBridge.xMappingConfirmed THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#2; END_IF;
IF GVL_WelderBridge.xPdoOperational THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#4; END_IF;
IF xBasePermit THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#8; END_IF;
IF xHostFresh THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#16; END_IF;
IF xArcPermit THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#32; END_IF;
GVL_WelderBridge.aStatus[4] := UDINT_TO_WORD(SHR(udiBoot,16));
GVL_WelderBridge.aStatus[5] := UDINT_TO_WORD(udiBoot AND UDINT#65535);
GVL_WelderBridge.aStatus[6] := UDINT_TO_WORD(SHR(udiLastRequest,16));
GVL_WelderBridge.aStatus[7] := UDINT_TO_WORD(udiLastRequest AND UDINT#65535);
GVL_WelderBridge.aStatus[8] := wResult;
GVL_WelderBridge.aStatus[9] := wError;
GVL_WelderBridge.aStatus[10] := wApplied;
GVL_WelderBridge.aStatus[11] := UINT_TO_WORD(GVL_Welders.g_wWeldInStatus);
GVL_WelderBridge.aStatus[12] := UINT_TO_WORD(GVL_Welders.g_rWeldFaultCode);
GVL_WelderBridge.aStatus[13] := wOperation;
GVL_WelderBridge.aStatus[14] := UDINT_TO_WORD(SHR(udiOwner,16));
GVL_WelderBridge.aStatus[15] := UDINT_TO_WORD(udiOwner AND UDINT#65535);
GVL_WelderBridge.aStatus[16] := REAL_TO_WORD(LIMIT(0.0,GVL_Welders.g_rWeldVoltageAct*10.0,65535.0));
GVL_WelderBridge.aStatus[17] := REAL_TO_WORD(LIMIT(0.0,GVL_Welders.g_rWeldCurrentAct*10.0,65535.0));
GVL_WelderBridge.aStatus[18] := REAL_TO_WORD(LIMIT(0.0,GVL_Welders.g_rHotWireCurrentAct*10.0,65535.0));
GVL_WelderBridge.aStatus[19] := GVL_WelderBridge.wHeartbeat;
GVL_WelderBridge.aStatus[20] := UINT_TO_WORD(GVL_Welders.g_wWeldOperMode);
GVL_WelderBridge.aStatus[21] := UINT_TO_WORD(GVL_Welders.g_wWeldWorkMode);
GVL_WelderBridge.aStatus[22] := REAL_TO_WORD(GVL_Welders.g_wWeldCurrent * 10.0);
GVL_WelderBridge.aStatus[23] := UINT_TO_WORD(GVL_Welders.g_wHotWireFreq);
GVL_WelderBridge.aStatus[24] := UINT_TO_WORD(GVL_Welders.g_wHotWireCurrent);
GVL_WelderBridge.aStatus[25] := REAL_TO_WORD(GVL_Welders.g_wFrontGasTime * 100.0);
GVL_WelderBridge.aStatus[26] := REAL_TO_WORD(GVL_Welders.g_wTailGasTime * 10.0);
GVL_WelderBridge.aStatus[27] := REAL_TO_WORD(GVL_Welders.g_wRampUpTime * 10.0);
GVL_WelderBridge.aStatus[28] := REAL_TO_WORD(GVL_Welders.g_wRampDownTime * 10.0);
GVL_WelderBridge.aStatus[29] := REAL_TO_WORD(GVL_Welders.g_wPulseFreq * 10.0);
GVL_WelderBridge.aStatus[30] := REAL_TO_WORD(GVL_Welders.g_wDutyCycle * 10.0);
GVL_WelderBridge.aStatus[31] := REAL_TO_WORD(GVL_Welders.g_wPeakCurrent * 10.0);
GVL_WelderBridge.aStatus[32] := REAL_TO_WORD(GVL_Welders.g_wBaseCurrent * 10.0);
GVL_WelderBridge.aStatus[33] := REAL_TO_WORD(GVL_Welders.g_wArcStartCurrent * 10.0);
GVL_WelderBridge.aStatus[34] := REAL_TO_WORD(GVL_Welders.g_wArcEndCurrent * 10.0);
GVL_WelderBridge.aStatus[35] := UINT_TO_WORD(GVL_Welders.g_wACFreq);
GVL_WelderBridge.aStatus[36] := UINT_TO_WORD(GVL_Welders.g_wCleanRatio);
GVL_WelderBridge.aStatus[37] := UINT_TO_WORD(GVL_Welders.g_wACBias);
IF GVL_WelderBridge.xWeldPermit THEN GVL_WelderBridge.aStatus[3] := GVL_WelderBridge.aStatus[3] OR WORD#64; END_IF;
GVL_WelderBridge.aStatus[38] := 0;
IF GVL_Welders.g_bHotWireACMode THEN GVL_WelderBridge.aStatus[38] := GVL_WelderBridge.aStatus[38] OR WORD#1; END_IF;
IF GVL_Welders.g_bHighFreqDisable THEN GVL_WelderBridge.aStatus[38] := GVL_WelderBridge.aStatus[38] OR WORD#2; END_IF;
IF GVL_Welders.g_bGunCoolMode THEN GVL_WelderBridge.aStatus[38] := GVL_WelderBridge.aStatus[38] OR WORD#4; END_IF;
GVL_WelderBridge.aStatus[39] := 0;
FOR uiI := 0 TO 15 DO
    GVL_WelderBridge.aStatus[40+uiI] := BYTE_TO_WORD(GVL_Welders.g_byWeldIn[uiI*2]) OR SHL(BYTE_TO_WORD(GVL_Welders.g_byWeldIn[uiI*2+1]),8);
END_FOR;
FOR uiI := 56 TO 63 DO GVL_WelderBridge.aStatus[uiI] := 0; END_FOR;
END_PROGRAM
