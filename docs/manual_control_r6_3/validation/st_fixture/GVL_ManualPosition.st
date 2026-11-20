{attribute 'qualified_only'}
VAR_GLOBAL PERSISTENT RETAIN
    // Read-only position feedback to the PC. No robot or local permit writes.
    // MW450 requires both attributes. The bridge rebuilds this block every scan.
    aStatus AT %MW450 : ARRAY[0..39] OF WORD;
END_VAR
