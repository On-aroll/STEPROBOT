(* View only: copy mapped input bytes, without decoding the welding protocol.
   Call once per scan from MainTask. Has no hardware writes or SDO calls. *)
PROGRAM PRG_AtrFeedback
VAR
    abyTx : ARRAY[0..14] OF BYTE;
END_VAR

abyTx[0] := GVL_AtrFeedback.byTx01;
abyTx[1] := GVL_AtrFeedback.byTx02;
abyTx[2] := GVL_AtrFeedback.byTx03;
abyTx[3] := GVL_AtrFeedback.byTx04;
abyTx[4] := GVL_AtrFeedback.byTx05;
abyTx[5] := GVL_AtrFeedback.byTx06;
abyTx[6] := GVL_AtrFeedback.byTx07;
abyTx[7] := GVL_AtrFeedback.byTx08;
abyTx[8] := GVL_AtrFeedback.byTx09;
abyTx[9] := GVL_AtrFeedback.byTx10;
abyTx[10] := GVL_AtrFeedback.byTx11;
abyTx[11] := GVL_AtrFeedback.byTx12;
abyTx[12] := GVL_AtrFeedback.byTx13;
abyTx[13] := GVL_AtrFeedback.byTx14;
abyTx[14] := GVL_AtrFeedback.byTx15;
END_PROGRAM
