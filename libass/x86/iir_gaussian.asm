;******************************************************************************
;* iir_gaussian.asm: SSE and AVX IIR Gaussian blur
;******************************************************************************

%include "x86inc.asm"

SECTION_RODATA 32
low_word_zero: dd 0xFFFF0000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF

SECTION .text

%define horizontal 0
%define vertical 1
%define bytes 0
%define floats 1

; arg1 = register to broadcast to, arg2 = float pointer to broadcast
%macro broadcast_float 2
    %if mmsize == 32
        vbroadcastss %1, %2
    %else
        movss %1, %2
        shufps %1, %1, 0x00
    %endif
%endmacro

%macro clear_mm 1-*
    %if mmsize == 32
        vzeroall
    %else
        %rep %0
            pxor %1, %1
            %rotate 1
        %endrep
    %endif
%endmacro

; arg1 = register number to load to, arg2 = register number 2,
; arg3 = start address, arg4 = stride, arg5 = temp mmreg number,
; arg6 = temp gpr, arg7 = byte or float
%macro load_vertical 7
    xor %6, %6
    %if %7 == bytes
        pxor m%1, m%1
        pxor m%2, m%2
        %define INSERT_INSTRUCTION pinsrb
        %define INSERT_INSTRUCTION2 vinserti128
    %else
        %define INSERT_INSTRUCTION insertps
        %define INSERT_INSTRUCTION2 vinsertf128
    %endif
    INSERT_INSTRUCTION xmm%1, [%3], 0
    INSERT_INSTRUCTION xmm%1, [%3 + %4], 4
    lea %6, [%3 + %4 * 2]
    INSERT_INSTRUCTION xmm%1, xmm%1, [%6], 8
    INSERT_INSTRUCTION xmm%1, xmm%1, [%6 + %4], 12
    lea %6, [%6 + %4 * 2]
    %if mmsize == 32
        INSERT_INSTRUCTION xmm%5, [%6], 0
        INSERT_INSTRUCTION xmm%5, [%6 + %4], 4
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%5, [%6], 8
        INSERT_INSTRUCTION xmm%5, [%6 + %4], 12
        INSERT_INSTRUCTION2 ymm%1, ymm%1, xmm%5, 0x01
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%2, [%6], 0
        INSERT_INSTRUCTION xmm%2, [%6 + %4], 4
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%2, [%6], 8
        INSERT_INSTRUCTION xmm%2, [%6 + %4], 12
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%5, [%6], 0
        INSERT_INSTRUCTION xmm%5, [%6 + %4], 4
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%5, [%6], 8
        INSERT_INSTRUCTION xmm%5, [%6 + %4], 12
        INSERT_INSTRUCTION2 ymm%2, ymm%2, xmm%5, 0x01
    %else
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%2, [%6], 0
        INSERT_INSTRUCTION xmm%2, [%6 + %4], 4
        lea %6, [%6 + %4 * 2]
        INSERT_INSTRUCTION xmm%2, [%6], 8
        INSERT_INSTRUCTION xmm%2, [%6 + %4], 12
    %endif
    %if %7 == bytes
        cvtdq2ps m%1, m%1
        cvtdq2ps m%2, m%2
    %endif
%endmacro

;------------------------------------------------------------------------------
; void gaussian( float *oTemp, uint8_t* id, float *od,
;                intptr_t width, intptr_t height, intptr_t Nwidth,
;                float *a0, float *a1, float *a2,
;                float *a3, float *b1, float *b2,
;                float *cprev, float *cnext );
;------------------------------------------------------------------------------

%macro blur_pass 1
%if %1 == horizontal
    cglobal gaussian_horizontal, 15, 15
%else
    cglobal gaussian_vertical, 15, 15
%endif
    clear_mm m14, m15, m10, m11, m12, m13; skip edge calculations
    xor r14, r14 ; x
