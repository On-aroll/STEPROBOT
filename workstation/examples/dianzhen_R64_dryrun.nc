; R6.4 DRY RUN ONLY - derived from dianzhen.txt
; No extrusion, welding output, machine homing or origin capture.
; XYZ and F retained. Coordinates use the saved robot experiment origin.
; Select omit process outputs for the retained M05 before conversion.
G21 G17 G94 G91.1 ; mm, XY plane, mm/min and relative arc centres
;=============================================
; Octet Lattice 3x3x3  Continuous Direct-Write Gcode
; Unit cell: 12x12x12 mm, 6 struts per cell
; Overall size: 36 x 36 x 36 mm
; FOR METAL DED ROBOT ARM
; Extrusion speed: 15 mm/s → F900
; Travel speed: F1200
; Strut len = 8.4853 mm, E per strut = 0.3394
; E_per_mm = 0.04
;=============================================
; DRY RUN: M82 omitted - no extruder mode is configured.
G90                    ; Absolute Coordinates
; DRY RUN: G28 omitted - no machine homing; use saved robot origin.
G1 Z2.0 F1200          ; Safe Z height
G1 X0 Y0 Z2.0 F1200
; DRY RUN: G92 E0 omitted - no extruder coordinate is configured.
G4 P1 ; 1 second; original G4 P1000 was documented as Wait 1s
;=============================================
; Cell index i (X), j (Y), k (Z): 0,1,2
; Cell center: cx = i*12 +6, cy = j*12 +6, cz = k*12 +6
; 6 strut vectors: (6,6,0), (6,-6,0), (6,0,6), (6,0,-6), (0,6,6), (0,6,-6)
;=============================================
; ===== k=0 (Z layer 0, cz=6) =====
; i=0,j=0, cx=6,cy=6,cz=6
G1 X12 Y12 Z6 F1200
G1 X0 Y0 Z6  F900 ; DRY RUN: E0.3394 omitted; no extrusion
G1 X0 Y12 Z6 F1200
G1 X12 Y0 Z6  F900 ; DRY RUN: E0.6788 omitted; no extrusion
G1 X12 Y6 Z12 F1200
G1 X0 Y6 Z0  F900 ; DRY RUN: E1.0182 omitted; no extrusion
G1 X12 Y6 Z0 F1200
G1 X0 Y6 Z12  F900 ; DRY RUN: E1.3576 omitted; no extrusion
G1 X6 Y12 Z12 F1200
G1 X6 Y0 Z0  F900 ; DRY RUN: E1.6970 omitted; no extrusion
G1 X6 Y12 Z0 F1200
G1 X6 Y0 Z12  F900 ; DRY RUN: E2.0364 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=1, cx=6,cy=18,cz=6
G1 X12 Y24 Z6 F1200
G1 X0 Y12 Z6  F900 ; DRY RUN: E2.3758 omitted; no extrusion
G1 X0 Y24 Z6 F1200
G1 X12 Y12 Z6  F900 ; DRY RUN: E2.7152 omitted; no extrusion
G1 X12 Y18 Z12 F1200
G1 X0 Y18 Z0  F900 ; DRY RUN: E3.0546 omitted; no extrusion
G1 X12 Y18 Z0 F1200
G1 X0 Y18 Z12  F900 ; DRY RUN: E3.3940 omitted; no extrusion
G1 X6 Y24 Z12 F1200
G1 X6 Y12 Z0  F900 ; DRY RUN: E3.7334 omitted; no extrusion
G1 X6 Y24 Z0 F1200
G1 X6 Y12 Z12  F900 ; DRY RUN: E4.0728 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=2, cx=6,cy=30,cz=6
G1 X12 Y36 Z6 F1200
G1 X0 Y24 Z6  F900 ; DRY RUN: E4.4122 omitted; no extrusion
G1 X0 Y36 Z6 F1200
G1 X12 Y24 Z6  F900 ; DRY RUN: E4.7516 omitted; no extrusion
G1 X12 Y30 Z12 F1200
G1 X0 Y30 Z0  F900 ; DRY RUN: E5.0910 omitted; no extrusion
G1 X12 Y30 Z0 F1200
G1 X0 Y30 Z12  F900 ; DRY RUN: E5.4304 omitted; no extrusion
G1 X6 Y36 Z12 F1200
G1 X6 Y24 Z0  F900 ; DRY RUN: E5.7698 omitted; no extrusion
G1 X6 Y36 Z0 F1200
G1 X6 Y24 Z12  F900 ; DRY RUN: E6.1092 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=0, cx=18,cy=6,cz=6
G1 X24 Y12 Z6 F1200
G1 X12 Y0 Z6  F900 ; DRY RUN: E6.4486 omitted; no extrusion
G1 X12 Y12 Z6 F1200
G1 X24 Y0 Z6  F900 ; DRY RUN: E6.7880 omitted; no extrusion
G1 X24 Y6 Z12 F1200
G1 X12 Y6 Z0  F900 ; DRY RUN: E7.1274 omitted; no extrusion
G1 X24 Y6 Z0 F1200
G1 X12 Y6 Z12  F900 ; DRY RUN: E7.4668 omitted; no extrusion
G1 X18 Y12 Z12 F1200
G1 X18 Y0 Z0  F900 ; DRY RUN: E7.8062 omitted; no extrusion
G1 X18 Y12 Z0 F1200
G1 X18 Y0 Z12  F900 ; DRY RUN: E8.1456 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=1, cx=18,cy=18,cz=6
G1 X24 Y24 Z6 F1200
G1 X12 Y12 Z6  F900 ; DRY RUN: E8.4850 omitted; no extrusion
G1 X12 Y24 Z6 F1200
G1 X24 Y12 Z6  F900 ; DRY RUN: E8.8244 omitted; no extrusion
G1 X24 Y18 Z12 F1200
G1 X12 Y18 Z0  F900 ; DRY RUN: E9.1638 omitted; no extrusion
G1 X24 Y18 Z0 F1200
G1 X12 Y18 Z12  F900 ; DRY RUN: E9.5032 omitted; no extrusion
G1 X18 Y24 Z12 F1200
G1 X18 Y12 Z0  F900 ; DRY RUN: E9.8426 omitted; no extrusion
G1 X18 Y24 Z0 F1200
G1 X18 Y12 Z12  F900 ; DRY RUN: E10.1820 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=2, cx=18,cy=30,cz=6
G1 X24 Y36 Z6 F1200
G1 X12 Y24 Z6  F900 ; DRY RUN: E10.5214 omitted; no extrusion
G1 X12 Y36 Z6 F1200
G1 X24 Y24 Z6  F900 ; DRY RUN: E10.8608 omitted; no extrusion
G1 X24 Y30 Z12 F1200
G1 X12 Y30 Z0  F900 ; DRY RUN: E11.2002 omitted; no extrusion
G1 X24 Y30 Z0 F1200
G1 X12 Y30 Z12  F900 ; DRY RUN: E11.5396 omitted; no extrusion
G1 X18 Y36 Z12 F1200
G1 X18 Y24 Z0  F900 ; DRY RUN: E11.8790 omitted; no extrusion
G1 X18 Y36 Z0 F1200
G1 X18 Y24 Z12  F900 ; DRY RUN: E12.2184 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=0, cx=30,cy=6,cz=6
G1 X36 Y12 Z6 F1200
G1 X24 Y0 Z6  F900 ; DRY RUN: E12.5578 omitted; no extrusion
G1 X24 Y12 Z6 F1200
G1 X36 Y0 Z6  F900 ; DRY RUN: E12.8972 omitted; no extrusion
G1 X36 Y6 Z12 F1200
G1 X24 Y6 Z0  F900 ; DRY RUN: E13.2366 omitted; no extrusion
G1 X36 Y6 Z0 F1200
G1 X24 Y6 Z12  F900 ; DRY RUN: E13.5760 omitted; no extrusion
G1 X30 Y12 Z12 F1200
G1 X30 Y0 Z0  F900 ; DRY RUN: E13.9154 omitted; no extrusion
G1 X30 Y12 Z0 F1200
G1 X30 Y0 Z12  F900 ; DRY RUN: E14.2548 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=1, cx=30,cy=18,cz=6
G1 X36 Y24 Z6 F1200
G1 X24 Y12 Z6  F900 ; DRY RUN: E14.5942 omitted; no extrusion
G1 X24 Y24 Z6 F1200
G1 X36 Y12 Z6  F900 ; DRY RUN: E14.9336 omitted; no extrusion
G1 X36 Y18 Z12 F1200
G1 X24 Y18 Z0  F900 ; DRY RUN: E15.2730 omitted; no extrusion
G1 X36 Y18 Z0 F1200
G1 X24 Y18 Z12  F900 ; DRY RUN: E15.6124 omitted; no extrusion
G1 X30 Y24 Z12 F1200
G1 X30 Y12 Z0  F900 ; DRY RUN: E15.9518 omitted; no extrusion
G1 X30 Y24 Z0 F1200
G1 X30 Y12 Z12  F900 ; DRY RUN: E16.2912 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=2, cx=30,cy=30,cz=6
G1 X36 Y36 Z6 F1200
G1 X24 Y24 Z6  F900 ; DRY RUN: E16.6306 omitted; no extrusion
G1 X24 Y36 Z6 F1200
G1 X36 Y24 Z6  F900 ; DRY RUN: E16.9700 omitted; no extrusion
G1 X36 Y30 Z12 F1200
G1 X24 Y30 Z0  F900 ; DRY RUN: E17.3094 omitted; no extrusion
G1 X36 Y30 Z0 F1200
G1 X24 Y30 Z12  F900 ; DRY RUN: E17.6488 omitted; no extrusion
G1 X30 Y36 Z12 F1200
G1 X30 Y24 Z0  F900 ; DRY RUN: E17.9882 omitted; no extrusion
G1 X30 Y36 Z0 F1200
G1 X30 Y24 Z12  F900 ; DRY RUN: E18.3276 omitted; no extrusion
G1 Z0.5 F1200

