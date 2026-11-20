(* Raw TxPDO inputs only, ATR-EtherCAT at the observed 16-byte layout.
   Bind 0x6000:01..0F to byTx01..byTx15 in the slave I/O mapping.
   The final 0x00000008 entry is padding and has no variable.
   No process outputs, fixed addresses, or welding field interpretation. *)
VAR_GLOBAL
    byTx01 : BYTE; (* wire byte 0,  0x6000:01 *)
    byTx02 : BYTE; (* wire byte 1,  0x6000:02 *)
    byTx03 : BYTE; (* wire byte 2,  0x6000:03 *)
    byTx04 : BYTE; (* wire byte 3,  0x6000:04 *)
    byTx05 : BYTE; (* wire byte 4,  0x6000:05 *)
    byTx06 : BYTE; (* wire byte 5,  0x6000:06 *)
    byTx07 : BYTE; (* wire byte 6,  0x6000:07 *)
    byTx08 : BYTE; (* wire byte 7,  0x6000:08 *)
    byTx09 : BYTE; (* wire byte 8,  0x6000:09 *)
    byTx10 : BYTE; (* wire byte 9,  0x6000:0A *)
    byTx11 : BYTE; (* wire byte 10, 0x6000:0B *)
    byTx12 : BYTE; (* wire byte 11, 0x6000:0C *)
    byTx13 : BYTE; (* wire byte 12, 0x6000:0D *)
    byTx14 : BYTE; (* wire byte 13, 0x6000:0E *)
    byTx15 : BYTE; (* wire byte 14, 0x6000:0F *)
END_VAR
