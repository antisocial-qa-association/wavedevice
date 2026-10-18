// SPDX-License-Identifier: LicenseRef-PublicDomain
/*
 * wavedevice - /dev/sine and friends, for people who wanted /dev/urandom
 * but in tune.
 *
 * /dev/sine, /dev/sawtooth, /dev/triangle, /dev/square. Each open() gets
 * its own oscillator starting at phase 0, so every reader hears a whole
 * wave from the top. Reads never end and never come up short except on
 * a signal. Output is unsigned 8-bit mono.
 *
 * Module params freq, rate and amp set what each open() starts with;
 * changing them never disturbs streams already open. A program can
 * retune its own fd with the ioctls in uapi/wavedevice.h.
 *
 * Public domain. See LICENSE.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "uapi/wavedevice.h"
#include "wavegen.h"

/* Generate this much at a time, then copy it out to userspace. */
#define WD_CHUNK	PAGE_SIZE

/*
 * Params are range-checked on write. Whether freq fits under rate / 2 is
 * only checked at open(), so they can be changed in either order.
 */
struct wd_param {
	unsigned int val;
	unsigned int min, max;
};

static int wd_param_set(const char *buf, const struct kernel_param *kp)
{
	struct wd_param *p = kp->arg;
	unsigned int v;
	int ret;

	ret = kstrtouint(buf, 0, &v);
	if (ret)
		return ret;
	if (v < p->min || v > p->max)
		return -EINVAL;
	WRITE_ONCE(p->val, v);
	return 0;
}

static int wd_param_get(char *buf, const struct kernel_param *kp)
{
	const struct wd_param *p = kp->arg;

	return scnprintf(buf, PAGE_SIZE, "%u\n", p->val);
}

static const struct kernel_param_ops wd_param_ops = {
	.set = wd_param_set,
	.get = wd_param_get,
};

static struct wd_param wd_freq = { 440, 1, WG_RATE_MAX / 2 };
static struct wd_param wd_rate = { 48000, 1, WG_RATE_MAX };
static struct wd_param wd_amp = { 128, 0, WG_AMP_MAX };

module_param_cb(freq, &wd_param_ops, &wd_freq, 0644);
MODULE_PARM_DESC(freq, "Hz for new opens, 1..rate/2 (default 440)");
module_param_cb(rate, &wd_param_ops, &wd_rate, 0644);
MODULE_PARM_DESC(rate, "Samples/s the reader plays at (default 48000)");
module_param_cb(amp, &wd_param_ops, &wd_amp, 0644);
MODULE_PARM_DESC(amp, "Amplitude 0..256, 256 = full scale (default 128)");

struct wd_dev {
	struct miscdevice misc;
	unsigned int shape;
};

/* Per open file. Threads or forks sharing the fd share one stream. */
struct wd_file {
	struct mutex lock;	/* serializes readers and ioctls */
	struct wg_osc osc;
	struct wavedevice_params params;	/* what osc was set from */
	u8 *buf;		/* WD_CHUNK bytes of scratch */
};

static int wd_open(struct inode *inode, struct file *file)
{
	/* misc_open() left our miscdevice in private_data. */
	struct wd_dev *dev = container_of(file->private_data,
					  struct wd_dev, misc);
	struct wd_file *wf;
	unsigned int freq, rate, amp;
	int ret;

	ret = nonseekable_open(inode, file);	/* lseek() gets -ESPIPE */
	if (ret)
		return ret;

	/* _ACCOUNT: charge the reader's memcg; any user can open lots. */
	wf = kzalloc(sizeof(*wf), GFP_KERNEL_ACCOUNT);
	if (!wf)
		return -ENOMEM;
	wf->buf = kmalloc(WD_CHUNK, GFP_KERNEL_ACCOUNT);
	if (!wf->buf) {
		kfree(wf);
		return -ENOMEM;
	}
	/*
	 * No lock: each param is its own sysfs write, so a set of them is
	 * never changed atomically anyway, and wg_init rejects a bad mix.
	 */
	freq = READ_ONCE(wd_freq.val);
	rate = READ_ONCE(wd_rate.val);
	amp = READ_ONCE(wd_amp.val);
	if (wg_init(&wf->osc, dev->shape, freq, rate, amp)) {
		pr_warn_ratelimited("freq=%u is above rate/2 (rate=%u)\n",
				    freq, rate);
		kfree(wf->buf);
		kfree(wf);
		return -EINVAL;
	}
	wf->params.freq = freq;
	wf->params.rate = rate;
	wf->params.amp = amp;
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

static long wd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct wd_file *wf = file->private_data;
	void __user *uarg = (void __user *)arg;
	struct wavedevice_params p;
	int ret = 0;

	switch (cmd) {
	case WAVEDEVICE_GET_PARAMS:
		if (mutex_lock_interruptible(&wf->lock))
			return -ERESTARTSYS;
		p = wf->params;		/* kzalloc'd, so flags and padding are 0 */
		mutex_unlock(&wf->lock);
		if (copy_to_user(uarg, &p, sizeof(p)))
			return -EFAULT;
		return 0;

	case WAVEDEVICE_SET_PARAMS:
		if (copy_from_user(&p, uarg, sizeof(p)))
			return -EFAULT;
		if (p.flags)
			return -EINVAL;
		if (mutex_lock_interruptible(&wf->lock))
			return -ERESTARTSYS;
		if (wg_set(&wf->osc, p.freq, p.rate, p.amp))
			ret = -EINVAL;
		else
			wf->params = p;
		mutex_unlock(&wf->lock);
		return ret;
	}
	return -ENOTTY;
}

static const struct file_operations wd_fops = {
	.owner		= THIS_MODULE,	/* open fds pin the module */
	.open		= wd_open,
	.release	= wd_release,
	.read		= wd_read,
	.unlocked_ioctl	= wd_ioctl,
	/* All-__u32 struct: same layout for 32-bit callers. */
	.compat_ioctl	= compat_ptr_ioctl,
	/* No .write: write() gets -EINVAL, even for root. */
};

/*
 * A misc device is a char device with major 10 and a minor picked at
 * register time. udev/devtmpfs creates /dev/<name> with .mode for us.
 */
#define WD_DEV(_name, _shape) {			\
	.misc = {					\
		.minor	= MISC_DYNAMIC_MINOR,		\
		.name	= _name,			\
		.fops	= &wd_fops,			\
		.mode	= 0444,				\
	},						\
	.shape = _shape,				\
}

static struct wd_dev wd_devs[] = {
	WD_DEV("sine", WG_SINE),
	WD_DEV("sawtooth", WG_SAWTOOTH),
	WD_DEV("triangle", WG_TRIANGLE),
	WD_DEV("square", WG_SQUARE),
};

static int __init wd_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(wd_devs); i++) {
		ret = misc_register(&wd_devs[i].misc);
		if (ret) {
			pr_err("can't register /dev/%s: %d\n",
			       wd_devs[i].misc.name, ret);
			while (i--)
				misc_deregister(&wd_devs[i].misc);
			return ret;
		}
	}
	return 0;
}

static void __exit wd_exit(void)
{
	int i = ARRAY_SIZE(wd_devs);

	while (i--)
		misc_deregister(&wd_devs[i].misc);
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
