FUNCTION F_AutoFileIndex : UINT
VAR_INPUT
    sName : STRING(64);
END_VAR
F_AutoFileIndex := UINT#0;
IF sName = 'auto_main.srd' THEN F_AutoFileIndex := UINT#1; END_IF;
IF sName = 'auto_main.srp' THEN F_AutoFileIndex := UINT#2; END_IF;
IF sName = 'auto_01.srd' THEN F_AutoFileIndex := UINT#3; END_IF;
IF sName = 'auto_01.srp' THEN F_AutoFileIndex := UINT#4; END_IF;
IF sName = 'auto_02.srd' THEN F_AutoFileIndex := UINT#5; END_IF;
IF sName = 'auto_02.srp' THEN F_AutoFileIndex := UINT#6; END_IF;
IF sName = 'auto_03.srd' THEN F_AutoFileIndex := UINT#7; END_IF;
IF sName = 'auto_03.srp' THEN F_AutoFileIndex := UINT#8; END_IF;
IF sName = 'auto_04.srd' THEN F_AutoFileIndex := UINT#9; END_IF;
IF sName = 'auto_04.srp' THEN F_AutoFileIndex := UINT#10; END_IF;
IF sName = 'auto_05.srd' THEN F_AutoFileIndex := UINT#11; END_IF;
IF sName = 'auto_05.srp' THEN F_AutoFileIndex := UINT#12; END_IF;
IF sName = 'auto_06.srd' THEN F_AutoFileIndex := UINT#13; END_IF;
IF sName = 'auto_06.srp' THEN F_AutoFileIndex := UINT#14; END_IF;
IF sName = 'auto_07.srd' THEN F_AutoFileIndex := UINT#15; END_IF;
IF sName = 'auto_07.srp' THEN F_AutoFileIndex := UINT#16; END_IF;
IF sName = 'auto_08.srd' THEN F_AutoFileIndex := UINT#17; END_IF;
IF sName = 'auto_08.srp' THEN F_AutoFileIndex := UINT#18; END_IF;
IF sName = 'auto_09.srd' THEN F_AutoFileIndex := UINT#19; END_IF;
IF sName = 'auto_09.srp' THEN F_AutoFileIndex := UINT#20; END_IF;
IF sName = 'auto_10.srd' THEN F_AutoFileIndex := UINT#21; END_IF;
IF sName = 'auto_10.srp' THEN F_AutoFileIndex := UINT#22; END_IF;
IF sName = 'auto_11.srd' THEN F_AutoFileIndex := UINT#23; END_IF;
IF sName = 'auto_11.srp' THEN F_AutoFileIndex := UINT#24; END_IF;
IF sName = 'auto_12.srd' THEN F_AutoFileIndex := UINT#25; END_IF;
IF sName = 'auto_12.srp' THEN F_AutoFileIndex := UINT#26; END_IF;
IF sName = 'auto_13.srd' THEN F_AutoFileIndex := UINT#27; END_IF;
IF sName = 'auto_13.srp' THEN F_AutoFileIndex := UINT#28; END_IF;
IF sName = 'auto_14.srd' THEN F_AutoFileIndex := UINT#29; END_IF;
IF sName = 'auto_14.srp' THEN F_AutoFileIndex := UINT#30; END_IF;
IF sName = 'auto_15.srd' THEN F_AutoFileIndex := UINT#31; END_IF;
IF sName = 'auto_15.srp' THEN F_AutoFileIndex := UINT#32; END_IF;
END_FUNCTION
