/* SPDX-License-Identifier: LicenseRef-PublicDomain */
/*
 * The oscillator. Plain integer C with no kernel dependencies,
 * so the module and tests/test_wavegen.c run the exact same code.
 *
 * Homage to the MOS 6581 SID: a 24-bit phase accumulator advances by
 * `step` every sample and the waveform is read off its top 8 bits. The
 * SID never had a sine; we cheat with a table. Output is unsigned 8-bit,
 * centered on 128, and naively aliased, also like the SID.
 */
#ifndef WAVEGEN_H
#define WAVEGEN_H

#ifdef __KERNEL__
#include <linux/math64.h>
#include <linux/types.h>
/* 64-by-32 division needs a helper on 32-bit kernels. */
#define wg_div_u64(n, d)	div_u64((n), (d))
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define wg_div_u64(n, d)	((n) / (d))
#endif

#include "sine_table.h"

enum wg_shape { WG_SINE, WG_SAWTOOTH, WG_TRIANGLE, WG_SQUARE, WG_NSHAPES };

#define WG_PHASE_BITS	24
#define WG_PHASE_MASK	((1u << WG_PHASE_BITS) - 1)
#define WG_RATE_MAX	768000
#define WG_AMP_MAX	256

struct wg_osc {
	u32 phase;	/* low WG_PHASE_BITS used */
	u32 step;	/* phase increment per sample, at most 2^23 (Nyquist) */
	u16 amp;	/* 0..WG_AMP_MAX; WG_AMP_MAX is full scale */
	u8 shape;	/* enum wg_shape */
};

/*
 * Retune without touching phase, so the wave carries on from where it
 * is: no click. Returns 0, or -1 if out of range (o is untouched).
 */
static inline int wg_set(struct wg_osc *o, u32 freq, u32 rate, u32 amp)
{
	if (rate == 0 || rate > WG_RATE_MAX ||
	    freq == 0 || freq > rate / 2 || amp > WG_AMP_MAX)
		return -1;
	/* Rounded to nearest. freq <= rate / 2 keeps this <= 2^23. */
	o->step = (u32)wg_div_u64(((u64)freq << WG_PHASE_BITS) + rate / 2,
				  rate);
	o->amp = amp;
	return 0;
}

/* Start at phase 0. Returns 0, or -1 if out of range (o is untouched). */
static inline int wg_init(struct wg_osc *o, unsigned int shape,
			  u32 freq, u32 rate, u32 amp)
{
	struct wg_osc n = { .phase = 0, .shape = shape };

	if (shape >= WG_NSHAPES || wg_set(&n, freq, rate, amp))
		return -1;
	*o = n;
	return 0;
}

/* Full-scale sample (0..255) for the top 8 bits of phase. */
static inline u8 wg_shape_sample(unsigned int shape, u32 phase)
{
	u8 i = phase >> (WG_PHASE_BITS - 8);
	u8 fold;

	switch (shape) {
	case WG_SINE:
		return wg_sine_table[i];
	case WG_SAWTOOTH:
		return i;
	case WG_TRIANGLE:
		/* The SID's trick: XOR the ramp with its own MSB, then double. */
		fold = (i & 0x80) ? (u8)~i : i;
		return fold << 1;
	case WG_SQUARE:
		return (i & 0x80) ? 0 : 255;
	}
	return 128;
}

/* Scale around the 128 midpoint. Integer only: no FPU in the kernel. */
static inline u8 wg_scale(u8 s, u16 amp)
{
	return 128 + ((int)s - 128) * amp / WG_AMP_MAX;
}

/* Write n samples to buf and advance the oscillator. */
static inline void wg_fill(struct wg_osc *o, u8 *buf, size_t n)
{
	u32 phase = o->phase;
	size_t i;

	for (i = 0; i < n; i++) {
		buf[i] = wg_scale(wg_shape_sample(o->shape, phase), o->amp);
		phase = (phase + o->step) & WG_PHASE_MASK;
	}
	o->phase = phase;
}

#endif /* WAVEGEN_H */