; ===== k=1 (Z layer 1, cz=18) =====
; i=0,j=0, cx=6,cy=6,cz=18
G1 X12 Y12 Z18 F1200
G1 X0 Y0 Z18  F900 ; DRY RUN: E18.6670 omitted; no extrusion
G1 X0 Y12 Z18 F1200
G1 X12 Y0 Z18  F900 ; DRY RUN: E19.0064 omitted; no extrusion
G1 X12 Y6 Z24 F1200
G1 X0 Y6 Z12  F900 ; DRY RUN: E19.3458 omitted; no extrusion
G1 X12 Y6 Z12 F1200
G1 X0 Y6 Z24  F900 ; DRY RUN: E19.6852 omitted; no extrusion
G1 X6 Y12 Z24 F1200
G1 X6 Y0 Z12  F900 ; DRY RUN: E20.0246 omitted; no extrusion
G1 X6 Y12 Z12 F1200
G1 X6 Y0 Z24  F900 ; DRY RUN: E20.3640 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=1, cx=6,cy=18,cz=18
G1 X12 Y24 Z18 F1200
G1 X0 Y12 Z18  F900 ; DRY RUN: E20.7034 omitted; no extrusion
G1 X0 Y24 Z18 F1200
G1 X12 Y12 Z18  F900 ; DRY RUN: E21.0428 omitted; no extrusion
G1 X12 Y18 Z24 F1200
G1 X0 Y18 Z12  F900 ; DRY RUN: E21.3822 omitted; no extrusion
G1 X12 Y18 Z12 F1200
G1 X0 Y18 Z24  F900 ; DRY RUN: E21.7216 omitted; no extrusion
G1 X6 Y24 Z24 F1200
G1 X6 Y12 Z12  F900 ; DRY RUN: E22.0610 omitted; no extrusion
G1 X6 Y24 Z12 F1200
G1 X6 Y12 Z24  F900 ; DRY RUN: E22.4004 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=2, cx=6,cy=30,cz=18
G1 X12 Y36 Z18 F1200
G1 X0 Y24 Z18  F900 ; DRY RUN: E22.7398 omitted; no extrusion
G1 X0 Y36 Z18 F1200
G1 X12 Y24 Z18  F900 ; DRY RUN: E23.0792 omitted; no extrusion
G1 X12 Y30 Z24 F1200
G1 X0 Y30 Z12  F900 ; DRY RUN: E23.4186 omitted; no extrusion
G1 X12 Y30 Z12 F1200
G1 X0 Y30 Z24  F900 ; DRY RUN: E23.7580 omitted; no extrusion
G1 X6 Y36 Z24 F1200
G1 X6 Y24 Z12  F900 ; DRY RUN: E24.0974 omitted; no extrusion
G1 X6 Y36 Z12 F1200
G1 X6 Y24 Z24  F900 ; DRY RUN: E24.4368 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=0, cx=18,cy=6,cz=18
G1 X24 Y12 Z18 F1200
G1 X12 Y0 Z18  F900 ; DRY RUN: E24.7762 omitted; no extrusion
G1 X12 Y12 Z18 F1200
G1 X24 Y0 Z18  F900 ; DRY RUN: E25.1156 omitted; no extrusion
G1 X24 Y6 Z24 F1200
G1 X12 Y6 Z12  F900 ; DRY RUN: E25.4550 omitted; no extrusion
G1 X24 Y6 Z12 F1200
G1 X12 Y6 Z24  F900 ; DRY RUN: E25.7944 omitted; no extrusion
G1 X18 Y12 Z24 F1200
G1 X18 Y0 Z12  F900 ; DRY RUN: E26.1338 omitted; no extrusion
G1 X18 Y12 Z12 F1200
G1 X18 Y0 Z24  F900 ; DRY RUN: E26.4732 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=1, cx=18,cy=18,cz=18
G1 X24 Y24 Z18 F1200
G1 X12 Y12 Z18  F900 ; DRY RUN: E26.8126 omitted; no extrusion
G1 X12 Y24 Z18 F1200
G1 X24 Y12 Z18  F900 ; DRY RUN: E27.1520 omitted; no extrusion
G1 X24 Y18 Z24 F1200
G1 X12 Y18 Z12  F900 ; DRY RUN: E27.4914 omitted; no extrusion
G1 X24 Y18 Z12 F1200
G1 X12 Y18 Z24  F900 ; DRY RUN: E27.8308 omitted; no extrusion
G1 X18 Y24 Z24 F1200
G1 X18 Y12 Z12  F900 ; DRY RUN: E28.1702 omitted; no extrusion
G1 X18 Y24 Z12 F1200
G1 X18 Y12 Z24  F900 ; DRY RUN: E28.5096 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=2, cx=18,cy=30,cz=18
G1 X24 Y36 Z18 F1200
G1 X12 Y24 Z18  F900 ; DRY RUN: E28.8490 omitted; no extrusion
G1 X12 Y36 Z18 F1200
G1 X24 Y24 Z18  F900 ; DRY RUN: E29.1884 omitted; no extrusion
G1 X24 Y30 Z24 F1200
G1 X12 Y30 Z12  F900 ; DRY RUN: E29.5278 omitted; no extrusion
G1 X24 Y30 Z12 F1200
G1 X12 Y30 Z24  F900 ; DRY RUN: E29.8672 omitted; no extrusion
G1 X18 Y36 Z24 F1200
G1 X18 Y24 Z12  F900 ; DRY RUN: E30.2066 omitted; no extrusion
G1 X18 Y36 Z12 F1200
G1 X18 Y24 Z24  F900 ; DRY RUN: E30.5460 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=0, cx=30,cy=6,cz=18
G1 X36 Y12 Z18 F1200
G1 X24 Y0 Z18  F900 ; DRY RUN: E30.8854 omitted; no extrusion
G1 X24 Y12 Z18 F1200
G1 X36 Y0 Z18  F900 ; DRY RUN: E31.2248 omitted; no extrusion
G1 X36 Y6 Z24 F1200
G1 X24 Y6 Z12  F900 ; DRY RUN: E31.5642 omitted; no extrusion
G1 X36 Y6 Z12 F1200
G1 X24 Y6 Z24  F900 ; DRY RUN: E31.9036 omitted; no extrusion
G1 X30 Y12 Z24 F1200
G1 X30 Y0 Z12  F900 ; DRY RUN: E32.2430 omitted; no extrusion
G1 X30 Y12 Z12 F1200
G1 X30 Y0 Z24  F900 ; DRY RUN: E32.5824 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=1, cx=30,cy=18,cz=18
G1 X36 Y24 Z18 F1200
G1 X24 Y12 Z18  F900 ; DRY RUN: E32.9218 omitted; no extrusion
G1 X24 Y24 Z18 F1200
G1 X36 Y12 Z18  F900 ; DRY RUN: E33.2612 omitted; no extrusion
G1 X36 Y18 Z24 F1200
G1 X24 Y18 Z12  F900 ; DRY RUN: E33.6006 omitted; no extrusion
G1 X36 Y18 Z12 F1200
G1 X24 Y18 Z24  F900 ; DRY RUN: E33.9400 omitted; no extrusion
G1 X30 Y24 Z24 F1200
G1 X30 Y12 Z12  F900 ; DRY RUN: E34.2794 omitted; no extrusion
G1 X30 Y24 Z12 F1200
G1 X30 Y12 Z24  F900 ; DRY RUN: E34.6188 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=2, cx=30,cy=30,cz=18
G1 X36 Y36 Z18 F1200
G1 X24 Y24 Z18  F900 ; DRY RUN: E34.9582 omitted; no extrusion
G1 X24 Y36 Z18 F1200
G1 X36 Y24 Z18  F900 ; DRY RUN: E35.2976 omitted; no extrusion
G1 X36 Y30 Z24 F1200
G1 X24 Y30 Z12  F900 ; DRY RUN: E35.6370 omitted; no extrusion
G1 X36 Y30 Z12 F1200
G1 X24 Y30 Z24  F900 ; DRY RUN: E35.9764 omitted; no extrusion
G1 X30 Y36 Z24 F1200
G1 X30 Y24 Z12  F900 ; DRY RUN: E36.3158 omitted; no extrusion
G1 X30 Y36 Z12 F1200
G1 X30 Y24 Z24  F900 ; DRY RUN: E36.6552 omitted; no extrusion
G1 Z0.5 F1200

