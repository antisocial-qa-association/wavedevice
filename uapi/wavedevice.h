/* SPDX-License-Identifier: LicenseRef-PublicDomain */
/*
 * wavedevice userspace API: retune one open /dev/{sine,sawtooth,...}
 * without restarting its wave. Copy this header into your program.
 *
 * Changes affect only the fd you call them on (and anything sharing
 * it). Shape is fixed per device. Retuning keeps phase, so there's no
 * click. Out-of-range values get EINVAL and change nothing.
 */
#ifndef _UAPI_WAVEDEVICE_H
#define _UAPI_WAVEDEVICE_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct wavedevice_params {
	__u32 freq;	/* Hz, 1..rate/2 */
	__u32 rate;	/* samples/s you play at, 1..768000 */
	__u32 amp;	/* 0..256, 256 is full scale */
	__u32 flags;	/* must be 0 */
};

/* '~' is unclaimed in ioctl-number.rst, and it looks like a wave. */
#define WAVEDEVICE_IOC_TYPE	'~'
#define WAVEDEVICE_GET_PARAMS	_IOR(WAVEDEVICE_IOC_TYPE, 1, struct wavedevice_params)
#define WAVEDEVICE_SET_PARAMS	_IOW(WAVEDEVICE_IOC_TYPE, 2, struct wavedevice_params)

#endif /* _UAPI_WAVEDEVICE_H */