.ltr_loop
    ; m0 = currIn ; m1 = currInN
    %if %1 == horizontal
        load_vertical 0, 1, r1, r5, 2, r15, bytes
    %else
        load_vertical 0, 1, r1, r4, 2, r15, floats
    %endif
    ; m2 = currComp ; m3 = currCompN
    broadcast_float m3, [r6]
    vmulps m2, m3, m0
    vmulps m3, m3, m1
    ; m4 = temp1 ; m5 = temp1N
    ; m14 = prevIn ; m15 = prevInN
    broadcast_float m5, [r7]
    vmulps m4, m5, m14
    vmulps m5, m5, m15
    ; m6 = temp2 ; m7 = temp2N
    broadcast_float m7, [r10]
    vmulps m6, m7, m10
    vmulps m7, m7, m11
    ; m8 = temp3 ; m9 = temp3N
    broadcast_float m9, [r11]
    mulps m8, m9, m12
    mulps m9, m9, m13
    addps m2, m4
    addps m3, m5
    addps m6, m8
    addps m7, m9
    ; m10 = prevOut ; m11 = prevOutN ; m12 = prev2Out ; m13 = prev2OutN
    mova m12, m10
    subps m10, m2, m6
    mova m13, m11
    subps m11, m3, m7
    mova m14, m0
    mova m15, m1
    movaps [r0], m10
    movaps [r0 + mmsize], m11
    inc r14
    add r0, mmsize * 2
    %if %1 == horizontal
        inc r1
    %else
        add r1, 4
    %endif
    cmp r14, r3
    jl .ltr_loop
    %if %1 == horizontal
        dec r1
    %else
        sub r1, 4
    %endif
    sub r0, mmsize * 2
    lea r15, [r3 * 4 - 4]
    imul r15, r4
    add r2, r15
    clear_mm m14, m15, m10, m11, m12, m13, m0, m1; skip edge calculations
    %if %1 == horizontal
        lea r14, [r3 - 1]
    %else
        lea r14, [r4 - 1]
    %endif
.rtl_loop
    ; m0 = currIn ; m1 = currInN
    ; m8 = inNext ; m9 = inNextN
    %if %1 == horizontal
        load_vertical 8, 9, r1, r5, 2, r15, bytes
    %else
        load_vertical 8, 9, r1, r4, 2, r15, floats
    %endif
    ; m6 = output ; m7 = outputN
    movaps m6, [r0]
    %if %1 == horizontal
        movaps m7, [r0 + mmsize]
    %endif
    ; m2 = currComp
    broadcast_float m2, [r6]
    vmulps m2, m2, m0
    ; m3 = temp1
    ; m14 = prevIn ; m15 = prevInN
    broadcast_float m3, [r7]
    vmulps m3, m3, m14
    ; m4 = temp2
    broadcast_float m4, [r10]
    vmulps m4, m4, m10
    ; m5 = temp3
    broadcast_float m5, [r11]
    mulps m5, m5, m12
    addps m2, m3
    addps m4, m5
    ; m10 = prevOut ; m11 = prevOutN ; m12 = prev2Out ; m13 = prev2OutN
    mova m12, m10
    subps m10, m2, m4
    mova m14, m0
    mova m0, m8
    addps m6, m10
    %if %1 == vertical
        cvttps2dq m6, m6
        movaps m7, [r0 + mmsize]
    %endif
    ; m2 = currComp
    broadcast_float m2, [r6]
    vmulps m2, m2, m1
    ; m3 = temp1
    broadcast_float m3, [r7]
    vmulps m3, m3, m15
    ; m4 = temp2
    broadcast_float m4, [r10]
    vmulps m4, m4, m11
    ; m5 = temp3
    broadcast_float m5, [r11]
    mulps m5, m5, m13
    addps m2, m3
    addps m4, m5
    ; m10 = prevOut ; m11 = prevOutN ; m12 = prev2Out ; m13 = prev2OutN
    mova m13, m11
    subps m11, m2, m4
    mova m15, m1
    mova m1, m7
    addps m7, m11
    dec r14
    %if %1 == horizontal
        movntps [r2], m6
        movntps [r2 + mmsize], m7
        dec r1
    %else
        cvttps2dq m7, m7
        %if mmsize == 32
            vextracti128 xm4, m6, 1
            vextracti128 xm3, m7, 1
            packusdw m5, m6, m4
            packusdw m6, m7, m3
            packuswb xm3, xm5, xm6
            mova [r2], xm3
        %else

        %endif
        sub r1, 4
    %endif
    lea r0, [r0 + -mmsize * 2]
    sub r2, r4
    cmp r14, 0
    jge .rtl_loop
    RET
%endmacro

INIT_YMM avx
blur_pass horizontal
blur_pass vertical

INIT_XMM sse4
blur_pass horizontal
blur_pass vertical