; ===== k=2 (Z layer 2, cz=30) =====
; i=0,j=0, cx=6,cy=6,cz=30
G1 X12 Y12 Z30 F1200
G1 X0 Y0 Z30  F900 ; DRY RUN: E36.9946 omitted; no extrusion
G1 X0 Y12 Z30 F1200
G1 X12 Y0 Z30  F900 ; DRY RUN: E37.3340 omitted; no extrusion
G1 X12 Y6 Z36 F1200
G1 X0 Y6 Z24  F900 ; DRY RUN: E37.6734 omitted; no extrusion
G1 X12 Y6 Z24 F1200
G1 X0 Y6 Z36  F900 ; DRY RUN: E38.0128 omitted; no extrusion
G1 X6 Y12 Z36 F1200
G1 X6 Y0 Z24  F900 ; DRY RUN: E38.3522 omitted; no extrusion
G1 X6 Y12 Z24 F1200
G1 X6 Y0 Z36  F900 ; DRY RUN: E38.6916 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=1, cx=6,cy=18,cz=30
G1 X12 Y24 Z30 F1200
G1 X0 Y12 Z30  F900 ; DRY RUN: E39.0310 omitted; no extrusion
G1 X0 Y24 Z30 F1200
G1 X12 Y12 Z30  F900 ; DRY RUN: E39.3704 omitted; no extrusion
G1 X12 Y18 Z36 F1200
G1 X0 Y18 Z24  F900 ; DRY RUN: E39.7098 omitted; no extrusion
G1 X12 Y18 Z24 F1200
G1 X0 Y18 Z36  F900 ; DRY RUN: E40.0492 omitted; no extrusion
G1 X6 Y24 Z36 F1200
G1 X6 Y12 Z24  F900 ; DRY RUN: E40.3886 omitted; no extrusion
G1 X6 Y24 Z24 F1200
G1 X6 Y12 Z36  F900 ; DRY RUN: E40.7280 omitted; no extrusion
G1 Z0.5 F1200

