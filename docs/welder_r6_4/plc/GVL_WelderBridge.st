{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // AC802 fixed MW registers require both persistence attributes.
    aCommand AT %MW7000 : ARRAY[0..31] OF WORD;
    wHeartbeat AT %MW7064 : WORD;
    aStatus AT %MW7200 : ARRAY[0..63] OF WORD;
END_VAR

VAR_GLOBAL
    // Local commissioning inputs; never write these over the PC command channel.
    xMappingConfirmed : BOOL := FALSE;
    xWeldPermit : BOOL := FALSE;
    // Drive from the real EtherCAT slave OP / working-counter-valid signal.
    // A constant TRUE is not a valid connection-health signal.
    xPdoOperational : BOOL := FALSE;
END_VAR
