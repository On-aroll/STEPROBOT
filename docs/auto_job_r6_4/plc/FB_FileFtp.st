FUNCTION_BLOCK FB_FileFtp
VAR_INPUT
    xStart : BOOL;
    xAbort : BOOL;
    pBuffer : POINTER TO BYTE;
    udiSize : UDINT;
    sProject : STRING(64);
    sFile : STRING(64);
    xCreateProject : BOOL;
    xReplaceFixed : BOOL;
    xVerifyOnly : BOOL;
    udiVerifyCrc : UDINT;
    sUser : STRING(64);
    sPassword : STRING(64);
END_VAR
VAR CONSTANT
    cdiControlID : DINT := DINT#245;
    cdiDataID : DINT := DINT#246;
END_VAR
VAR_OUTPUT
    xDone : BOOL := FALSE;
    xTestPassed : BOOL := FALSE;
    xError : BOOL := FALSE;
    xLoginOk : BOOL := FALSE;
    xBaseDirOk : BOOL := FALSE;
    xScratchCreated : BOOL := FALSE;
    xUploadComplete : BOOL := FALSE;
    xReadbackComplete : BOOL := FALSE;
    xContentMatch : BOOL := FALSE;
    xQuitOk : BOOL := FALSE;
    xCleanupOk : BOOL := FALSE;
    uiStep : UINT := UINT#0;
    uiErrorStage : UINT := UINT#0;
    wErrorID : WORD := WORD#0;
    wDataCloseErrorID : WORD := WORD#0;
    wControlCloseErrorID : WORD := WORD#0;
    udiExpectedBytes : UDINT := UDINT#0;
    udiUploadedBytes : UDINT := UDINT#0;
    udiReadbackBytes : UDINT := UDINT#0;
    uiReplyCode : UINT := UINT#0;
    uiUserReplyCode : UINT := UINT#0;
    uiPassReplyCode : UINT := UINT#0;
    uiBaseDirReplyCode : UINT := UINT#0;
    uiNameCheckReplyCode : UINT := UINT#0;
    uiMkdReplyCode : UINT := UINT#0;
    uiStorReplyCode : UINT := UINT#0;
    uiUploadCompleteCode : UINT := UINT#0;
    uiRetrReplyCode : UINT := UINT#0;
    uiDownloadCompleteCode : UINT := UINT#0;
    uiPasvPort : UINT := UINT#0;
    wReadbackEndStatus : WORD := WORD#0;
    sRemotePath : STRING(160);
    sLastReply : STRING(255);
    sMkdReply : STRING(255);
    sPasvReply : STRING(255);
    sUploadReply : STRING(255);
    sDownloadReply : STRING(255);
    xControlOwned : BOOL := FALSE;
    xDataOwned : BOOL := FALSE;
    xUploadDataConnected : BOOL := FALSE;
    xDownloadDataConnected : BOOL := FALSE;
    xLineTruncated : BOOL := FALSE;

