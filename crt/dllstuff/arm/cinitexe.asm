;++
;
; Module Name:
;
;    cinitexe.asm
;
; Abstract:
;
;    The bounds of the C++ initializer table of an image that uses crtdll.dll
;    (CRT32\DLLSTUFF\I386\CINITEXE.ASM for ARM). Read-only, as the compiler
;    makes the initializers (.CRT$XCU): the link keeps sections of other
;    attributes apart, and the bounds would then hold none.
;
;--

        AREA    |.CRT$XCA|, DATA, READONLY, ALIGN=2
        EXPORT  __xc_a
__xc_a  DCD     0

        AREA    |.CRT$XCZ|, DATA, READONLY, ALIGN=2
        EXPORT  __xc_z
__xc_z  DCD     0

        END
