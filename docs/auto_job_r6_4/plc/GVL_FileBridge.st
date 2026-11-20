{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    (* Reserve these MW ranges in the FIELD project before importing. *)
    aCommand AT %MW6000 : ARRAY[0..119] OF WORD;
    aHeartbeat AT %MW6120 : ARRAY[0..4] OF WORD;
    aStatus AT %MW6200 : ARRAY[0..63] OF WORD;
END_VAR
VAR_GLOBAL
    (* LOCAL configuration only. Never map credentials/permit to Modbus. *)
    xFileTransferPermit : BOOL := FALSE;
    xReset : BOOL := FALSE;
    sFtpUser : STRING(64) := '';
    sFtpPassword : STRING(64) := '';
    aBuffer : ARRAY[0..1048575] OF BYTE;
    (* One-time LOCAL commissioning: exact existing project already bound to these manual service and one auto GDI number.
       This mode reuses bindings. It never edits robot system configuration. *)
    xFixedMappingConfirmed : BOOL := FALSE;
    sFixedProject : STRING(64) := 'pcjob_manual_r63.sr';
    uiFixedCapture : UINT := UINT#10;
    uiFixedPath : UINT := UINT#11;
    uiAutoCapacity : UINT := UINT#15;
    (* Nonretained: a reboot or failed update never authorizes a partly updated project. *)
    xFixedUpdateBusy : BOOL;
    xFixedReady : BOOL;
    xFixedIdle : BOOL;
    aAutoTag : ARRAY[0..2] OF WORD;
END_VAR
