{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // PC reads HR500..539. Reserve this unused range in the field project.
    // The publisher rebuilds all validity metadata; retained values never grant validity.
    aStatus AT %MW500 : ARRAY[0..39] OF WORD;
END_VAR
VAR_GLOBAL
    xEnable : BOOL := TRUE;
    sRobotAddress : STRING(15) := '192.168.3.188';
    uiRobotPort : UINT := UINT#502;
    byRobotUnit : BYTE := BYTE#1;
    // Local commissioning only: compare all six axes with the pendant.
    // FALSE = first register is the low half of IEEE754 REAL.
    xHighWordFirst : BOOL := FALSE;
END_VAR
