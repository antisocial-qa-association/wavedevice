// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * Reference stream from the userspace build of wavegen.h, for comparing
 * against the device byte for byte.
 *   wgref SHAPE FREQ RATE AMP NBYTES > out
 */
#include <stdio.h>
#include <stdlib.h>

#include "../wavegen.h"

int main(int argc, char **argv)
{
	static u8 buf[65536];
	struct wg_osc o;
	unsigned long n;

	if (argc != 6) {
		fputs("usage: wgref SHAPE FREQ RATE AMP NBYTES\n", stderr);
		return 2;
	}
	if (wg_init(&o, strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0),
		    strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0))) {
		fputs("wgref: invalid parameters\n", stderr);
		return 2;
	}
	for (n = strtoul(argv[5], NULL, 0); n; ) {
		size_t k = n < sizeof(buf) ? n : sizeof(buf);

		wg_fill(&o, buf, k);
		if (fwrite(buf, 1, k, stdout) != k)
			return 1;
		n -= k;
	}
	return 0;
}
