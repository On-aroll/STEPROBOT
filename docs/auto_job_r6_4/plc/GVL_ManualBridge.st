{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // PC extension: no overlap with HR200..215, HR300..363 or FTP HR6000+.
    aCommand AT %MW220 : ARRAY[0..15] OF WORD;
    aStatus AT %MW400 : ARRAY[0..31] OF WORD;
END_VAR
VAR_GLOBAL
    // Local configuration. Assign ONE unused GDI slot to manual_service before setting TRUE.
    xInstalled : BOOL := FALSE;
    xServiceBindingConfirmed : BOOL := FALSE;
    uiServiceProgram : UINT := 10;
    // PRG_ManualBridge is the only writer. PRG_RobotBridge reads these requests.
    xActive : BOOL;
    xStartRequest : BOOL;
    xStopRequest : BOOL;
END_VAR
