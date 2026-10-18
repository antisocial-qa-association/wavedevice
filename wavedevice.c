// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * wavedevice - /dev/sine and friends, for people who wanted /dev/urandom
 * but in tune.
 *
 * Each open() gets its own oscillator starting at phase 0, so every
 * reader hears a whole wave from the top. Reads never end and never come
 * up short except on a signal. Output is unsigned 8-bit mono.
 *
 * Public domain. See LICENSE.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "wavegen.h"

/* Generate this much at a time, then copy it out to userspace. */
#define WD_CHUNK	PAGE_SIZE

struct wd_dev {
	struct miscdevice misc;
	unsigned int shape;
};

/* Per open file. Threads or forks sharing the fd share one stream. */
struct wd_file {
	struct mutex lock;	/* serializes readers: osc and buf */
	struct wg_osc osc;
	u8 *buf;		/* WD_CHUNK bytes of scratch */
};

static int wd_open(struct inode *inode, struct file *file)
{
	/* misc_open() left our miscdevice in private_data. */
	struct wd_dev *dev = container_of(file->private_data,
					  struct wd_dev, misc);
	struct wd_file *wf;
	int ret;

	ret = nonseekable_open(inode, file);	/* lseek() gets -ESPIPE */
	if (ret)
		return ret;

	wf = kzalloc(sizeof(*wf), GFP_KERNEL);
	if (!wf)
		return -ENOMEM;
	wf->buf = kmalloc(WD_CHUNK, GFP_KERNEL);
	if (!wf->buf) {
		kfree(wf);
		return -ENOMEM;
	}
	if (wg_init(&wf->osc, dev->shape, 440, 48000, 128)) {
		kfree(wf->buf);
		kfree(wf);
		return -EINVAL;
	}
	mutex_init(&wf->lock);
	file->private_data = wf;
	return 0;
}

/* Called once, when the last reference to the open file goes away. */
static int wd_release(struct inode *inode, struct file *file)
{
	struct wd_file *wf = file->private_data;

	mutex_destroy(&wf->lock);
	kfree(wf->buf);
	kfree(wf);
	return 0;
}

static ssize_t wd_read(struct file *file, char __user *ubuf, size_t count,
		       loff_t *ppos)
{
	struct wd_file *wf = file->private_data;
	size_t done = 0;
	ssize_t err = 0;

	if (mutex_lock_interruptible(&wf->lock))
		return -ERESTARTSYS;

	while (done < count) {
		size_t n = min_t(size_t, count - done, WD_CHUNK);
		u32 phase = wf->osc.phase;
		size_t left;

		wg_fill(&wf->osc, wf->buf, n);
		/* Only bytes just written to buf ever reach userspace. */
		left = copy_to_user(ubuf + done, wf->buf, n);
		if (left) {
			/*
			 * Bad user pointer partway through. Rewind the
			 * oscillator to what was actually delivered, so the
			 * stream stays continuous for the next read.
			 */
			n -= left;
			wf->osc.phase = (phase + (u32)n * wf->osc.step) &
					WG_PHASE_MASK;
			done += n;
			err = -EFAULT;
			break;
		}
		done += n;

		/* Like /dev/zero: a signal ends the read with what we have. */
		if (signal_pending(current))
			break;
		cond_resched();
	}

	mutex_unlock(&wf->lock);
	return done ? done : err;
}

static const struct file_operations wd_fops = {
	.owner		= THIS_MODULE,	/* open fds pin the module */
	.open		= wd_open,
	.release	= wd_release,
	.read		= wd_read,
	/* No .write: write() gets -EINVAL, even for root. */
};

/*
 * A misc device is a char device with major 10 and a minor picked at
 * register time. udev/devtmpfs creates /dev/<name> with .mode for us.
 */
static struct wd_dev wd_sine = {
	.misc = {
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= "sine",
		.fops	= &wd_fops,
		.mode	= 0444,
	},
	.shape = WG_SINE,
};

static int __init wd_init(void)
{
	return misc_register(&wd_sine.misc);
}

static void __exit wd_exit(void)
{
	misc_deregister(&wd_sine.misc);
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
