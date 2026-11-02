/*
 * Copyright (C) 2026 libass contributors
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "asm.S"

.arch armv8-a+sme2

/*
 * int16_t bitmaps retain the NEON layout: eight-pixel vertical stripes, with
 * coverage in [0, 0x4000]. Each ZA0.S tile computes sixteen filter positions
 * for sixteen independent pixels. The symmetric filter can be written as
 *
 *   (center * (65536 - 2 * sum(param)) +
 *    sum(param[k] * (left[k] + right[k])) + 32768) >> 16.
 *
 * UMOPA handles the unsigned center coefficient even above INT16_MAX. Zero,
 * negative and excessively large coefficients use the existing NEON path.
 * These ordinary non-streaming, private-ZA functions also fall back for small
 * bitmaps or an SVL other than 512 bits. RDSVL is checked on every call since
 * streaming vector length is a per-thread setting on Linux.
 *
 * Register allocation after coefficient setup:
 *   x0/x1  current destination/source stripe bases
 *   x2/x3  source stripe count / height
 *   x4     source stripe size in bytes (16 * height)
 *   x19    current stripe index
 *   x20    horizontal: output stripes; vertical: output stripe size in bytes
 *   x21    first row of the current sixteen-row batch
 *   x22/23 destination row pointers in the two output stripes
 *   x24    whether the second stripe exists
 *   x25    vertical output height
 *   x26    source stripe size in halfwords (8 * height)
 *   x28    number of rows to store in the current tile
 *   z0-z8  load/transpose/output temporaries
 *   z14    vertical input permutation
 *   z16+   packed coefficient columns, one vector per pair of input samples
 *   p0.h/p7.s = all lanes; p1.s = eight output pixels; p2.h = sixteen halfwords
 */

const words_transpose_sme2, align=6
    .dc.w 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15
    .dcb.w 16, 32
endconst

/*
 * Build one 16 x 2 column of the Toeplitz coefficient matrix in a Z register.
 * z2.h contains 2*pair + k - row - radius, for row=0..15 and k=0..1.
 * The coefficient lookup table in z0 has zeros after param[radius-1].
 */
.macro make_coeff reg
    abs z3.h, p0/m, z2.h
    tbl \reg\().h, {z0.h}, z3.h
    add z2.h, z2.h, #2
.endm

.macro setup_coeff n
    ptrue p0.h
    ptrue p3.h, vl\n
    ld1h {z1.h}, p3/z, [x4]
    neg w5, w5, lsl 1
    mov z0.h, w5                 // low halfword = 65536 - 2 * sum(param)
    ptrue p3.h, vl1
    splice z0.h, p3, z0.h, z1.h // [center, param[0], ..., param[n-1], 0, ...]
    index z1.h, #0, #1
    mov z2.d, z1.d
    and z2.h, z2.h, #1
    lsr z1.h, z1.h, #1
    sub z2.h, z2.h, z1.h
    sub z2.h, z2.h, #\n
    make_coeff z16
    make_coeff z17
    make_coeff z18
    make_coeff z19
    make_coeff z20
    make_coeff z21
    make_coeff z22
    make_coeff z23
    make_coeff z24
    make_coeff z25
    make_coeff z26
    make_coeff z27
.if \n >= 5
    make_coeff z28
.endif
.if \n >= 6
    make_coeff z29
.endif
.if \n >= 7
    make_coeff z30
.endif
.if \n >= 8
    make_coeff z31
.endif
    ptrue p1.s, vl8
    ptrue p7.s
    ptrue p2.h, vl16
.endm

/*
 * Load sixteen rows from one source stripe, then transpose their four pairs
 * of adjacent columns into four vectors. Each 32-bit word holds two int16_t
 * pixels; eight UZP operations replace a full memory transpose.
 * A negative coefficient register marks a pair outside the filter window.
 */
.macro horz_stripe rel, c0, c1, c2, c3
.if \rel < 0
    sub x6, x19, #-\rel
.else
    add x6, x19, #\rel
.endif
    cmp x6, x2
    b.hs 1f                     // unsigned comparison also rejects negative stripes
.if \rel == -2
    sub x7, x1, x4, lsl 1
.elseif \rel == -1
    sub x7, x1, x4
.elseif \rel == 0
    mov x7, x1
.else
    add x7, x1, x4
