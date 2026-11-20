PROGRAM PRG_FileBridge
VAR
    xInit : BOOL;
    xResetOld : BOOL;
    aPrevious : ARRAY[0..119] OF WORD;
    aSeen : ARRAY[0..119] OF WORD;
    aFrame : ARRAY[0..119] OF WORD;
    aAccepted : ARRAY[0..119] OF WORD;
    aFrameBytes : ARRAY[0..235] OF BYTE;
    aPayload : ARRAY[0..191] OF BYTE;
    aNames : ARRAY[0..31] OF STRING(64);
    uiState : UINT;
    wError : WORD;
    udiOwner : UDINT;
    udiAck : UDINT;
    uiAckOp : UINT;
    udiSize : UDINT;
    udiReceived : UDINT;
    udiExpectedCrc : UDINT;
    udiRunningCrc : UDINT := UDINT#16#FFFFFFFF;
    udiTotalSize : UDINT;
    uiVerified : UINT;
    uiExpectedFiles : UINT;
    uiOrdinal : UINT;
    uiMode : UINT;
    sProject : STRING(64);
    sFile : STRING(64);
    fbFtp : FB_FileFtp;
    fbLease : TON;
    wHeartbeatOld : WORD;
    xKick : BOOL;
    xStable : BOOL;
    xNew : BOOL;
    xSame : BOOL;
    xAccept : BOOL;
    xActive : BOOL;
    xClean : BOOL;
    xFileVerified : BOOL;
    xFtpStarted : BOOL;
    wReject : WORD;
    udiRejectSeq : UDINT;
    udiRejectOwner : UDINT;
    wRequestError : WORD;
    udiSeq : UDINT;
    udiSession : UDINT;
    udiBoot : UDINT;
    udiFrameCrc : UDINT;
    udiFrameSize : UDINT;
    udiOffset : UDINT;
    udiFileCrc : UDINT;
    uiOp : UINT;
    uiLength : UINT;
    uiCount : UINT;
    uiIndex : UINT;
    uiField : UINT;
    uiPos : UINT;
    uiI : UINT;
    uiJ : UINT;
    uiSnapshot : UINT;
    wFlags : WORD;
    sNewProject : STRING(64);
    sNewFile : STRING(64);
    pName : POINTER TO BYTE;
    xNamesOk : BOOL;
    xPairsOk : BOOL;
    xFound : BOOL;
    xGlobal : BOOL;
    uiPrograms : UINT;
    sPair : STRING(64);
    sReadyProject : STRING(64);
    uiReadyCapture : UINT;
    uiReadyPath : UINT;
    uiReadyReturn : UINT;
    xFixedConfig : BOOL;
    udiFixedProjectCrc : UDINT;
    uiSessionCapture : UINT;
    uiSessionPath : UINT;
    uiSessionReturn : UINT;
    aSessionTag : ARRAY[0..2] OF WORD;
END_VAR

(* This program NEVER writes robot motion/control outputs or GDI selection. *)
IF NOT xInit THEN
    GVL_FileBridge.xFixedReady := FALSE;
    GVL_FileBridge.xFixedUpdateBusy := FALSE;
    IF GVL_FileBridgeRetain.udiBootCounter = UDINT#16#FFFFFFFF THEN
        GVL_FileBridgeRetain.udiBootCounter := UDINT#1;
    ELSE
        GVL_FileBridgeRetain.udiBootCounter := GVL_FileBridgeRetain.udiBootCounter + UDINT#1;
    END_IF;
    FOR uiI := UINT#0 TO UINT#119 DO
        aPrevious[uiI] := GVL_FileBridge.aCommand[uiI];
        aSeen[uiI] := GVL_FileBridge.aCommand[uiI];
    END_FOR;
    xInit := TRUE;
END_IF;