; i=0,j=2, cx=6,cy=30,cz=30
G1 X12 Y36 Z30 F1200
G1 X0 Y24 Z30  F900 ; DRY RUN: E41.0674 omitted; no extrusion
G1 X0 Y36 Z30 F1200
G1 X12 Y24 Z30  F900 ; DRY RUN: E41.4068 omitted; no extrusion
G1 X12 Y30 Z36 F1200
G1 X0 Y30 Z24  F900 ; DRY RUN: E41.7462 omitted; no extrusion
G1 X12 Y30 Z24 F1200
G1 X0 Y30 Z36  F900 ; DRY RUN: E42.0856 omitted; no extrusion
G1 X6 Y36 Z36 F1200
G1 X6 Y24 Z24  F900 ; DRY RUN: E42.4250 omitted; no extrusion
G1 X6 Y36 Z24 F1200
G1 X6 Y24 Z36  F900 ; DRY RUN: E42.7644 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=0, cx=18,cy=6,cz=30
G1 X24 Y12 Z30 F1200
G1 X12 Y0 Z30  F900 ; DRY RUN: E43.1038 omitted; no extrusion
G1 X12 Y12 Z30 F1200
G1 X24 Y0 Z30  F900 ; DRY RUN: E43.4432 omitted; no extrusion
G1 X24 Y6 Z36 F1200
G1 X12 Y6 Z24  F900 ; DRY RUN: E43.7826 omitted; no extrusion
G1 X24 Y6 Z24 F1200
G1 X12 Y6 Z36  F900 ; DRY RUN: E44.1220 omitted; no extrusion
G1 X18 Y12 Z36 F1200
G1 X18 Y0 Z24  F900 ; DRY RUN: E44.4614 omitted; no extrusion
G1 X18 Y12 Z24 F1200
G1 X18 Y0 Z36  F900 ; DRY RUN: E44.8008 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=1, cx=18,cy=18,cz=30
G1 X24 Y24 Z30 F1200
G1 X12 Y12 Z30  F900 ; DRY RUN: E45.1402 omitted; no extrusion
G1 X12 Y24 Z30 F1200
G1 X24 Y12 Z30  F900 ; DRY RUN: E45.4796 omitted; no extrusion
G1 X24 Y18 Z36 F1200
G1 X12 Y18 Z24  F900 ; DRY RUN: E45.8190 omitted; no extrusion
G1 X24 Y18 Z24 F1200
G1 X12 Y18 Z36  F900 ; DRY RUN: E46.1584 omitted; no extrusion
G1 X18 Y24 Z36 F1200
G1 X18 Y12 Z24  F900 ; DRY RUN: E46.4978 omitted; no extrusion
G1 X18 Y24 Z24 F1200
G1 X18 Y12 Z36  F900 ; DRY RUN: E46.8372 omitted; no extrusion
G1 Z0.5 F1200