.endif
    add x7, x7, x21, lsl 4
    ld1h {z0.h}, p3/z, [x7]
    ld1h {z1.h}, p4/z, [x7, #1, mul vl]
    ld1h {z2.h}, p5/z, [x7, #2, mul vl]
    ld1h {z3.h}, p6/z, [x7, #3, mul vl]
.if \c0 >= 0 || \c2 >= 0
    uzp1 z4.s, z0.s, z1.s
    uzp1 z5.s, z2.s, z3.s
.endif
.if \c1 >= 0 || \c3 >= 0
    uzp2 z6.s, z0.s, z1.s
    uzp2 z7.s, z2.s, z3.s
.endif
.if \c0 >= 0
    uzp1 z8.s, z4.s, z5.s
    umopa za0.s, p0/m, p0/m, z\c0\().h, z8.h
.endif
.if \c1 >= 0
    uzp1 z8.s, z6.s, z7.s
    umopa za0.s, p0/m, p0/m, z\c1\().h, z8.h
.endif
.if \c2 >= 0
    uzp2 z8.s, z4.s, z5.s
    umopa za0.s, p0/m, p0/m, z\c2\().h, z8.h
.endif
.if \c3 >= 0
    uzp2 z8.s, z6.s, z7.s
    umopa za0.s, p0/m, p0/m, z\c3\().h, z8.h
.endif
1:
.endm

/* Load a pair of source rows from each of two stripes into one UMOPA operand.
 * x6 is the signed offset in halfwords within the first stripe. Predicates
 * supply zero above/below the bitmap, including partially valid row pairs.
 */
.macro vert_pair coeff
    whilelt p3.h, x6, x26
    whilelt p4.h, x6, xzr
    bic p3.b, p2/z, p3.b, p4.b
    add x7, x1, x6, lsl 1
    ld1h {z0.h}, p3/z, [x7]
    tbl z0.h, {z0.h}, z14.h
    mov z1.h, #0
    cbz x24, 1f
    add x7, x7, x4
    ld1h {z1.h}, p3/z, [x7]
    tbl z1.h, {z1.h}, z14.h
1:
    splice z0.h, p2, z0.h, z1.h
    umopa za0.s, p0/m, p0/m, \coeff\().h, z0.h
    add x6, x6, #16
.endm

/*
 * void blur(int16_t *dst, const int16_t *src, size_t width, size_t height,
 *           const int16_t *param);
 */
.macro blur_sme2 n, dir, min_height
function blur\n\()_\dir\()16_sme2, export=1
    cmp x2, #64
    b.lo 90f
    cmp x3, #\min_height
    b.lo 90f
    rdsvl x6, #1
    cmp x6, #64
    b.ne 90f

    mov w5, #0
.set pos, 0
.rept \n
    ldrsh w6, [x4, #2 * pos]
    tbnz w6, #31, 90f
    add w5, w5, w6
.set pos, pos + 1
.endr
    cbz w5, 90f                 // center coefficient 65536 cannot fit in uint16_t
    cmp w5, #32768
    b.hi 90f

    /* Commit a caller's dormant ZA before using private ZA storage. The
     * AAPCS64 TPIDR2 block contains a buffer pointer, a halfword slice count,
     * and six reserved zero bytes. For an unknown extension, leave TPIDR2
     * untouched and use NEON instead; this is explicitly permitted by the ABI.
     * STR ZA stores one SVL-byte horizontal slice without entering streaming
     * mode. No external SME runtime or compiler-specific function ABI is needed.
     * https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
     */
    mrs x6, TPIDR2_EL0
    cbz x6, 93f
    ldr x7, [x6, #8]
    lsr x8, x7, #16
    cbnz x8, 90f                // unrecognized reserved bytes: preserve dormant ZA
    cmp w7, #64
    b.hi 90f
    ldr x8, [x6]
    cbz x8, 92f
    cbz w7, 92f
    mov w12, #0
91:
    str za[w12, 0], [x8]
    add x8, x8, #64
    add w12, w12, #1
    cmp w12, w7
    b.lo 91b
92:
    msr TPIDR2_EL0, xzr
93:
    // Switching modes clears Z/P registers, including the callee-saved D8-D15.
    stp x19, x20, [sp, #-144]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8, d9, [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]
    smstart
    setup_coeff \n

    lsl x4, x3, #4
    lsl x26, x3, #3
    mov x19, #0
.ifc \dir, horz
    add x20, x2, #2 * \n + 7
    lsr x20, x20, #3
.else
    add x25, x3, #2 * \n
    lsl x20, x25, #4
    movrel x7, words_transpose_sme2
    ld1h {z14.h}, p0/z, [x7]
.endif
    add x2, x2, #7
    lsr x2, x2, #3

10:                             // next pair of output stripes
    add x6, x19, #1
.ifc \dir, horz
    cmp x6, x20
.else
    cmp x6, x2
.endif
    cset w24, lo
    mov x21, #0
11:                             // next batch of sixteen image rows
    zero {za}
    add x22, x0, x21, lsl 4
.ifc \dir, horz
    add x23, x22, x4
    sub x28, x3, x21
    // The same four tail predicates apply to every source stripe in the batch.
    lsl x6, x21, #3
    whilelo p3.h, x6, x26
    add x6, x6, #32
    whilelo p4.h, x6, x26
    add x6, x6, #32
    whilelo p5.h, x6, x26
    add x6, x6, #32
    whilelo p6.h, x6, x26

.if \n == 4
    horz_stripe -1, 16, 17, 18, 19
    horz_stripe  0, 20, 21, 22, 23
    horz_stripe  1, 24, 25, 26, 27
.elseif \n == 5
    horz_stripe -2, -1, -1, -1, 16
    horz_stripe -1, 17, 18, 19, 20
    horz_stripe  0, 21, 22, 23, 24
    horz_stripe  1, 25, 26, 27, 28
.elseif \n == 6
    horz_stripe -2, -1, -1, 16, 17
    horz_stripe -1, 18, 19, 20, 21
    horz_stripe  0, 22, 23, 24, 25
    horz_stripe  1, 26, 27, 28, 29
.elseif \n == 7
    horz_stripe -2, -1, 16, 17, 18
    horz_stripe -1, 19, 20, 21, 22
    horz_stripe  0, 23, 24, 25, 26
    horz_stripe  1, 27, 28, 29, 30
.else
    horz_stripe -2, 16, 17, 18, 19
    horz_stripe -1, 20, 21, 22, 23
    horz_stripe  0, 24, 25, 26, 27
    horz_stripe  1, 28, 29, 30, 31
.endif
.else
    add x23, x22, x20
    sub x28, x25, x21
    sub x6, x21, #2 * \n
    lsl x6, x6, #3
    vert_pair z16
    vert_pair z17
    vert_pair z18
    vert_pair z19
    vert_pair z20
    vert_pair z21
    vert_pair z22
    vert_pair z23
    vert_pair z24
    vert_pair z25
    vert_pair z26
    vert_pair z27
.if \n >= 5
    vert_pair z28
.endif
.if \n >= 6
    vert_pair z29
.endif
.if \n >= 7
    vert_pair z30
.endif
.if \n >= 8
    vert_pair z31
.endif
.endif
    mov x6, #16
    cmp x28, x6
    csel x28, x28, x6, lo
    mov w12, #0
12:
    // Break MOVA's merge dependency on the preceding row, even with all lanes
    // active. Omitting this clear roughly halves throughput on Apple M4.
    mov z0.s, #0
.ifc \dir, horz
    // A vertical ZA slice is one image row across sixteen output columns.
    mova z0.s, p7/m, za0v.s[w12, 0]
.else
    // A horizontal ZA slice is one output row across the two source stripes.
    mova z0.s, p7/m, za0h.s[w12, 0]
.endif
    add z0.s, z0.s, #32768
    lsr z0.s, z0.s, #16
    st1h {z0.s}, p1, [x22]
    cbz x24, 13f
    ext z0.b, z0.b, z0.b, #32
    st1h {z0.s}, p1, [x23]
13:
    add x22, x22, #16
    add x23, x23, #16
    add w12, w12, #1
    cmp x12, x28
    b.lo 12b

    add x21, x21, #16
.ifc \dir, horz
    cmp x21, x3
.else
    cmp x21, x25
.endif
    b.lo 11b
    add x1, x1, x4, lsl 1
.ifc \dir, horz
    add x0, x0, x4, lsl 1
.else
    add x0, x0, x20, lsl 1
.endif
    add x19, x19, #2
.ifc \dir, horz
    cmp x19, x20
.else
    cmp x19, x2
.endif
    b.lo 10b

    smstop
    ldp d8, d9, [sp, #80]
    ldp d10, d11, [sp, #96]
    ldp d12, d13, [sp, #112]
    ldp d14, d15, [sp, #128]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #144
    ret
90:
    b EXTERN\()blur\n\()_\dir\()16_neon
endfunc
.endm

blur_sme2 4, horz, 32
blur_sme2 5, horz, 32
blur_sme2 6, horz, 32
blur_sme2 7, horz, 32
blur_sme2 8, horz, 32
blur_sme2 4, vert, 64
blur_sme2 5, vert, 32
blur_sme2 6, vert, 32
blur_sme2 7, vert, 32
blur_sme2 8, vert, 32
