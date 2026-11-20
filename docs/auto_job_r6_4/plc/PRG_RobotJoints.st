PROGRAM PRG_RobotJoints
VAR
    fbRead : FB_RobotJointRead;
    wFlags : WORD;
    i : UINT;
    uiSource : UINT;
END_VAR
// Independent read-only feedback; existing control/FTP/position channels stay owned by their writers.
fbRead(xEnable := GVL_RobotJoints.xEnable, sAddress := GVL_RobotJoints.sRobotAddress,
    uiPort := GVL_RobotJoints.uiRobotPort, byUnit := GVL_RobotJoints.byRobotUnit);
FOR i := UINT#0 TO UINT#39 DO GVL_RobotJoints.aStatus[i] := WORD#0; END_FOR;
wFlags := WORD#0;
IF GVL_RobotBridge.xRobotLinkOK THEN wFlags := wFlags OR WORD#8; END_IF;
IF GVL_RobotBridge.xRobotStopped AND NOT GVL_RobotBridge.xRobotRunning
    AND NOT GVL_RobotBridge.xRobotPaused THEN wFlags := wFlags OR WORD#2; END_IF;
IF fbRead.xValid AND GVL_RobotBridge.xRobotLinkOK THEN wFlags := wFlags OR WORD#1; END_IF;
GVL_RobotJoints.aStatus[1] := WORD#1;
GVL_RobotJoints.aStatus[2] := wFlags;
GVL_RobotJoints.aStatus[3] := UINT_TO_WORD(fbRead.uiStep);
GVL_RobotJoints.aStatus[4] := UDINT_TO_WORD(SHR(GVL_RobotBridgeRetain.udiBootCounter,16));
GVL_RobotJoints.aStatus[5] := UDINT_TO_WORD(GVL_RobotBridgeRetain.udiBootCounter AND UDINT#65535);
GVL_RobotJoints.aStatus[6] := UDINT_TO_WORD(SHR(fbRead.udiCounter,16));
GVL_RobotJoints.aStatus[7] := UDINT_TO_WORD(fbRead.udiCounter AND UDINT#65535);
GVL_RobotJoints.aStatus[8] := UINT_TO_WORD(fbRead.uiAgeMs);
FOR i := UINT#0 TO UINT#11 DO
    uiSource := i;
    IF GVL_RobotJoints.xHighWordFirst THEN
        IF (i MOD UINT#2) = UINT#0 THEN uiSource := i + UINT#1;
        ELSE uiSource := i - UINT#1; END_IF;
    END_IF;
    GVL_RobotJoints.aStatus[10+i] := fbRead.aWords[uiSource];
END_FOR;
GVL_RobotJoints.aStatus[32] := GVL_RobotJoints.aStatus[6];
GVL_RobotJoints.aStatus[33] := GVL_RobotJoints.aStatus[7];
IF fbRead.xError THEN GVL_RobotJoints.aStatus[34] := WORD#1; END_IF;
IF fbRead.xCleanupFault THEN GVL_RobotJoints.aStatus[34] := WORD#2; END_IF;
GVL_RobotJoints.aStatus[35] := fbRead.wErrorID;
GVL_RobotJoints.aStatus[36] := WORD#16#0641;
GVL_RobotJoints.aStatus[37] := WORD#6;
GVL_RobotJoints.aStatus[38] := WORD#1; // REAL, first register low half
GVL_RobotJoints.aStatus[39] := WORD#40;
GVL_RobotJoints.aStatus[0] := WORD#16#4A52;
END_PROGRAM
