// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * Reference stream from the userspace build of wavegen.h, for comparing
 * against the device byte for byte.
 *   wgref SHAPE FREQ RATE AMP NBYTES [FREQ RATE AMP NBYTES]... > out
 * Each extra group retunes with wg_set (phase kept), like the ioctl.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../wavegen.h"

static unsigned long arg(char *s)
{
	return strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
	static u8 buf[65536];
	struct wg_osc o;
	int g;

	if (argc < 6 || (argc - 2) % 4) {
		fputs("usage: wgref SHAPE FREQ RATE AMP NBYTES [FREQ RATE AMP NBYTES]...\n",
		      stderr);
		return 2;
	}
	for (g = 2; g < argc; g += 4) {
		unsigned long n = arg(argv[g + 3]);
		int bad = g == 2 ?
			wg_init(&o, arg(argv[1]), arg(argv[g]), arg(argv[g + 1]),
				arg(argv[g + 2])) :
			wg_set(&o, arg(argv[g]), arg(argv[g + 1]), arg(argv[g + 2]));

		if (bad) {
			fputs("wgref: invalid parameters\n", stderr);
			return 2;
		}
		while (n) {
			size_t k = n < sizeof(buf) ? n : sizeof(buf);

			wg_fill(&o, buf, k);
			if (fwrite(buf, 1, k, stdout) != k)
				return 1;
			n -= k;
		}
	}
	return 0;
}