END_VAR
VAR
    (* Caller owns immutable buffer until xDone and clean socket closure. *)
    xAbortLatched : BOOL;
    xProgress : BOOL;
    xDirectoryReady : BOOL;
    xReplaceRun : BOOL;
    sFileRun : STRING(64);
    pBufferRun : POINTER TO BYTE;

    stControlServer : CmpHCTCPIP.ADDRESS := (strIPAddr := '192.168.3.188', uiPort := UINT#21);
    stDataServer : CmpHCTCPIP.ADDRESS;
    fbControlClient : CmpHCTCPIP.SktTCPClient;
    fbControlClose : CmpHCTCPIP.SktClose;
    fbDataClient : CmpHCTCPIP.SktTCPClient;
    fbDataClose : CmpHCTCPIP.SktClose;
    fbReplyTimeout : TON;
    fbCommandTimeout : TON;
    fbUploadTimeout : TON;
    fbReadbackTimeout : TON;
    fbDataCloseTimeout : TON;
    fbControlCloseTimeout : TON;

    xStartOld : BOOL := FALSE;
    xFail : BOOL := FALSE;
    xFinishedNow : BOOL := FALSE;
    wPendingError : WORD := WORD#0;
    uiPendingStage : UINT := UINT#0;
    uiPhase : UINT := UINT#0;
    uiNextCommand : UINT := UINT#0;
    uiTransferMode : UINT := UINT#0;
    xReplyReady : BOOL := FALSE;
    xReplyAccepted : BOOL := FALSE;
    xDigits : BOOL := FALSE;
    xMultiline : BOOL := FALSE;
    sMultilineCode : STRING(3);
    sLine : STRING(255);
    pLine : POINTER TO BYTE;
    uiLineLength : UINT := UINT#0;
    fbControlIo : FB_FileSocketIo;
    fbDataIo : FB_FileSocketIo;
    uiControlRxIndex : UINT;
    uiControlRxCount : UINT;
    uiParse : UINT;
    byControl : BYTE;
    pSendNow : POINTER TO BYTE;
    uiSendNow : UINT;
    xVerifyRun : BOOL;
    udiVerifyCrcRun : UDINT;
    udiReadbackCrc : UDINT;
    sCommand : STRING(160);
    pCommand : POINTER TO BYTE;
    uiCommandIndex : UINT := UINT#0;
    uiCommandLength : UINT := UINT#0;
    sUserRun : STRING(64);
    sPasswordRun : STRING(64);
    sScratchRun : STRING(64);
    pCredential : POINTER TO BYTE;
    uiI : UINT := UINT#0;
    xNameCharacter : BOOL := FALSE;
    xBytesMatch : BOOL := FALSE;
    xReadbackEndCandidate : BOOL := FALSE;
    xDataCloseFailed : BOOL := FALSE;
    pReply : POINTER TO BYTE;
    aPasv : ARRAY[0..5] OF UINT;
    uiPasvField : UINT := UINT#0;
    uiPasvValue : UINT := UINT#0;
    xPasvValid : BOOL := FALSE;
    xPasvInTuple : BOOL := FALSE;
    xPasvClosed : BOOL := FALSE;
    xPasvHasDigit : BOOL := FALSE;
END_VAR
(* Derived from field-passed PRG_FtpUploadProbe. New bridge is NOT field validated. *)
(* IDs 245/246 must be free; default cabinet 192.168.3.188:21. *)
(* Bounded 8192-byte I/O, exact offsets and explicit EOF. No motion commands. *)
pLine := ADR(sLine);
pCommand := ADR(sCommand);
xFail := FALSE;
xFinishedNow := FALSE;
xReplyReady := FALSE;
wPendingError := WORD#0;
uiPendingStage := UINT#20 + uiPhase * UINT#10;

pSendNow := pCommand;
uiSendNow := UINT#0;
IF uiStep = UINT#40 THEN
    pSendNow := pCommand + uiCommandIndex;
    uiSendNow := uiCommandLength - uiCommandIndex;
END_IF;
fbControlClient(xExecute := uiStep = UINT#10, diConnectID := cdiControlID,
    stServerAddress := stControlServer);
fbControlIo(xSend := uiStep = UINT#40,
    xReceive := (uiStep = UINT#20) AND (uiControlRxIndex >= uiControlRxCount),
    diConnectID := cdiControlID, pSend := pSendNow, uiSendSize := uiSendNow);
IF fbControlIo.uiCount > UINT#0 THEN
    uiControlRxIndex := UINT#0;
    uiControlRxCount := fbControlIo.uiCount;
END_IF;
pSendNow := pBufferRun;
uiSendNow := UINT#0;
IF (uiStep = UINT#60) AND (udiUploadedBytes < udiExpectedBytes) THEN
    pSendNow := pBufferRun + udiUploadedBytes;
    IF udiExpectedBytes - udiUploadedBytes > UDINT#8192 THEN
        uiSendNow := UINT#8192;
    ELSE
        uiSendNow := UDINT_TO_UINT(udiExpectedBytes - udiUploadedBytes);
    END_IF;
END_IF;
fbDataClient(xExecute := uiStep = UINT#50, diConnectID := cdiDataID,
    stServerAddress := stDataServer);
fbDataIo(xSend := (uiStep = UINT#60) AND (uiSendNow > UINT#0),
    xReceive := uiStep = UINT#70, diConnectID := cdiDataID,
    pSend := pSendNow, uiSendSize := uiSendNow);
fbDataClose(xExecute := ((uiStep = UINT#80) OR (uiStep = UINT#90))
    AND xDataOwned AND NOT xDataCloseFailed, diConnectID := cdiDataID);
fbControlClose(xExecute := (uiStep = UINT#95) AND xControlOwned,
    diConnectID := cdiControlID);
fbReplyTimeout(IN := uiStep = UINT#20, PT := T#20s);
fbCommandTimeout(IN := (uiStep = UINT#40) OR (uiStep = UINT#45), PT := T#30s);
xProgress := ((uiStep = UINT#60) AND (fbDataIo.uiSent > UINT#0))
    OR ((uiStep = UINT#70) AND (fbDataIo.uiCount > UINT#0));
fbUploadTimeout(IN := ((uiStep = UINT#60) OR (uiStep = UINT#65)) AND NOT xProgress,
    PT := T#30s);
fbReadbackTimeout(IN := (uiStep = UINT#70) AND NOT xProgress, PT := T#30s);
fbDataCloseTimeout(IN := ((uiStep = UINT#80) OR (uiStep = UINT#90))
    AND xDataOwned AND NOT xDataCloseFailed, PT := T#5s);
fbControlCloseTimeout(IN := uiStep = UINT#95, PT := T#5s);

(* Bounded control-reply parser; identical-code SPACE terminates multiline replies. *)
IF (uiStep = UINT#20) AND NOT fbControlIo.xError THEN
    IF uiControlRxIndex < uiControlRxCount THEN
    FOR uiParse := uiControlRxIndex TO uiControlRxCount - UINT#1 DO
      IF (uiControlRxIndex < uiControlRxCount) AND NOT xReplyReady AND NOT xFail THEN
        byControl := fbControlIo.aRecv[uiControlRxIndex];
        uiControlRxIndex := uiControlRxIndex + UINT#1;
        IF byControl = BYTE#10 THEN
            IF uiLineLength > UINT#0 THEN
                IF pLine[uiLineLength - UINT#1] = BYTE#13 THEN
                    uiLineLength := uiLineLength - UINT#1;
                    pLine[uiLineLength] := BYTE#0;
                END_IF;
            END_IF;
            IF xMultiline THEN
                IF LEFT(sLine, 4) = CONCAT(sMultilineCode, ' ') THEN
                    xMultiline := FALSE;
                    xReplyReady := TRUE;
                END_IF;
            ELSE
                xDigits := FALSE;
                IF uiLineLength >= UINT#4 THEN
                    xDigits := (pLine[0] >= BYTE#48) AND (pLine[0] <= BYTE#57)
                        AND (pLine[1] >= BYTE#48) AND (pLine[1] <= BYTE#57)
                        AND (pLine[2] >= BYTE#48) AND (pLine[2] <= BYTE#57);
                END_IF;
                IF xDigits THEN
                    IF pLine[3] = BYTE#45 THEN
                        xMultiline := TRUE;
                        sMultilineCode := LEFT(sLine, 3);
                    ELSIF pLine[3] = BYTE#32 THEN
                        xReplyReady := TRUE;
                    ELSE
                        xFail := TRUE;
                        wPendingError := WORD#16#F203;
                    END_IF;
                ELSE
                    xFail := TRUE;
                    wPendingError := WORD#16#F203;
                END_IF;
            END_IF;
            IF xReplyReady THEN
                uiReplyCode := (BYTE_TO_UINT(pLine[0]) - UINT#48) * UINT#100
                    + (BYTE_TO_UINT(pLine[1]) - UINT#48) * UINT#10
                    + BYTE_TO_UINT(pLine[2]) - UINT#48;
                sLastReply := sLine;
            ELSIF xFail THEN
                sLastReply := sLine;
            END_IF;
            sLine := '';
            uiLineLength := UINT#0;
        ELSE
            IF uiLineLength < UINT#255 THEN
                pLine[uiLineLength] := byControl;
                uiLineLength := uiLineLength + UINT#1;
                pLine[uiLineLength] := BYTE#0;
            ELSE
                xLineTruncated := TRUE;
                sLastReply := sLine;
                xFail := TRUE;
                wPendingError := WORD#16#F204;
            END_IF;
        END_IF;
      END_IF;
    END_FOR;
    END_IF;
END_IF;

CASE uiStep OF
    UINT#0:
        IF xStart AND NOT xStartOld THEN
            xAbortLatched := FALSE;
            xDirectoryReady := FALSE;
            xDone := FALSE;
            xTestPassed := FALSE;
            xError := FALSE;
            xLoginOk := FALSE;
            xBaseDirOk := FALSE;
            xScratchCreated := FALSE;
            xUploadComplete := FALSE;
            xReadbackComplete := FALSE;
            xContentMatch := FALSE;
            xQuitOk := FALSE;
            xCleanupOk := FALSE;
            xUploadDataConnected := FALSE;
            xDownloadDataConnected := FALSE;
            xDataCloseFailed := FALSE;
            xLineTruncated := FALSE;
            xReadbackEndCandidate := FALSE;
            xBytesMatch := TRUE;
            uiErrorStage := UINT#0;
            wErrorID := WORD#0;
            wDataCloseErrorID := WORD#0;
            wControlCloseErrorID := WORD#0;
            wReadbackEndStatus := WORD#0;
            udiUploadedBytes := UDINT#0;
            udiReadbackBytes := UDINT#0;
            uiReplyCode := UINT#0;
            uiUserReplyCode := UINT#0;
            uiPassReplyCode := UINT#0;
            uiBaseDirReplyCode := UINT#0;
            uiNameCheckReplyCode := UINT#0;
            uiMkdReplyCode := UINT#0;
            uiStorReplyCode := UINT#0;
            uiUploadCompleteCode := UINT#0;
            uiRetrReplyCode := UINT#0;
            uiDownloadCompleteCode := UINT#0;
            uiPasvPort := UINT#0;
            sLastReply := '';
            sMkdReply := '';
            sPasvReply := '';
            sUploadReply := '';
            sDownloadReply := '';
            sLine := '';
            uiLineLength := UINT#0;
            uiControlRxIndex := UINT#0;
            uiControlRxCount := UINT#0;
            xMultiline := FALSE;
            sMultilineCode := '';
            uiPhase := UINT#0;
            uiTransferMode := UINT#0;
            sUserRun := sUser;
            sPasswordRun := sPassword;
            sScratchRun := sProject;
            sFileRun := sFile;
            xReplaceRun := xReplaceFixed;
            xVerifyRun := xVerifyOnly;
            udiVerifyCrcRun := udiVerifyCrc;
            udiReadbackCrc := UDINT#16#FFFFFFFF;
            pBufferRun := pBuffer;
            sRemotePath := CONCAT(CONCAT(CONCAT('/UserPrograms/', sScratchRun), '/'), sFileRun);
            udiExpectedBytes := udiSize;
            uiPendingStage := UINT#1;
            IF (LEN(sUserRun) = 0) OR (LEN(sPasswordRun) = 0) THEN
                xFail := TRUE;
                wPendingError := WORD#16#F200;
            ELSE
                pCredential := ADR(sUserRun);
                FOR uiI := UINT#0 TO INT_TO_UINT(LEN(sUserRun)) - UINT#1 DO
                    IF (pCredential[uiI] < BYTE#32) OR (pCredential[uiI] > BYTE#126) THEN
                        xFail := TRUE;
                        wPendingError := WORD#16#F207;
                    END_IF;
                END_FOR;
                pCredential := ADR(sPasswordRun);
                FOR uiI := UINT#0 TO INT_TO_UINT(LEN(sPasswordRun)) - UINT#1 DO
                    IF (pCredential[uiI] < BYTE#32) OR (pCredential[uiI] > BYTE#126) THEN
                        xFail := TRUE;
                        wPendingError := WORD#16#F207;
                    END_IF;
                END_FOR;
            END_IF;
            IF NOT F_FileNameValid(sScratchRun, TRUE)
                OR NOT F_FileNameValid(sFileRun, FALSE)
                OR (udiExpectedBytes > UDINT#1048576)
                OR (pBufferRun = 0) THEN
                xFail := TRUE;
                wPendingError := WORD#16#F209;
            END_IF;
            IF xVerifyRun AND NOT xReplaceRun THEN
                xFail := TRUE;
                wPendingError := WORD#16#F20C;
            END_IF;
            IF xReplaceRun AND (NOT GVL_FileBridge.xFixedMappingConfirmed
                OR (GVL_RobotBridge.uiFixedJobGuardVersion <> UINT#140)
                OR NOT GVL_FileBridge.xFixedIdle OR (sScratchRun <> GVL_FileBridge.sFixedProject)
                OR NOT F_FixedFileValid(sFileRun)) THEN
                xFail := TRUE;
                wPendingError := WORD#16#F20C;
            END_IF;
            IF NOT xFail THEN
                uiStep := UINT#10;
            END_IF;
        END_IF;

    UINT#10:
        IF fbControlClient.xError THEN
            xFail := TRUE;
            wPendingError := fbControlClient.wErrorID;
            uiPendingStage := UINT#10;
        ELSIF fbControlClient.xDone THEN
            xControlOwned := TRUE;
            uiStep := UINT#20;
        END_IF;

    UINT#20:
        IF fbControlIo.xError OR fbControlIo.xPeerClosed THEN
            xFail := TRUE;
            wPendingError := WORD#16#F224;
        ELSIF NOT xFail THEN
            IF xReplyReady THEN
                xReplyAccepted := FALSE;
                CASE uiPhase OF
                    UINT#0:
                        IF uiReplyCode = UINT#220 THEN
                            xReplyAccepted := TRUE;
                            uiNextCommand := UINT#1;
                            uiStep := UINT#30;
                        ELSIF uiReplyCode = UINT#140 THEN
                            xReplyAccepted := TRUE;
                        END_IF;
                    UINT#1:
                        uiUserReplyCode := uiReplyCode;
                        IF uiReplyCode = UINT#331 THEN
                            xReplyAccepted := TRUE;
                            uiNextCommand := UINT#2;
                            uiStep := UINT#30;
                        ELSIF uiReplyCode = UINT#230 THEN
                            xReplyAccepted := TRUE;
                            xLoginOk := TRUE;
                            uiNextCommand := UINT#3;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#2:
                        uiPassReplyCode := uiReplyCode;
                        IF uiReplyCode = UINT#230 THEN
                            xReplyAccepted := TRUE;
                            xLoginOk := TRUE;
                            uiNextCommand := UINT#3;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#3:
                        uiBaseDirReplyCode := uiReplyCode;
                        IF uiReplyCode = UINT#250 THEN
                            xReplyAccepted := TRUE;
                            xBaseDirOk := TRUE;
                            IF xCreateProject OR xReplaceRun THEN
                                uiNextCommand := UINT#14;
                            ELSE
                                uiNextCommand := UINT#5;
                            END_IF;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#14:
                        uiNameCheckReplyCode := uiReplyCode;
                        IF uiReplyCode = UINT#550 THEN
                            xReplyAccepted := TRUE;
                            IF xReplaceRun THEN
                                (* Fixed binding must already exist: never silently create another target. *)
                                xFail := TRUE;
                                wPendingError := WORD#16#F20B;
                            ELSE
                                uiNextCommand := UINT#4;
                                uiStep := UINT#30;
                            END_IF;
                        ELSIF uiReplyCode = UINT#250 THEN
                            xReplyAccepted := TRUE;
                            IF xReplaceRun THEN
                                xDirectoryReady := TRUE;
                                uiNextCommand := UINT#6;
                                uiStep := UINT#30;
                            ELSE
                                (* Original mode retains its no-overwrite behavior. *)
                                xFail := TRUE;
                                wPendingError := WORD#16#F208;
                            END_IF;
                        END_IF;
                    UINT#4:
                        uiMkdReplyCode := uiReplyCode;
                        sMkdReply := sLastReply;
                        IF uiReplyCode = UINT#257 THEN
                            xReplyAccepted := TRUE;
                            xScratchCreated := TRUE;
                            uiNextCommand := UINT#5;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#5:
                        IF uiReplyCode = UINT#250 THEN
                            xDirectoryReady := TRUE;
                            xReplyAccepted := TRUE;
                            uiNextCommand := UINT#6;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#6:
                        IF uiReplyCode = UINT#200 THEN
                            xReplyAccepted := TRUE;
                            IF xVerifyRun THEN uiNextCommand := UINT#10;
                            ELSE uiNextCommand := UINT#7; END_IF;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#7, UINT#10:
                        IF uiReplyCode = UINT#227 THEN
                            xReplyAccepted := TRUE;
                            sPasvReply := sLastReply;
                            uiStep := UINT#35;
                        END_IF;
                    UINT#8:
                        uiStorReplyCode := uiReplyCode;
                        IF (uiReplyCode = UINT#125) OR (uiReplyCode = UINT#150) THEN
                            xReplyAccepted := TRUE;
                            IF udiExpectedBytes = UDINT#0 THEN
                                (* Empty STOR still needs data EOF, 226 and exact RETR. *)
                                uiStep := UINT#80;
                            ELSE
                                uiStep := UINT#60;
                            END_IF;
                        END_IF;
                    UINT#9:
                        uiUploadCompleteCode := uiReplyCode;
                        sUploadReply := sLastReply;
                        IF (uiReplyCode = UINT#226) OR (uiReplyCode = UINT#250) THEN
                            xReplyAccepted := TRUE;
                            xUploadComplete := TRUE;
                            uiNextCommand := UINT#10;
                            uiStep := UINT#30;
                        END_IF;
                    UINT#11:
                        uiRetrReplyCode := uiReplyCode;
                        IF (uiReplyCode = UINT#125) OR (uiReplyCode = UINT#150) THEN
                            xReplyAccepted := TRUE;
                            uiStep := UINT#70;
                        END_IF;
                    UINT#12:
                        uiDownloadCompleteCode := uiReplyCode;
                        sDownloadReply := sLastReply;
                        IF (uiReplyCode = UINT#226) OR (uiReplyCode = UINT#250) THEN
                            xReplyAccepted := TRUE;
                            xReadbackComplete := TRUE;
                            xContentMatch := xBytesMatch AND xReadbackEndCandidate
                                AND (udiReadbackBytes = udiExpectedBytes);
                            IF xContentMatch THEN
                                uiNextCommand := UINT#13;
                                uiStep := UINT#30;
                            ELSE
                                xFail := TRUE;
                                wPendingError := WORD#16#F205;
                            END_IF;
                        END_IF;
                    UINT#13:
                        IF uiReplyCode = UINT#221 THEN
                            xReplyAccepted := TRUE;
                            xQuitOk := TRUE;
                            uiStep := UINT#90;
                        END_IF;
                END_CASE;
                IF NOT xReplyAccepted THEN
                    xFail := TRUE;
                    wPendingError := WORD#16#F202;
                END_IF;
            ELSIF fbReplyTimeout.Q THEN
                xFail := TRUE;
                wPendingError := WORD#16#F201;
            END_IF;
        END_IF;

    UINT#30:
        uiPhase := uiNextCommand;
        CASE uiNextCommand OF
            UINT#1: sCommand := CONCAT(CONCAT('USER ', sUserRun), '$R$N');
            UINT#2: sCommand := CONCAT(CONCAT('PASS ', sPasswordRun), '$R$N');
            UINT#3: sCommand := 'CWD /UserPrograms$R$N';
            UINT#4: sCommand := CONCAT(CONCAT('MKD ', sScratchRun), '$R$N');
            UINT#5, UINT#14: sCommand := CONCAT(CONCAT('CWD ', sScratchRun), '$R$N');
            UINT#6: sCommand := 'TYPE I$R$N';
            UINT#7, UINT#10: sCommand := 'PASV$R$N';
            UINT#8: sCommand := CONCAT(CONCAT('STOR ', sFileRun), '$R$N');
            UINT#11: sCommand := CONCAT(CONCAT('RETR ', sFileRun), '$R$N');
            UINT#13: sCommand := 'QUIT$R$N';
        ELSE
            xFail := TRUE;
            wPendingError := WORD#16#F20A;
        END_CASE;
        IF NOT xFail THEN
            uiCommandLength := INT_TO_UINT(LEN(sCommand));
            uiCommandIndex := UINT#0;
            uiStep := UINT#40;
        END_IF;

    UINT#35:
        (* Parse six decimal octets in the first parenthesized PASV tuple. *)
        pReply := ADR(sPasvReply);
        xPasvValid := TRUE;
        xPasvInTuple := FALSE;
        xPasvClosed := FALSE;
        xPasvHasDigit := FALSE;
        uiPasvField := UINT#0;
        uiPasvValue := UINT#0;
        FOR uiI := UINT#0 TO INT_TO_UINT(LEN(sPasvReply)) - UINT#1 DO
            IF NOT xPasvInTuple THEN
                IF pReply[uiI] = BYTE#40 THEN
                    xPasvInTuple := TRUE;
                END_IF;
            ELSIF NOT xPasvClosed THEN
                IF (pReply[uiI] >= BYTE#48) AND (pReply[uiI] <= BYTE#57) THEN
                    xPasvHasDigit := TRUE;
                    IF uiPasvValue <= UINT#255 THEN
                        uiPasvValue := uiPasvValue * UINT#10 + BYTE_TO_UINT(pReply[uiI]) - UINT#48;
                        IF uiPasvValue > UINT#255 THEN
                            xPasvValid := FALSE;
                        END_IF;
                    ELSE
                        xPasvValid := FALSE;
                    END_IF;
                ELSIF (pReply[uiI] = BYTE#44) OR (pReply[uiI] = BYTE#41) THEN
                    IF NOT xPasvHasDigit OR (uiPasvField > UINT#5) OR (uiPasvValue > UINT#255) THEN
                        xPasvValid := FALSE;
                    ELSE
                        aPasv[uiPasvField] := uiPasvValue;
                        uiPasvField := uiPasvField + UINT#1;
                    END_IF;
                    uiPasvValue := UINT#0;
                    xPasvHasDigit := FALSE;
                    IF pReply[uiI] = BYTE#41 THEN
                        xPasvClosed := TRUE;
                    END_IF;
                ELSE
                    xPasvValid := FALSE;
                END_IF;
            END_IF;
        END_FOR;
        IF NOT xPasvInTuple OR NOT xPasvClosed OR (uiPasvField <> UINT#6) THEN
            xPasvValid := FALSE;
        END_IF;
        IF xPasvValid THEN
            uiPasvPort := aPasv[4] * UINT#256 + aPasv[5];
            IF (uiPasvPort = UINT#0) OR (uiPasvPort = UINT#21) THEN
                xPasvValid := FALSE;
            END_IF;
        END_IF;
        IF xPasvValid THEN
            (* Data connections are pinned to the control server, not a third-party PASV IP. *)
            stDataServer.strIPAddr := stControlServer.strIPAddr;
            stDataServer.uiPort := uiPasvPort;
            xDataCloseFailed := FALSE;
            IF uiPhase = UINT#7 THEN
                uiTransferMode := UINT#1;
            ELSE
                uiTransferMode := UINT#2;
            END_IF;
            uiStep := UINT#50;
        ELSE
            xFail := TRUE;
            wPendingError := WORD#16#F20B;
        END_IF;

    UINT#40:
        IF fbControlIo.xError THEN
            xFail := TRUE;
            wPendingError := fbControlIo.wErrorID;
        ELSIF fbControlIo.uiSent > UINT#0 THEN
            uiCommandIndex := uiCommandIndex + fbControlIo.uiSent;
            IF uiCommandIndex = uiCommandLength THEN
                sCommand := '';
                uiStep := UINT#20;
            END_IF;
        ELSIF fbCommandTimeout.Q THEN
            xFail := TRUE;
            wPendingError := WORD#16#F206;
        END_IF;

    UINT#50:
        uiPendingStage := UINT#200 + uiTransferMode;
        IF fbDataClient.xError THEN
            xFail := TRUE;
            wPendingError := fbDataClient.wErrorID;
        ELSIF fbDataClient.xDone THEN
            xDataOwned := TRUE;
            IF uiTransferMode = UINT#1 THEN
                xUploadDataConnected := TRUE;
                uiNextCommand := UINT#8;
            ELSE
                xDownloadDataConnected := TRUE;
                uiNextCommand := UINT#11;
            END_IF;
            uiStep := UINT#30;
        END_IF;

    UINT#60:
        uiPendingStage := UINT#210;
        IF fbDataIo.xError THEN
            xFail := TRUE;
            wPendingError := fbDataIo.wErrorID;
        ELSIF fbDataIo.uiSent > UINT#0 THEN
            udiUploadedBytes := udiUploadedBytes + UINT_TO_UDINT(fbDataIo.uiSent);
            IF udiUploadedBytes = udiExpectedBytes THEN uiStep := UINT#80; END_IF;
        ELSIF fbUploadTimeout.Q THEN
            xFail := TRUE;
            wPendingError := WORD#16#F206;
        END_IF;

    UINT#70:
        uiPendingStage := UINT#220;
        IF fbDataIo.xError THEN
            xFail := TRUE;
            wPendingError := fbDataIo.wErrorID;
        ELSIF fbDataIo.uiCount > UINT#0 THEN
            IF UINT_TO_UDINT(fbDataIo.uiCount) > udiExpectedBytes - udiReadbackBytes THEN
                xBytesMatch := FALSE;
                xFail := TRUE;
                wPendingError := WORD#16#F205;
            ELSE
                IF xVerifyRun THEN
                    udiReadbackCrc := F_FileCrc32Fast(ADR(fbDataIo.aRecv), fbDataIo.uiCount, udiReadbackCrc);
                ELSE
                    FOR uiI := UINT#0 TO fbDataIo.uiCount - UINT#1 DO
                        IF fbDataIo.aRecv[uiI] <> pBufferRun[udiReadbackBytes + UINT_TO_UDINT(uiI)] THEN
                            xBytesMatch := FALSE;
                        END_IF;
                    END_FOR;
                END_IF;
                udiReadbackBytes := udiReadbackBytes + UINT_TO_UDINT(fbDataIo.uiCount);
                IF NOT xBytesMatch THEN
                    xFail := TRUE;
                    wPendingError := WORD#16#F205;
                END_IF;
            END_IF;
        ELSIF fbDataIo.xPeerClosed THEN
            IF xVerifyRun AND ((udiReadbackCrc XOR UDINT#16#FFFFFFFF) <> udiVerifyCrcRun) THEN
                xBytesMatch := FALSE;
            END_IF;
            IF xBytesMatch AND (udiReadbackBytes = udiExpectedBytes) THEN
                xReadbackEndCandidate := TRUE;
                uiStep := UINT#80;
            ELSE
                xFail := TRUE;
                wPendingError := WORD#16#F205;
            END_IF;
        ELSIF fbReadbackTimeout.Q THEN
            xFail := TRUE;
            wPendingError := WORD#16#F201;
        END_IF;

    UINT#80:
        IF fbDataClose.xError OR fbDataCloseTimeout.Q THEN
            IF fbDataClose.xError THEN
                wDataCloseErrorID := fbDataClose.wErrorID;
            ELSE
                wDataCloseErrorID := WORD#16#F20C;
            END_IF;
            xDataCloseFailed := TRUE;
            IF NOT xError THEN
                xError := TRUE;
                wErrorID := wDataCloseErrorID;
                uiErrorStage := UINT#290;
            END_IF;
            uiStep := UINT#95;
        ELSIF fbDataClose.xDone THEN
            xDataOwned := FALSE;
            IF uiTransferMode = UINT#1 THEN
                uiPhase := UINT#9;
            ELSE
                uiPhase := UINT#12;
            END_IF;
            uiStep := UINT#20;
        END_IF;

    UINT#90:
        IF NOT xDataOwned OR xDataCloseFailed THEN
            uiStep := UINT#95;
        ELSIF fbDataClose.xError OR fbDataCloseTimeout.Q THEN
            IF fbDataClose.xError THEN
                wDataCloseErrorID := fbDataClose.wErrorID;
            ELSE
                wDataCloseErrorID := WORD#16#F20C;
            END_IF;
            xDataCloseFailed := TRUE;
            IF NOT xError THEN
                xError := TRUE;
                wErrorID := wDataCloseErrorID;
                uiErrorStage := UINT#290;
            END_IF;
            uiStep := UINT#95;
        ELSIF fbDataClose.xDone THEN
            xDataOwned := FALSE;
            uiStep := UINT#95;
        END_IF;

    UINT#95:
        IF NOT xControlOwned THEN
            xDone := TRUE;
            xFinishedNow := TRUE;
            uiStep := UINT#100;
        ELSIF fbControlClose.xError OR fbControlCloseTimeout.Q THEN
            IF fbControlClose.xError THEN
                wControlCloseErrorID := fbControlClose.wErrorID;
            ELSE
                wControlCloseErrorID := WORD#16#F20C;
            END_IF;
            IF NOT xError THEN
                xError := TRUE;
                wErrorID := wControlCloseErrorID;
                uiErrorStage := UINT#295;
            END_IF;
            xDone := TRUE;
            xFinishedNow := TRUE;
            uiStep := UINT#100;
        ELSIF fbControlClose.xDone THEN
            xControlOwned := FALSE;
            xDone := TRUE;
            xFinishedNow := TRUE;
            uiStep := UINT#100;
        END_IF;

    UINT#100:
        IF NOT xStart AND NOT xControlOwned AND NOT xDataOwned THEN
            uiStep := UINT#0;
        END_IF;
END_CASE;

(* Never cancel a pending native connect before its terminal result. *)
IF xAbort AND (uiStep <> UINT#0) AND (uiStep <> UINT#100) THEN
    xAbortLatched := TRUE;
END_IF;
IF xAbortLatched AND (uiStep <> UINT#0) AND (uiStep <> UINT#10)
    AND (uiStep <> UINT#50) AND (uiStep <> UINT#90)
    AND (uiStep <> UINT#95) AND (uiStep <> UINT#100) THEN
    xFail := TRUE;
    wPendingError := WORD#16#F20D;
END_IF;
IF xFail THEN
    IF NOT xError THEN
        xError := TRUE;
        wErrorID := wPendingError;
        uiErrorStage := uiPendingStage;
    END_IF;
    uiStep := UINT#90;
END_IF;

IF xFinishedNow THEN
    xCleanupOk := NOT xControlOwned AND NOT xDataOwned
        AND (wDataCloseErrorID = WORD#0) AND (wControlCloseErrorID = WORD#0);
    xTestPassed := xLoginOk AND xBaseDirOk AND xDirectoryReady
        AND (xVerifyRun OR xUploadDataConnected) AND xDownloadDataConnected
        AND (xVerifyRun OR xUploadComplete) AND xReadbackComplete AND xContentMatch
        AND xQuitOk AND xCleanupOk AND NOT xError;
    sPasswordRun := '';
    sUserRun := '';
    sCommand := '';
END_IF;
xStartOld := xStart;
END_FUNCTION_BLOCK
