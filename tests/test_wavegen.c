// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * Userspace tests for wavegen.h. Build and run with `make test`
 * (or `make vm-test` from the Mac).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../wavegen.h"

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "%s:%d: FAIL %s: ", __FILE__, __LINE__, #cond); \
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
	}								\
} while (0)

static const char *const names[WG_NSHAPES] = {
	[WG_SINE] = "sine", [WG_SAWTOOTH] = "sawtooth",
	[WG_TRIANGLE] = "triangle", [WG_SQUARE] = "square",
};

static void test_table(void)
{
	for (int i = 0; i < 128; i++)
		CHECK(wg_sine_table[i] + wg_sine_table[i + 128] == 256,
		      "i=%d %d+%d", i, wg_sine_table[i], wg_sine_table[i + 128]);
	CHECK(wg_sine_table[0] == 128, "%d", wg_sine_table[0]);
	CHECK(wg_sine_table[64] == 255, "%d", wg_sine_table[64]);
	CHECK(wg_sine_table[192] == 1, "%d", wg_sine_table[192]);
}

static void test_init_validation(void)
{
	struct wg_osc o;

	CHECK(wg_init(&o, WG_SINE, 440, 48000, 128) == 0, "default");
	CHECK(wg_init(&o, WG_SINE, 24000, 48000, 128) == 0, "nyquist ok");
	CHECK(wg_init(&o, WG_SINE, 24001, 48000, 128) != 0, "above nyquist");
	CHECK(wg_init(&o, WG_SINE, 0, 48000, 128) != 0, "zero freq");
	CHECK(wg_init(&o, WG_SINE, 1, 0, 128) != 0, "zero rate");
	CHECK(wg_init(&o, WG_SINE, 1, WG_RATE_MAX + 1, 128) != 0, "rate max");
	CHECK(wg_init(&o, WG_SINE, 1, WG_RATE_MAX, 256) == 0, "rate max ok");
	CHECK(wg_init(&o, WG_SINE, 440, 48000, 257) != 0, "amp > 256");
	CHECK(wg_init(&o, WG_NSHAPES, 440, 48000, 128) != 0, "bad shape");
	CHECK(wg_init(&o, (unsigned int)-1, 440, 48000, 128) != 0, "bad shape");

	/* 5 * 2^24 / 48000 = 1747.63: rounds up, truncation would not. */
	CHECK(wg_init(&o, WG_SINE, 5, 48000, 128) == 0, "5 Hz");
	CHECK(o.step == 1748, "step %u", o.step);
}

static int osc_eq(const struct wg_osc *a, const struct wg_osc *b)
{
	return a->phase == b->phase && a->step == b->step &&
	       a->amp == b->amp && a->shape == b->shape;
}

static void test_retune(void)
{
	struct wg_osc o, before;
	u8 buf[1000];

	wg_init(&o, WG_SAWTOOTH, 441, 44100, 256);
	wg_fill(&o, buf, sizeof(buf));
	before = o;
	CHECK(wg_set(&o, 30000, 48000, 128) != 0, "above nyquist accepted");
	CHECK(wg_set(&o, 440, 48000, 300) != 0, "amp 300 accepted");
	CHECK(osc_eq(&o, &before), "failed set changed osc");
	CHECK(wg_set(&o, 880, 48000, 64) == 0, "valid set refused");
	CHECK(o.phase == before.phase, "retune moved phase");
	CHECK(o.shape == WG_SAWTOOTH, "retune changed shape");
	CHECK(o.amp == 64, "amp %u", o.amp);
	wg_init(&before, WG_SINE, 880, 48000, 64);
	CHECK(o.step == before.step, "step %u vs %u", o.step, before.step);

	before = o;
	CHECK(wg_init(&o, WG_NSHAPES, 440, 48000, 128) != 0, "bad shape");
	CHECK(osc_eq(&o, &before), "failed init changed osc");
}

static void test_scale(void)
{
	CHECK(wg_scale(0, 255) == 1, "%d", wg_scale(0, 255));
	CHECK(wg_scale(255, 256) == 255, "%d", wg_scale(255, 256));
	CHECK(wg_scale(0, 256) == 0, "%d", wg_scale(0, 256));
	CHECK(wg_scale(200, 0) == 128, "%d", wg_scale(200, 0));
}

