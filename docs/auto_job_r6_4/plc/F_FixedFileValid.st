FUNCTION F_FixedFileValid : BOOL
VAR_INPUT
    sName : STRING(64);
END_VAR
F_FixedFileValid := F_AutoFileIndex(sName) > UINT#0;
END_FUNCTION
