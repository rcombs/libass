/******************************************************************************
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2015 Martin Storsjo
 * Copyright © 2015 Janne Grunau
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#define PRIVATE_PREFIX checkasm_

#include "aarch64/asm.S"

const register_init, align=4
        .quad 0x21f86d66c8ca00ce
        .quad 0x75b6ba21077c48ad
        .quad 0xed56bb2dcb3c7736
        .quad 0x8bda43d3fd1a7e06
        .quad 0xb64a9c9e5d318408
        .quad 0xdf9a54b303f1d3a3
        .quad 0x4a75479abd64e097
        .quad 0x249214109d5d1c88
        .quad 0x1a1b2550a612b48c
        .quad 0x79445c159ce79064
        .quad 0x2eed899d5a28ddcd
        .quad 0x86b2536fcd8cf636
        .quad 0xb0856806085e7943
        .quad 0x3f2bf84fc0fcca4e
        .quad 0xacbd382dcf5b8de2
        .quad 0xd229e1f5b281303f
        .quad 0x71aeaff20b095fd9
        .quad 0xab63e2e11fa38ed9
endconst


const error_message_register
        .asciz "failed to preserve register"
error_message_stack:
        .asciz "stack clobbered"
#if CONFIG_SME2
error_message_za:
        .asciz "failed to preserve ZA state"
#endif
endconst


#if defined(__BYTE_ORDER__ ) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORD_OFFS 4
#else
#define WORD_OFFS 0
#endif


// max number of args used by any asm function.
#define MAX_ARGS 15

#define CLOBBER_STACK ((8*MAX_ARGS + 15) & ~15)

function stack_clobber, export=1
        mov             x3,  sp
        mov             x2,  #CLOBBER_STACK
1:
        stp             x0,  x1,  [sp, #-16]!
        subs            x2,  x2,  #16
        b.gt            1b
        mov             sp,  x3
        ret
endfunc

// + 16 for stack canary reference
#define ARG_STACK (((8*(MAX_ARGS - 8) + 15) & ~15) + 16)

#define CALL_STATE_SIZE 32
#define CALL_FUNC       (-CALL_STATE_SIZE)
#define CALL_ZA_BLOCK   (CALL_FUNC + 8)
#define CALL_ZA_VL      (CALL_FUNC + 16)
#define CALL_ZA_ERROR   (CALL_FUNC + 24)
#define SAVED_REGS_SIZE (10*8 + 8*8)

#if CONFIG_SME2
.arch armv8-a+sme2

#define TPIDR2_BLOCK_SIZE 16

/*
 * Exercise the private-ZA lazy-save protocol for every checked call, including
 * NEON fallbacks. The non-SME C driver enters with no pending ZA of its own.
 * Each horizontal ZA slice gets a distinct byte pattern. Allocate SVL squared
 * bytes for the save buffer so this also works at other streaming lengths.
 * Run before initializing callee-saved registers; only caller-saved registers
 * are used here and in check_za.
 */
