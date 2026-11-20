FUNCTION F_FileCrc32 : UDINT
VAR_INPUT
    pData : POINTER TO BYTE;
    uiCount : UINT;
    udiSeed : UDINT;
END_VAR
VAR
    udiCrc : UDINT;
    uiI : UINT;
    uiBit : UINT;
END_VAR
(* Running reflected CRC-32. Caller supplies initial FFFFFFFF and final XOR. *)
udiCrc := udiSeed;
IF uiCount > UINT#0 THEN
    FOR uiI := UINT#0 TO uiCount - UINT#1 DO
        udiCrc := udiCrc XOR BYTE_TO_UDINT(pData[uiI]);
        FOR uiBit := UINT#0 TO UINT#7 DO
            IF (udiCrc AND UDINT#1) <> UDINT#0 THEN
                udiCrc := SHR(udiCrc, 1) XOR UDINT#16#EDB88320;
            ELSE
                udiCrc := SHR(udiCrc, 1);
            END_IF;
        END_FOR;
    END_FOR;
END_IF;
F_FileCrc32 := udiCrc;
END_FUNCTION
