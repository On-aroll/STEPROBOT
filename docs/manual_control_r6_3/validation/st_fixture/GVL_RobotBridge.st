// REFERENCE ONLY. Keep the field project declarations and boot counter; do not import.
{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // PC <-> PLC. Inovance native Modbus TCP slave: MWn <-> HR offset n.
    // Commission by reading HR300=16#4D42 and HR301=2 before permitting motion.
    aHostCmd AT %MW200 : ARRAY[0..15] OF WORD;
    aHostStatus AT %MW300 : ARRAY[0..63] OF WORD;

END_VAR

VAR_GLOBAL
    // PLC master -> STEP slave: FC04 / offset 0 / 32 WORD.
    aRobotRx : ARRAY[0..31] OF WORD;
    // PLC master -> STEP slave: ONE FC16 / offset 0 / 2 WORDs.
    // Map BOTH aRobotTx[0] and aRobotTx[1], NOT wRobotTxControl alone.
    aRobotTx : ARRAY[0..1] OF WORD;
    wRobotTxControl : WORD; // first-word diagnostic alias, not a separate channel

    // Local commissioning/operation inputs. Do not map these into aHostCmd.
    xCommissioned : BOOL := FALSE;
    // Set locally ONLY after checking DI49..56 and the GDI program bindings.
    xGdiProgramMappingConfirmed : BOOL := FALSE;
    xRemotePermit : BOOL := FALSE;
    // Optional native master/channel fault. FALSE leaves DO42 watchdog in use.
    xNativeModbusError : BOOL := FALSE;

    // Published live diagnostics.
    xRobotLinkOK : BOOL;
    xRobotRunning : BOOL;
    xRobotPaused : BOOL;
    xRobotStopped : BOOL;
    xRobotEnabled : BOOL;
    xRobotAlarm : BOOL;
    xRobotEmergency : BOOL;
    xRobotExternalAuto : BOOL;
    xHostRegistered : BOOL;
    xHostAlive : BOOL;
    xBusy : BOOL;
    xStopInterlock : BOOL;
    xLoadOperatorConfirmed : BOOL;
END_VAR