.macro init_za
        movrel          x9, CONCAT(EXTERN, test_za)
        ldr             w9, [x9]
        cbz             w9, .Lza_init_done
        rdsvl           x11, #1
        str             x11, [x29, #CALL_ZA_VL]
        mul             x9, x11, x11
        add             x9, x9, #TPIDR2_BLOCK_SIZE
        sub             sp, sp, x9
        mov             x9, sp
        str             x9, [x29, #CALL_ZA_BLOCK]
        add             x9, x9, #TPIDR2_BLOCK_SIZE
        stp             x9, x11, [sp]  // buffer, num_za_save_slices, reserved zeros
        mov             x10, #1
.Lza_fill_row:
        dup             v0.16b, w10
        mov             x13, x11
.Lza_fill_vector:
        str             q0, [x9], #16
        subs            x13, x13, #16
        b.ne            .Lza_fill_vector
        add             x10, x10, #1
        cmp             x10, x11
        b.ls            .Lza_fill_row

        // Whole-ZA loads/stores are legal with SM=0 and ZA=1.
        smstart         za
        add             x9, sp, #TPIDR2_BLOCK_SIZE
        mov             w12, #0
.Lza_load:
        ldr             za[w12, 0], [x9]
        add             x9, x9, x11
        add             w12, w12, #1
        cmp             x12, x11
        b.lo            .Lza_load
        // Clear the buffer so stale data cannot fake a committed save.
        add             x9, sp, #TPIDR2_BLOCK_SIZE
        mul             x10, x11, x11
.Lza_clear:
        stp             xzr, xzr, [x9], #16
        subs            x10, x10, #16
        b.ne            .Lza_clear
        mov             x9, sp
        msr             TPIDR2_EL0, x9
.Lza_init_done:
.endm

/* Accept either legal return: preserve dormant ZA, or commit its lazy save
 * and return with ZA disabled. Check the TPIDR2 block in both cases, then
 * inspect the saved slices. Clear SME state before reporting any failure to C.
 */
.macro check_za
        ldr             x4, [x29, #CALL_ZA_BLOCK]
        cbz             x4, .Lza_check_done
        mrs             x9, SVCR
        tbnz            x9, #0, .Lza_error // non-streaming interface requires SM=0
        ldr             x6, [x29, #CALL_ZA_VL]
        add             x7, x4, #TPIDR2_BLOCK_SIZE
        ldp             x10, x11, [x4]
        cmp             x10, x7
        b.ne            .Lza_error
        cmp             x11, x6         // includes the reserved zero bytes
        b.ne            .Lza_error
        mrs             x10, TPIDR2_EL0
        cbz             x10, .Lza_committed
        cmp             x10, x4
        b.ne            .Lza_error
        tbz             x9, #1, .Lza_error
        // ZA was preserved: copy its slices to the buffer for comparison.
        mov             x9, x7
        mov             w12, #0
.Lza_store:
        str             za[w12, 0], [x9]
        add             x9, x9, x6
        add             w12, w12, #1
        cmp             x12, x6
        b.lo            .Lza_store
        b               .Lza_verify
.Lza_committed:
        tbnz            x9, #1, .Lza_error
.Lza_verify:
        mov             x14, #0x0101010101010101
        mov             x15, #1
.Lza_check_row:
        and             x11, x15, #255
        mul             x11, x14, x11
        lsr             x8, x6, #4
.Lza_check_vector:
        ldp             x9, x10, [x7], #16
        cmp             x9, x11
        b.ne            .Lza_error
        cmp             x10, x11
        b.ne            .Lza_error
        subs            x8, x8, #1
        b.ne            .Lza_check_vector
        add             x15, x15, #1
        cmp             x15, x6
        b.ls            .Lza_check_row
        b               .Lza_cleanup
.Lza_error:
        mov             x9, #1
        str             x9, [x29, #CALL_ZA_ERROR]
.Lza_cleanup:
        msr             TPIDR2_EL0, xzr
        smstop
.Lza_check_done:
.endm
#endif

function checked_call, export=1
        AARCH64_SIGN_LINK_REGISTER
        stp             x29, x30, [sp, #-16]!
        mov             x29, sp
        stp             x0, xzr, [sp, #-CALL_STATE_SIZE]!
        stp             xzr, xzr, [sp, #16]
        stp             x19, x20, [sp, #-16]!
        stp             x21, x22, [sp, #-16]!
        stp             x23, x24, [sp, #-16]!
        stp             x25, x26, [sp, #-16]!
        stp             x27, x28, [sp, #-16]!
        stp             d8,  d9,  [sp, #-16]!
        stp             d10, d11, [sp, #-16]!
        stp             d12, d13, [sp, #-16]!
        stp             d14, d15, [sp, #-16]!

#if CONFIG_SME2
        init_za
#endif
        movrel          x9, register_init
        ldp             d8,  d9,  [x9], #16
        ldp             d10, d11, [x9], #16
        ldp             d12, d13, [x9], #16
        ldp             d14, d15, [x9], #16
        ldp             x19, x20, [x9], #16
        ldp             x21, x22, [x9], #16
        ldp             x23, x24, [x9], #16
        ldp             x25, x26, [x9], #16
        ldp             x27, x28, [x9], #16

        sub             sp,  sp,  #ARG_STACK
.equ pos, 0
.rept MAX_ARGS-8
        // Skip the first 8 args, that are loaded into registers
        ldr             x9, [x29, #16 + 8*8 + pos]
        str             x9, [sp, #pos]
.equ pos, pos + 8
.endr

        // Fill x8-x17 with garbage. This doesn't have to be preserved,
        // but avoids relying on them having any particular value.
        movrel          x9, register_init
        ldp             x10, x11, [x9], #32
        ldp             x12, x13, [x9], #32
        ldp             x14, x15, [x9], #32
        ldp             x16, x17, [x9], #32
        ldp             x8,  x9,  [x9]

        // For stack overflows, the callee is free to overwrite the parameters
        // that were passed on the stack (if any), so we can only check after
        // that point. First figure out how many parameters the function
        // really took on the stack:
        ldr             w2,  [x29, #16 + 8*8 + (MAX_ARGS-8)*8 + WORD_OFFS]
        // Load the first non-parameter value from the stack, that should be
        // left untouched by the function. Store a copy of it inverted, so that
        // e.g. overwriting everything with zero would be noticed.
        ldr             x2,  [sp, x2, lsl #3]
        mvn             x2,  x2
        str             x2,  [sp, #ARG_STACK-8]

        // Load the in-register arguments
        ldr             x12, [x29, #CALL_FUNC]
        ldp             x0,  x1,  [x29, #16]
        ldp             x2,  x3,  [x29, #32]
        ldp             x4,  x5,  [x29, #48]
        ldp             x6,  x7,  [x29, #64]
        // Call the target function
        blr             x12

        // Load the number of stack parameters, stack canary and its reference
        ldr             w2,  [x29, #16 + 8*8 + (MAX_ARGS-8)*8 + WORD_OFFS]
        ldr             x2,  [sp, x2, lsl #3]
        ldr             x3,  [sp, #ARG_STACK-8]

        add             sp,  sp,  #ARG_STACK
        stp             x0,  x1,  [sp, #-16]!

#if CONFIG_SME2
        check_za
#endif
        mvn             x3,  x3
        cmp             x2,  x3
        b.ne            2f

#if CONFIG_SME2
        ldr             x3, [x29, #CALL_ZA_ERROR]
        cbnz            x3, 3f
#endif
        movrel          x9, register_init
        movi            v3.8h,  #0

.macro check_reg_neon reg1, reg2
        ld1             {v1.2d},  [x9], #16
        uzp1            v2.2d,  v\reg1\().2d, v\reg2\().2d
        eor             v1.16b, v1.16b, v2.16b
        orr             v3.16b, v3.16b, v1.16b
.endm
        check_reg_neon  8,  9
        check_reg_neon  10, 11
        check_reg_neon  12, 13
        check_reg_neon  14, 15
        uqxtn           v3.8b,  v3.8h
        umov            x3,  v3.d[0]

.macro check_reg reg1, reg2
        ldp             x0,  x1,  [x9], #16
        eor             x0,  x0,  \reg1
        eor             x1,  x1,  \reg2
        orr             x3,  x3,  x0
        orr             x3,  x3,  x1
.endm
        check_reg       x19, x20
        check_reg       x21, x22
        check_reg       x23, x24
        check_reg       x25, x26
        check_reg       x27, x28

        cbz             x3,  0f

        movrel          x0, error_message_register
        b               1f
#if CONFIG_SME2
3:
        movrel          x0, error_message_za
        b               1f
#endif
2:
        movrel          x0, error_message_stack
1:
#ifdef PREFIX
        bl              _checkasm_fail_func
#else
        bl              checkasm_fail_func
#endif
0:
        ldp             x0,  x1,  [sp], #16
        sub             sp, x29, #CALL_STATE_SIZE + SAVED_REGS_SIZE
        ldp             d14, d15, [sp], #16
        ldp             d12, d13, [sp], #16
        ldp             d10, d11, [sp], #16
        ldp             d8,  d9,  [sp], #16
        ldp             x27, x28, [sp], #16
        ldp             x25, x26, [sp], #16
        ldp             x23, x24, [sp], #16
        ldp             x21, x22, [sp], #16
        ldp             x19, x20, [sp], #16
        add             sp, sp, #CALL_STATE_SIZE
        ldp             x29, x30, [sp], #16
        AARCH64_VALIDATE_LINK_REGISTER
        ret
endfunc
