// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * wavedevice - /dev/sine and friends, for people who wanted /dev/urandom
 * but in tune.
 *
 * Public domain. See LICENSE.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

static const struct file_operations wd_fops = {
	.owner	= THIS_MODULE,
	/* Marks the fd unseekable; lseek() gets -ESPIPE, like a pipe. */
	.open	= nonseekable_open,
};

/*
 * A misc device is a char device with major 10 and a minor picked at
 * register time. udev/devtmpfs creates /dev/<name> with .mode for us.
 */
static struct miscdevice wd_sine = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "sine",
	.fops	= &wd_fops,
	.mode	= 0444,
};

static int __init wd_init(void)
{
	return misc_register(&wd_sine);
}

static void __exit wd_exit(void)
{
	misc_deregister(&wd_sine);
}

module_init(wd_init);
module_exit(wd_exit);

MODULE_DESCRIPTION("Endless periodic waveforms as character devices");
/*
 * The source is public domain (see LICENSE). The kernel has no tag for
 * that; this string is the closest true statement it accepts without
 * tainting: anything you can do under the GPL, plus everything else.
 */
MODULE_LICENSE("GPL and additional rights");