xFixedConfig := GVL_FileBridge.xFixedMappingConfirmed
    AND (GVL_RobotBridge.uiFixedJobGuardVersion = UINT#140)
    AND F_FileNameValid(GVL_FileBridge.sFixedProject, TRUE)
    AND (GVL_FileBridge.uiFixedCapture >= UINT#1) AND (GVL_FileBridge.uiFixedCapture <= UINT#255)
    AND (GVL_FileBridge.uiFixedPath >= UINT#1) AND (GVL_FileBridge.uiFixedPath <= UINT#255)
    AND (GVL_FileBridge.uiAutoCapacity = UINT#15)
    AND (GVL_FileBridge.uiFixedCapture = UINT#10)
    AND (GVL_FileBridge.uiFixedPath <> UINT#10)
    AND (GVL_FileBridge.uiFixedCapture <> GVL_FileBridge.uiFixedPath)
;
GVL_FileBridge.xFixedIdle := GVL_RobotBridge.xRobotLinkOK AND GVL_RobotBridge.xRobotStopped
    AND NOT GVL_RobotBridge.xRobotRunning AND NOT GVL_RobotBridge.xRobotPaused
    AND NOT GVL_RobotBridge.xRobotEnabled AND NOT GVL_RobotBridge.xBusy
    AND NOT GVL_ManualBridge.xActive;
IF NOT xFixedConfig OR (sReadyProject <> GVL_FileBridge.sFixedProject)
    OR (uiReadyCapture <> GVL_FileBridge.uiFixedCapture) OR (uiReadyPath <> GVL_FileBridge.uiFixedPath)
    OR (uiReadyReturn <> GVL_FileBridge.uiAutoCapacity) THEN
    GVL_FileBridge.xFixedReady := FALSE;
END_IF;
udiFixedProjectCrc := F_FileCrc32(ADR(GVL_FileBridge.sFixedProject),
    INT_TO_UINT(LEN(GVL_FileBridge.sFixedProject)), UDINT#16#FFFFFFFF) XOR UDINT#16#FFFFFFFF;

xClean := NOT fbFtp.xControlOwned AND NOT fbFtp.xDataOwned
    AND ((fbFtp.uiStep = UINT#0) OR (fbFtp.uiStep = UINT#100));
IF GVL_FileBridge.xReset AND NOT xResetOld AND xClean
    AND (uiState <> UINT#300) AND (uiState <> UINT#310) THEN
    uiState := UINT#0;
    wError := WORD#0;
    udiOwner := UDINT#0;
    udiAck := UDINT#0;
    uiAckOp := UINT#0;
    uiVerified := UINT#0;
    uiExpectedFiles := UINT#0;
    udiReceived := UDINT#0;
    udiSize := UDINT#0;
    xFileVerified := FALSE;
    GVL_FileBridge.xFixedReady := FALSE;
    wReject := WORD#0;
END_IF;
xResetOld := GVL_FileBridge.xReset;

xActive := (uiState = UINT#100) OR (uiState = UINT#200)
    OR (uiState = UINT#300) OR (uiState = UINT#310);
xKick := FALSE;
IF (GVL_FileBridge.aHeartbeat[0] = UDINT_TO_WORD(SHR(udiOwner, 16)))
    AND (GVL_FileBridge.aHeartbeat[1] = UDINT_TO_WORD(udiOwner))
    AND (GVL_FileBridge.aHeartbeat[2] = UDINT_TO_WORD(SHR(GVL_FileBridgeRetain.udiBootCounter, 16)))
    AND (GVL_FileBridge.aHeartbeat[3] = UDINT_TO_WORD(GVL_FileBridgeRetain.udiBootCounter))
    AND (GVL_FileBridge.aHeartbeat[4] <> wHeartbeatOld) THEN
    wHeartbeatOld := GVL_FileBridge.aHeartbeat[4];
    xKick := TRUE;
END_IF;
fbLease(IN := xActive AND NOT xKick, PT := T#10s);

(* Two consecutive identical samples, then CRC of big-endian WORD bytes. *)
xStable := TRUE;
xNew := FALSE;
FOR uiI := UINT#0 TO UINT#119 DO
    aFrame[uiI] := GVL_FileBridge.aCommand[uiI];
    IF aFrame[uiI] <> aPrevious[uiI] THEN xStable := FALSE; END_IF;
    IF aFrame[uiI] <> aSeen[uiI] THEN xNew := TRUE; END_IF;
    aPrevious[uiI] := aFrame[uiI];
END_FOR;
IF xStable AND xNew THEN
    FOR uiI := UINT#0 TO UINT#119 DO aSeen[uiI] := aFrame[uiI]; END_FOR;
    FOR uiI := UINT#0 TO UINT#117 DO
        aFrameBytes[uiI * UINT#2] := WORD_TO_BYTE(SHR(aFrame[uiI], 8));
        aFrameBytes[uiI * UINT#2 + UINT#1] := WORD_TO_BYTE(aFrame[uiI] AND WORD#255);
    END_FOR;
    FOR uiI := UINT#0 TO UINT#95 DO
        aPayload[uiI * UINT#2] := WORD_TO_BYTE(SHR(aFrame[uiI + UINT#20], 8));
        aPayload[uiI * UINT#2 + UINT#1] := WORD_TO_BYTE(aFrame[uiI + UINT#20] AND WORD#255);
    END_FOR;
    uiOp := WORD_TO_UINT(aFrame[2]);
    udiSeq := SHL(WORD_TO_UDINT(aFrame[3]), 16) OR WORD_TO_UDINT(aFrame[4]);
    udiSession := SHL(WORD_TO_UDINT(aFrame[5]), 16) OR WORD_TO_UDINT(aFrame[6]);
    udiBoot := SHL(WORD_TO_UDINT(aFrame[7]), 16) OR WORD_TO_UDINT(aFrame[8]);
    udiFrameSize := SHL(WORD_TO_UDINT(aFrame[9]), 16) OR WORD_TO_UDINT(aFrame[10]);
    udiOffset := SHL(WORD_TO_UDINT(aFrame[11]), 16) OR WORD_TO_UDINT(aFrame[12]);
    uiLength := WORD_TO_UINT(aFrame[13]);
    udiFileCrc := SHL(WORD_TO_UDINT(aFrame[14]), 16) OR WORD_TO_UDINT(aFrame[15]);
    udiFrameCrc := F_FileCrc32(ADR(aFrameBytes), UINT#236, UDINT#16#FFFFFFFF) XOR UDINT#16#FFFFFFFF;
    wRequestError := WORD#0;
    xAccept := FALSE;
    IF (aFrame[0] <> WORD#16#F7B1) OR (aFrame[1] <> WORD#1)
        OR (((uiOp <> UINT#1) OR ((aFrame[18] <> WORD#2) AND (aFrame[18] <> WORD#3)))
            AND ((aFrame[19] <> WORD#0) OR (aFrame[116] <> WORD#0) OR (aFrame[117] <> WORD#0)))
        OR (uiLength > UINT#192) OR (udiSeq = UDINT#0) OR (udiSession = UDINT#0) THEN
        wRequestError := WORD#16#E001;
    ELSIF udiFrameCrc <> (SHL(WORD_TO_UDINT(aFrame[118]), 16) OR WORD_TO_UDINT(aFrame[119])) THEN
        wRequestError := WORD#16#E002;
    ELSIF udiBoot <> GVL_FileBridgeRetain.udiBootCounter THEN
        wRequestError := WORD#16#E003;
    ELSIF (udiSession = udiOwner) AND (udiSeq = udiAck) THEN
        xSame := TRUE;
        FOR uiI := UINT#0 TO UINT#119 DO
            IF aAccepted[uiI] <> aFrame[uiI] THEN xSame := FALSE; END_IF;
        END_FOR;
        IF NOT xSame THEN wRequestError := WORD#16#E004; END_IF;
        (* Exact replay: existing ACK/status only; NEVER repeat file/FTP operations. *)
    ELSE
        IF (uiOp = UINT#1) AND ((uiState = UINT#0) OR (uiState = UINT#400)) THEN
            IF (udiSeq <> UDINT#1) OR NOT xClean THEN wRequestError := WORD#16#E005; END_IF;
        ELSIF udiSession <> udiOwner THEN
            wRequestError := WORD#16#E006;
        ELSIF (udiSeq <= udiAck) OR (udiAck = UDINT#16#FFFFFFFF) THEN
            wRequestError := WORD#16#E005;
        ELSIF (uiOp <> UINT#4) AND (uiOp <> UINT#6) AND (udiSeq <> udiAck + UDINT#1) THEN
            wRequestError := WORD#16#E005;
        END_IF;
        IF NOT GVL_FileBridge.xFileTransferPermit AND (uiOp <> UINT#4) AND (uiOp <> UINT#6) THEN
            wRequestError := WORD#16#E007;
        END_IF;
        IF wRequestError = WORD#0 THEN
            CASE uiOp OF
                UINT#1: (* START file; first file also creates project session. *)
                    sNewProject := '';
                    sNewFile := '';
                    uiField := UINT#0;
                    uiPos := UINT#0;
                    xNamesOk := (uiLength >= UINT#2) AND (uiLength <= UINT#98);
                    IF xNamesOk THEN
                        FOR uiI := UINT#0 TO uiLength - UINT#1 DO
                            IF uiField = UINT#0 THEN pName := ADR(sNewProject);
                            ELSE pName := ADR(sNewFile); END_IF;
                            IF aPayload[uiI] = BYTE#0 THEN
                                IF uiPos = UINT#0 THEN xNamesOk := FALSE; END_IF;
                                uiField := uiField + UINT#1;
                                uiPos := UINT#0;
                            ELSIF (uiField > UINT#1) OR (uiPos >= UINT#48) THEN
                                xNamesOk := FALSE;
                            ELSE
                                pName[uiPos] := aPayload[uiI];
                                uiPos := uiPos + UINT#1;
                                pName[uiPos] := BYTE#0;
                            END_IF;
                        END_FOR;
                    END_IF;
                    xNamesOk := xNamesOk AND (uiField = UINT#2) AND (uiPos = UINT#0)
                        AND F_FileNameValid(sNewProject, TRUE) AND F_FileNameValid(sNewFile, FALSE);
                    uiCount := WORD_TO_UINT(aFrame[16]);
                    uiIndex := WORD_TO_UINT(aFrame[17]);
                    IF NOT GVL_FileBridge.xFileTransferPermit THEN
                        wRequestError := WORD#16#E007;
                    ELSIF NOT xNamesOk OR (aFrame[18] > WORD#3) THEN
                        wRequestError := WORD#16#E008;
                    ELSIF ((aFrame[18] = WORD#2) OR (aFrame[18] = WORD#3)) AND (NOT xFixedConfig
                        OR NOT GVL_FileBridge.xFixedIdle OR (sNewProject <> GVL_FileBridge.sFixedProject)
                        OR (uiCount < UINT#4) OR (uiCount > UINT#32) OR ((uiCount MOD UINT#2) <> UINT#0)
                        OR NOT F_FixedFileValid(sNewFile)
                        OR (aFrame[19] = WORD#0) OR (aFrame[19] > WORD#32767)
                        OR (aFrame[116] = WORD#0) OR (aFrame[116] > WORD#32767)
                        OR (aFrame[117] = WORD#0) OR (aFrame[117] > WORD#32767)) THEN
                        wRequestError := WORD#16#E015;
                    ELSIF ((aFrame[18] <> WORD#2) AND (aFrame[18] <> WORD#3)) AND (sNewProject = GVL_FileBridge.sFixedProject) THEN
                        wRequestError := WORD#16#E015;
                    ELSIF (udiFrameSize > UDINT#1048576)
                        OR (uiCount = UINT#0) OR (uiCount > UINT#32) OR (udiOffset <> UDINT#0) THEN
                        wRequestError := WORD#16#E009;
                    ELSIF (aFrame[18] = WORD#1) AND (RIGHT(sNewFile, 4) = '.txt') THEN
                        wRequestError := WORD#16#E008;
                    ELSIF (uiState = UINT#0) OR (uiState = UINT#400) THEN
                        IF (uiIndex <> UINT#1) OR (udiSeq <> UDINT#1) THEN
                            wRequestError := WORD#16#E00A;
                        ELSE
                            udiOwner := udiSession;
                            uiVerified := UINT#0;
                            udiTotalSize := UDINT#0;
                            uiExpectedFiles := uiCount;
                            uiMode := WORD_TO_UINT(aFrame[18]);
                            sProject := sNewProject;
                            IF uiMode >= UINT#2 THEN
                                GVL_FileBridge.xFixedReady := FALSE;
                                GVL_FileBridge.xFixedUpdateBusy := TRUE;
                                uiSessionCapture := GVL_FileBridge.uiFixedCapture;
                                uiSessionPath := GVL_FileBridge.uiFixedPath;
                                uiSessionReturn := GVL_FileBridge.uiAutoCapacity;
                                aSessionTag[0] := aFrame[19]; aSessionTag[1] := aFrame[116]; aSessionTag[2] := aFrame[117];
                            END_IF;
                            xAccept := TRUE;
                        END_IF;
                    ELSIF (uiState <> UINT#200) OR (sNewProject <> sProject)
                        OR (uiIndex <> uiVerified + UINT#1) OR (uiIndex > uiExpectedFiles)
                        OR (uiCount <> uiExpectedFiles) OR (aFrame[18] <> UINT_TO_WORD(uiMode)) THEN
                        wRequestError := WORD#16#E00A;
                    ELSE
                        xAccept := TRUE;
                        IF (uiMode >= UINT#2) AND ((aSessionTag[0] <> aFrame[19])
                            OR (aSessionTag[1] <> aFrame[116]) OR (aSessionTag[2] <> aFrame[117])) THEN
                            xAccept := FALSE; wRequestError := WORD#16#E015;
                        END_IF;
                        FOR uiI := UINT#0 TO uiVerified - UINT#1 DO
                            IF aNames[uiI] = sNewFile THEN
                                xAccept := FALSE;
                                wRequestError := WORD#16#E00B;
                            END_IF;
                        END_FOR;
                    END_IF;
                    IF xAccept THEN
                        IF udiTotalSize + udiFrameSize > UDINT#5242880 THEN
                            xAccept := FALSE;
                            wRequestError := WORD#16#E009;
                        ELSE
                            sFile := sNewFile;
                            udiSize := udiFrameSize;
                            udiExpectedCrc := udiFileCrc;
                            udiRunningCrc := UDINT#16#FFFFFFFF;
                            udiReceived := UDINT#0;
                            uiOrdinal := uiIndex;
                            uiState := UINT#100;
                            wError := WORD#0;
                            xFileVerified := FALSE;
                        END_IF;
                    END_IF;
                UINT#2: (* CHUNK; sequential, no writes while FTP owns buffer. *)
                    IF (uiMode = UINT#3) OR (uiState <> UINT#100) OR (uiLength = UINT#0)
                        OR (udiOffset <> udiReceived) OR (udiOffset + UINT_TO_UDINT(uiLength) > udiSize) THEN
                        wRequestError := WORD#16#E00C;
                    ELSE
                        FOR uiI := UINT#0 TO uiLength - UINT#1 DO
                            GVL_FileBridge.aBuffer[udiReceived + UINT_TO_UDINT(uiI)] := aPayload[uiI];
                        END_FOR;
                        udiRunningCrc := F_FileCrc32(ADR(aPayload), uiLength, udiRunningCrc);
                        udiReceived := udiReceived + UINT_TO_UDINT(uiLength);
                        xAccept := TRUE;
                    END_IF;
                UINT#3: (* DEPLOY only complete, CRC-checked RAM image. *)
                    IF (uiState <> UINT#100) OR ((uiMode <> UINT#3) AND (udiReceived <> udiSize)) OR (uiLength <> UINT#0) THEN
                        wRequestError := WORD#16#E00D;
                    ELSIF (uiMode <> UINT#3) AND ((udiRunningCrc XOR UDINT#16#FFFFFFFF) <> udiExpectedCrc) THEN
                        wRequestError := WORD#16#E00E;
                        wError := wRequestError;
                        uiState := UINT#900;
                    ELSIF NOT GVL_FileBridge.xFileTransferPermit THEN
                        wRequestError := WORD#16#E007;
                    ELSE
                        uiState := UINT#300;
                        xAccept := TRUE;
                    END_IF;
                UINT#4: (* ABORT; cleanup cannot be skipped or rearmed remotely. *)
                    IF (uiState = UINT#300) OR (uiState = UINT#310) THEN
                        uiState := UINT#310;
                    ELSE
                        uiState := UINT#900;
                    END_IF;
                    wError := WORD#16#E010;
                    xAccept := TRUE;
                UINT#5: (* FINISH: all files individually uploaded AND read back. *)
                    IF (uiState <> UINT#200) OR (uiVerified <> uiExpectedFiles) THEN
                        wRequestError := WORD#16#E00F;
                    ELSIF (uiMode >= UINT#2) AND (NOT xFixedConfig OR NOT GVL_FileBridge.xFixedIdle
                        OR (sProject <> GVL_FileBridge.sFixedProject)
                        OR (uiSessionCapture <> GVL_FileBridge.uiFixedCapture)
                        OR (uiSessionPath <> GVL_FileBridge.uiFixedPath)
                        OR (uiSessionReturn <> GVL_FileBridge.uiAutoCapacity)) THEN
                        wRequestError := WORD#16#E015;
                    ELSE
                        xPairsOk := TRUE;
                        IF uiMode = UINT#1 THEN
                            xGlobal := FALSE;
                            uiPrograms := UINT#0;
                            FOR uiI := UINT#0 TO uiVerified - UINT#1 DO
                                IF aNames[uiI] = '_project.srd' THEN xGlobal := TRUE; END_IF;
                                IF RIGHT(aNames[uiI], 4) = '.srp' THEN
                                    uiPrograms := uiPrograms + UINT#1;
                                    sPair := CONCAT(LEFT(aNames[uiI], LEN(aNames[uiI]) - 1), 'd');
                                    xFound := FALSE;
                                    FOR uiJ := UINT#0 TO uiVerified - UINT#1 DO
                                        IF aNames[uiJ] = sPair THEN xFound := TRUE; END_IF;
                                    END_FOR;
                                    IF NOT xFound THEN xPairsOk := FALSE; END_IF;
                                END_IF;
                            END_FOR;
                            xPairsOk := xPairsOk AND xGlobal AND (uiPrograms > UINT#0);
                        END_IF;
                        IF uiMode >= UINT#2 THEN
                            FOR uiI := UINT#0 TO uiVerified - UINT#1 DO
                                IF (F_AutoFileIndex(aNames[uiI]) = UINT#0)
                                    OR (F_AutoFileIndex(aNames[uiI]) > uiVerified) THEN xPairsOk := FALSE; END_IF;
                            END_FOR;
                        END_IF;
                        IF xPairsOk THEN
                            uiState := UINT#400; xAccept := TRUE;
                            IF uiMode >= UINT#2 THEN
                                GVL_FileBridge.xFixedReady := TRUE;
                                FOR uiI := UINT#0 TO UINT#2 DO GVL_FileBridge.aAutoTag[uiI] := aSessionTag[uiI]; END_FOR;
                                sReadyProject := GVL_FileBridge.sFixedProject;
                                uiReadyCapture := GVL_FileBridge.uiFixedCapture;
                                uiReadyPath := GVL_FileBridge.uiFixedPath;
                                uiReadyReturn := GVL_FileBridge.uiAutoCapacity;
                            END_IF;
                        ELSE wRequestError := WORD#16#E012; END_IF;
                    END_IF;
                UINT#6: (* RELEASE only after all handles are known closed. *)
                    IF xClean AND ((uiState = UINT#400) OR (uiState = UINT#900)) THEN
                        uiState := UINT#0;
                        xAccept := TRUE;
                    ELSE wRequestError := WORD#16#E013; END_IF;
            ELSE
                wRequestError := WORD#16#E001;
            END_CASE;
        END_IF;
        IF xAccept THEN
            udiAck := udiSeq;
            uiAckOp := uiOp;
            FOR uiI := UINT#0 TO UINT#119 DO aAccepted[uiI] := aFrame[uiI]; END_FOR;
        END_IF;
    END_IF;
    IF wRequestError <> WORD#0 THEN
        wReject := wRequestError;
        udiRejectSeq := udiSeq;
        udiRejectOwner := udiSession;
    END_IF;
END_IF;

IF xActive AND (fbLease.Q OR NOT GVL_FileBridge.xFileTransferPermit
    OR ((uiMode >= UINT#2) AND (NOT xFixedConfig OR NOT GVL_FileBridge.xFixedIdle
        OR (sProject <> GVL_FileBridge.sFixedProject)
        OR (uiSessionCapture <> GVL_FileBridge.uiFixedCapture)
        OR (uiSessionPath <> GVL_FileBridge.uiFixedPath)
        OR (uiSessionReturn <> GVL_FileBridge.uiAutoCapacity)))) THEN
    IF fbLease.Q THEN wError := WORD#16#E011;
    ELSE wError := WORD#16#E007; END_IF;
    IF (uiState = UINT#300) OR (uiState = UINT#310) THEN uiState := UINT#310;
    ELSIF uiState <> UINT#400 THEN uiState := UINT#900; END_IF;
END_IF;

IF ((uiState = UINT#300) OR (uiState = UINT#310)) AND xFtpStarted AND fbFtp.xDone THEN
    xFtpStarted := FALSE;
    IF (uiState = UINT#300) AND fbFtp.xTestPassed THEN
        IF uiMode = UINT#3 THEN
            (* FB CRC was verified against this session's immutable expected CRC. *)
            udiRunningCrc := udiExpectedCrc XOR UDINT#16#FFFFFFFF;
        END_IF;
        aNames[uiVerified] := sFile;
        uiVerified := uiVerified + UINT#1;
        udiTotalSize := udiTotalSize + udiSize;
        xFileVerified := TRUE;
        uiState := UINT#200;
    ELSE
        IF wError = WORD#0 THEN wError := WORD#16#E014; END_IF;
        IF fbFtp.xControlOwned OR fbFtp.xDataOwned THEN uiState := UINT#910;
        ELSE uiState := UINT#900; END_IF;
    END_IF;
END_IF;
IF (uiState = UINT#300) OR (uiState = UINT#310) THEN xFtpStarted := TRUE; END_IF;
fbFtp(xStart := (uiState = UINT#300) OR (uiState = UINT#310),
    xAbort := uiState = UINT#310, pBuffer := ADR(GVL_FileBridge.aBuffer),
    udiSize := udiSize, sProject := sProject, sFile := sFile,
    xCreateProject := uiVerified = UINT#0,
    xReplaceFixed := uiMode >= UINT#2,
    xVerifyOnly := uiMode = UINT#3, udiVerifyCrc := udiExpectedCrc,
    sUser := GVL_FileBridge.sFtpUser, sPassword := GVL_FileBridge.sFtpPassword);

(* Coherent FC03 status: matching EVEN snapshot head/tail required by PC. *)
IF uiSnapshot >= UINT#65534 THEN uiSnapshot := UINT#0;
ELSE uiSnapshot := uiSnapshot + UINT#2; END_IF;
GVL_FileBridge.aStatus[38] := UINT_TO_WORD(uiSnapshot + UINT#1);
wFlags := WORD#0;
IF GVL_FileBridge.xFileTransferPermit THEN wFlags := wFlags OR WORD#1; END_IF;
IF xClean THEN wFlags := wFlags OR WORD#2; END_IF;
IF xFileVerified THEN wFlags := wFlags OR WORD#4; END_IF;
IF uiState = UINT#400 THEN wFlags := wFlags OR WORD#8; END_IF;
IF (uiState = UINT#300) OR (uiState = UINT#310) THEN wFlags := wFlags OR WORD#16; END_IF;
GVL_FileBridge.aStatus[0] := WORD#16#F7B2;
GVL_FileBridge.aStatus[1] := WORD#1;
GVL_FileBridge.aStatus[2] := UINT_TO_WORD(uiState);
GVL_FileBridge.aStatus[3] := wError;
GVL_FileBridge.aStatus[4] := UDINT_TO_WORD(SHR(udiAck, 16));
GVL_FileBridge.aStatus[5] := UDINT_TO_WORD(udiAck);
GVL_FileBridge.aStatus[6] := UINT_TO_WORD(uiAckOp);
GVL_FileBridge.aStatus[7] := wFlags;
GVL_FileBridge.aStatus[8] := UDINT_TO_WORD(SHR(udiOwner, 16));
GVL_FileBridge.aStatus[9] := UDINT_TO_WORD(udiOwner);
GVL_FileBridge.aStatus[10] := UDINT_TO_WORD(SHR(GVL_FileBridgeRetain.udiBootCounter, 16));
GVL_FileBridge.aStatus[11] := UDINT_TO_WORD(GVL_FileBridgeRetain.udiBootCounter);
GVL_FileBridge.aStatus[12] := UDINT_TO_WORD(SHR(udiReceived, 16));
GVL_FileBridge.aStatus[13] := UDINT_TO_WORD(udiReceived);
GVL_FileBridge.aStatus[14] := UDINT_TO_WORD(SHR(udiSize, 16));
GVL_FileBridge.aStatus[15] := UDINT_TO_WORD(udiSize);
GVL_FileBridge.aStatus[16] := UDINT_TO_WORD(SHR(udiRunningCrc XOR UDINT#16#FFFFFFFF, 16));
GVL_FileBridge.aStatus[17] := UDINT_TO_WORD(udiRunningCrc XOR UDINT#16#FFFFFFFF);
GVL_FileBridge.aStatus[18] := UDINT_TO_WORD(SHR(udiExpectedCrc, 16));
GVL_FileBridge.aStatus[19] := UDINT_TO_WORD(udiExpectedCrc);
GVL_FileBridge.aStatus[20] := UINT_TO_WORD(uiVerified);
GVL_FileBridge.aStatus[21] := UINT_TO_WORD(uiExpectedFiles);
GVL_FileBridge.aStatus[22] := UINT_TO_WORD(uiOrdinal);
GVL_FileBridge.aStatus[23] := UINT_TO_WORD(fbFtp.uiStep);
GVL_FileBridge.aStatus[24] := UINT_TO_WORD(fbFtp.uiReplyCode);
GVL_FileBridge.aStatus[25] := fbFtp.wErrorID;
GVL_FileBridge.aStatus[26] := UINT_TO_WORD(fbFtp.uiErrorStage);
GVL_FileBridge.aStatus[27] := UDINT_TO_WORD(SHR(fbFtp.udiUploadedBytes, 16));
GVL_FileBridge.aStatus[28] := UDINT_TO_WORD(fbFtp.udiUploadedBytes);
GVL_FileBridge.aStatus[29] := UDINT_TO_WORD(SHR(fbFtp.udiReadbackBytes, 16));
GVL_FileBridge.aStatus[30] := UDINT_TO_WORD(fbFtp.udiReadbackBytes);
GVL_FileBridge.aStatus[31] := wReject;
GVL_FileBridge.aStatus[32] := UDINT_TO_WORD(SHR(udiRejectSeq, 16));
GVL_FileBridge.aStatus[33] := UDINT_TO_WORD(udiRejectSeq);
GVL_FileBridge.aStatus[34] := UDINT_TO_WORD(SHR(udiRejectOwner, 16));
GVL_FileBridge.aStatus[35] := UDINT_TO_WORD(udiRejectOwner);
GVL_FileBridge.aStatus[36] := wHeartbeatOld;
GVL_FileBridge.aStatus[37] := WORD#32;
GVL_FileBridge.aStatus[39] := WORD#16;
GVL_FileBridge.aStatus[40] := WORD#0;
GVL_FileBridge.aStatus[41] := WORD#192;
GVL_FileBridge.aStatus[42] := fbFtp.wDataCloseErrorID;
GVL_FileBridge.aStatus[43] := fbFtp.wControlCloseErrorID;
GVL_FileBridge.aStatus[44] := UINT_TO_WORD(uiMode);
GVL_FileBridge.aStatus[45] := UINT_TO_WORD(fbFtp.uiUploadCompleteCode);
GVL_FileBridge.aStatus[46] := UINT_TO_WORD(fbFtp.uiDownloadCompleteCode);
GVL_FileBridge.aStatus[47] := WORD#13; (* bit0 empty files; bit1 guarded fixed-project replacement *)
GVL_FileBridge.aStatus[48] := WORD#16#0104;
FOR uiI := UINT#49 TO UINT#62 DO GVL_FileBridge.aStatus[uiI] := WORD#0; END_FOR;
IF xFixedConfig THEN GVL_FileBridge.aStatus[49] := WORD#1; END_IF;
GVL_FileBridge.xFixedUpdateBusy := (uiMode >= UINT#2) AND ((uiState = UINT#100)
    OR (uiState = UINT#200) OR (uiState = UINT#300) OR (uiState = UINT#310) OR (uiState = UINT#910));
IF GVL_FileBridge.xFixedUpdateBusy THEN GVL_FileBridge.aStatus[50] := WORD#1; END_IF;
IF GVL_FileBridge.xFixedReady THEN GVL_FileBridge.aStatus[51] := WORD#1; END_IF;
GVL_FileBridge.aStatus[52] := UINT_TO_WORD(GVL_FileBridge.uiFixedCapture);
GVL_FileBridge.aStatus[53] := UINT_TO_WORD(GVL_FileBridge.uiFixedPath);
GVL_FileBridge.aStatus[54] := UINT_TO_WORD(GVL_FileBridge.uiAutoCapacity);
GVL_FileBridge.aStatus[55] := UDINT_TO_WORD(SHR(udiFixedProjectCrc, 16));
GVL_FileBridge.aStatus[56] := UDINT_TO_WORD(udiFixedProjectCrc);
IF GVL_FileBridge.xFixedIdle THEN GVL_FileBridge.aStatus[59] := WORD#1; END_IF;
GVL_FileBridge.aStatus[57] := GVL_FileBridge.aAutoTag[0];
GVL_FileBridge.aStatus[58] := GVL_FileBridge.aAutoTag[1];
GVL_FileBridge.aStatus[60] := GVL_FileBridge.aAutoTag[2];
GVL_FileBridge.aStatus[61] := WORD#8192;
GVL_FileBridge.aStatus[62] := WORD#1; (* batch I/O backend revision *)
(* Execute after ManualBridge: it clears the same parameter words first. *)
IF GVL_FileBridge.xFixedReady AND NOT GVL_ManualBridge.xActive
    AND (GVL_RobotBridge.aHostStatus[13] = UINT_TO_WORD(GVL_FileBridge.uiFixedPath)) THEN
    FOR uiI := UINT#0 TO UINT#2 DO GVL_ParamProbe.aParamTx[uiI] := GVL_FileBridge.aAutoTag[uiI]; END_FOR;
END_IF;
GVL_FileBridge.aStatus[63] := UINT_TO_WORD(uiSnapshot);
GVL_FileBridge.aStatus[38] := UINT_TO_WORD(uiSnapshot);
END_PROGRAM