/* 375 Hz at 48 kHz is step 2^17: exactly 128 samples per period. */
static void test_exact_period(void)
{
	enum { N = 4096, P = 128 };
	static u8 buf[N];
	struct wg_osc o;

	for (unsigned int s = 0; s < WG_NSHAPES; s++) {
		long sum = 0;

		CHECK(wg_init(&o, s, 375, 48000, 256) == 0, "%s", names[s]);
		CHECK(o.step == 1u << 17, "%s step %u", names[s], o.step);
		wg_fill(&o, buf, N);
		for (int i = 0; i + P < N; i++)
			if (buf[i] != buf[i + P]) {
				CHECK(0, "%s not periodic at %d", names[s], i);
				break;
			}
		for (int i = 0; i < N; i++)
			sum += buf[i];
		CHECK(sum / N >= 127 && sum / N <= 128,
		      "%s mean %ld", names[s], sum / N);
	}
}

/* Any read split must produce the same bytes as one big read. */
static void test_split_continuity(void)
{
	enum { N = 100003 };
	static u8 whole[N], parts[N];
	struct wg_osc a, b;
	unsigned int lcg = 12345;

	for (unsigned int s = 0; s < WG_NSHAPES; s++) {
		size_t off = 0;

		wg_init(&a, s, 441, 44100, 200);
		wg_init(&b, s, 441, 44100, 200);
		wg_fill(&a, whole, N);
		while (off < N) {
			size_t n;

			lcg = lcg * 1103515245u + 12345u;
			n = (lcg >> 16) % 5000;
			if (n > N - off)
				n = N - off;
			wg_fill(&b, parts + off, n);
			off += n;
		}
		CHECK(memcmp(whole, parts, N) == 0, "%s split differs", names[s]);
		CHECK(a.phase == b.phase, "%s phase %u vs %u",
		      names[s], a.phase, b.phase);
	}
}

static void test_frequency(void)
{
	enum { N = 48000 };
	static u8 buf[N];
	struct wg_osc o;
	int rising = 0;

	wg_init(&o, WG_SINE, 440, 48000, 256);
	wg_fill(&o, buf, N);
	for (int i = 1; i < N; i++)
		if (buf[i - 1] < 128 && buf[i] >= 128)
			rising++;
	CHECK(rising >= 439 && rising <= 441, "440 Hz gave %d/s", rising);
}

static void test_amplitude(void)
{
	enum { N = 4096 };
	static u8 buf[N];
	struct wg_osc o;

	for (unsigned int s = 0; s < WG_NSHAPES; s++) {
		int lo = 255, hi = 0;

		wg_init(&o, s, 375, 48000, 0);
		wg_fill(&o, buf, N);
		for (int i = 0; i < N; i++)
			if (buf[i] != 128) {
				CHECK(0, "%s amp 0 gave %d", names[s], buf[i]);
				break;
			}

		wg_init(&o, s, 375, 48000, 128);
		wg_fill(&o, buf, N);
		for (int i = 0; i < N; i++) {
			lo = buf[i] < lo ? buf[i] : lo;
			hi = buf[i] > hi ? buf[i] : hi;
		}
		CHECK(lo >= 64 && hi <= 192, "%s amp 128 range %d..%d",
		      names[s], lo, hi);

		wg_init(&o, s, 375, 48000, 256);
		wg_fill(&o, buf, N);
		lo = 255;
		hi = 0;
		for (int i = 0; i < N; i++) {
			lo = buf[i] < lo ? buf[i] : lo;
			hi = buf[i] > hi ? buf[i] : hi;
		}
		CHECK(lo <= 2 && hi >= 253, "%s amp 256 range %d..%d",
		      names[s], lo, hi);
	}
}

static void test_phase_stays_24bit(void)
{
	enum { N = 1 << 16 };
	static u8 buf[N];
	struct wg_osc o;

	/* Not a power of two, so wraps don't land phase on 0 by luck. */
	wg_init(&o, WG_SAWTOOTH, 23999, 48000, 256);
	for (int i = 0; i < 300; i++) {
		wg_fill(&o, buf, N);
		CHECK(o.phase < 1u << 24, "phase %#x", o.phase);
	}
	/* At Nyquist the square alternates every sample. */
	wg_init(&o, WG_SQUARE, 24000, 48000, 256);
	wg_fill(&o, buf, 8);
	for (int i = 1; i < 8; i++)
		CHECK(buf[i] != buf[i - 1], "nyquist square stuck at %d", i);
}

int main(void)
{
	test_table();
	test_init_validation();
	test_scale();
	test_retune();
	test_exact_period();
	test_split_continuity();
	test_frequency();
	test_amplitude();
	test_phase_stays_24bit();
	if (failures) {
		fprintf(stderr, "test_wavegen: %d failure(s)\n", failures);
		return 1;
	}
	puts("test_wavegen: ok");
	return 0;
}
