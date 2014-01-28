/*
 * Copyright (C) 2013 Rodger Combs <rcombs@rcombs.me>
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

#ifndef INTEL_IIR_GAUSS_BLUR_H
#define INTEL_IIR_GAUSS_BLUR_H

#define GAUSSIAN(suffix) void ass_gaussian_##suffix( float *oTemp,\
    void* id, void *od, intptr_t width, intptr_t height, intptr_t Nwidth, \
    float *a0, float *a1, float *a2, float *a3, float *b1, float *b2, \
    float *cprev, float *cnext );

GAUSSIAN(horizontal_avx)
GAUSSIAN(vertical_avx)
GAUSSIAN(horizontal_sse4)
GAUSSIAN(vertical_sse4)

#endif