; i=1,j=2, cx=18,cy=30,cz=30
G1 X24 Y36 Z30 F1200
G1 X12 Y24 Z30  F900 ; DRY RUN: E47.1766 omitted; no extrusion
G1 X12 Y36 Z30 F1200
G1 X24 Y24 Z30  F900 ; DRY RUN: E47.5160 omitted; no extrusion
G1 X24 Y30 Z36 F1200
G1 X12 Y30 Z24  F900 ; DRY RUN: E47.8554 omitted; no extrusion
G1 X24 Y30 Z24 F1200
G1 X12 Y30 Z36  F900 ; DRY RUN: E48.1948 omitted; no extrusion
G1 X18 Y36 Z36 F1200
G1 X18 Y24 Z24  F900 ; DRY RUN: E48.5342 omitted; no extrusion
G1 X18 Y36 Z24 F1200
G1 X18 Y24 Z36  F900 ; DRY RUN: E48.8736 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=0, cx=30,cy=6,cz=30
G1 X36 Y12 Z30 F1200
G1 X24 Y0 Z30  F900 ; DRY RUN: E49.2130 omitted; no extrusion
G1 X24 Y12 Z30 F1200
G1 X36 Y0 Z30  F900 ; DRY RUN: E49.5524 omitted; no extrusion
G1 X36 Y6 Z36 F1200
G1 X24 Y6 Z24  F900 ; DRY RUN: E49.8918 omitted; no extrusion
G1 X36 Y6 Z24 F1200
G1 X24 Y6 Z36  F900 ; DRY RUN: E50.2312 omitted; no extrusion
G1 X30 Y12 Z36 F1200
G1 X30 Y0 Z24  F900 ; DRY RUN: E50.5706 omitted; no extrusion
G1 X30 Y12 Z24 F1200
G1 X30 Y0 Z36  F900 ; DRY RUN: E50.9100 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=1, cx=30,cy=18,cz=30
G1 X36 Y24 Z30 F1200
G1 X24 Y12 Z30  F900 ; DRY RUN: E51.2494 omitted; no extrusion
G1 X24 Y24 Z30 F1200
G1 X36 Y12 Z30  F900 ; DRY RUN: E51.5888 omitted; no extrusion
G1 X36 Y18 Z36 F1200
G1 X24 Y18 Z24  F900 ; DRY RUN: E51.9282 omitted; no extrusion
G1 X36 Y18 Z24 F1200
G1 X24 Y18 Z36  F900 ; DRY RUN: E52.2676 omitted; no extrusion
G1 X30 Y24 Z36 F1200
G1 X30 Y12 Z24  F900 ; DRY RUN: E52.6070 omitted; no extrusion
G1 X30 Y24 Z24 F1200
G1 X30 Y12 Z36  F900 ; DRY RUN: E52.9464 omitted; no extrusion
G1 Z0.5 F1200

; i=2,j=2, cx=30,cy=30,cz=30
G1 X36 Y36 Z30 F1200
G1 X24 Y24 Z30  F900 ; DRY RUN: E53.2858 omitted; no extrusion
G1 X24 Y36 Z30 F1200
G1 X36 Y24 Z30  F900 ; DRY RUN: E53.6252 omitted; no extrusion
G1 X36 Y30 Z36 F1200
G1 X24 Y30 Z24  F900 ; DRY RUN: E53.9646 omitted; no extrusion
G1 X36 Y30 Z24 F1200
G1 X24 Y30 Z36  F900 ; DRY RUN: E54.3040 omitted; no extrusion
G1 X30 Y36 Z36 F1200
G1 X30 Y24 Z24  F900 ; DRY RUN: E54.6434 omitted; no extrusion
G1 X30 Y36 Z24 F1200
G1 X30 Y24 Z36  F900 ; DRY RUN: E54.9828 omitted; no extrusion
G1 Z0.5 F1200

; End Program
G1 Z5.0 F1200
M05
M30
