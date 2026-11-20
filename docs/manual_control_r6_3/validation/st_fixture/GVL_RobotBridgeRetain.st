// REFERENCE ONLY. Keep the field project declarations and boot counter; do not import.
{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // Incremented on application initialization, not on every scan.
    // Also require a fresh registration; a cold reset can reset RETAIN memory.
    udiBootCounter : UDINT := 0;
END_VAR
