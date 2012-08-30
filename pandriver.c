/*
 * panalyzer.c A Logic Analyzer for the RaspberryPi
 * Copyright (c) 2012 Richard Hirst <richardghirst@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * If you want the device node created automatically:
 *
 * ============= /etc/udev/rules.d/20-panalyzer.rules =============
 * SUBSYSTEM=="module", DEVPATH=="/module/pandriver", RUN+="/lib/udev/panalyzer"
 * ================================================================
 *
 * ===================== /lib/udev/panalyzer ======================
 * #!/bin/bash
 *
 * if [ "$ACTION" = "remove" ]; then
 *         rm -f /dev/panalyzer
 * elif [ "$ACTION" = "add" ]; then
 *          major=$( sed -n 's/ panalyzer//p' /proc/devices )
 *        [ "$major" ] && mknod -m 0666 /dev/panalyzer c $major 0
 * fi
 *
 * exit 0
 * ================================================================
 */

/* TODO:
 *  - Ensure relevant GPIO pins are inputs
 */

//#define RUN_FLATOUT

#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/cdev.h>
#include <mach/platform.h>
#include <asm/uaccess.h>
#include "panalyzer.h"

static int dev_open(struct inode *, struct file *);
static int dev_close(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static loff_t dev_llseek(struct file *flip, loff_t off, int whence);

static panctl_t def_panctl = DEF_PANCTL;

static panctl_t panctl;

static struct file_operations fops = 
{
	.open = dev_open,
	.read = dev_read,
	.write = dev_write,
	.llseek = dev_llseek,
	.release = dev_close,
};

static uint32_t *buffer;
static uint32_t first_data_index;
static volatile uint32_t *data;
static volatile uint32_t *ticker;
static volatile uint32_t *armtick;
static dev_t devno;
static struct cdev my_cdev;
static int my_major;
//static uint32_t foo, bar;

static int capture(void)
{
	uint32_t start_time, end_time, abort_time, t = 0, t1;
	uint32_t start_tick, end_tick;
	uint32_t *buf_start = buffer;
	uint32_t *buf_end   = buffer + panctl.num_samples;
	uint32_t *buf_ptr = buf_start;
	uint32_t sample;
	int post_trigger_samples;
	int pre_trigger_samples;
	int state = 0, last_state, state_samples = panctl.trigger[0].min_samples;
	int overruns = 0;

	for (last_state = 0; last_state < MAX_TRIGGERS && panctl.trigger[last_state].enabled; last_state++)
		;
	if (--last_state < 0)
		state = -1;

	if (panctl.trigger_point == 0)
		post_trigger_samples = panctl.num_samples * 19 / 20;
	else if (panctl.trigger_point == 1)
		post_trigger_samples = panctl.num_samples / 2;
	else
		post_trigger_samples = panctl.num_samples / 20;
	pre_trigger_samples = panctl.num_samples - post_trigger_samples;

	local_irq_disable();
	local_fiq_disable();
	start_time = *ticker;
	start_tick = armtick[8];
	t = start_time;
	abort_time = start_time + (panctl.num_samples > 500000 ? panctl.num_samples * 2 : 1000000);
#ifdef RUN_FLATOUT
	armtick[2] = 1<<9;
	while (buf_ptr != buf_end) {
		volatile uint32_t x = *ticker;
		*buf_ptr++ = armtick[8];
	}
#else
	for (;;) {
		do { t1 = *ticker; } while (t1 == t);
		sample = *data;
		overruns += t1 - t - 1;
		t = t1;
		*buf_ptr++ = sample;
		if (buf_ptr == buf_end)
			buf_ptr = buf_start;
		if (pre_trigger_samples > 0) {
			pre_trigger_samples--;
		} else if (state < 0) {
			if (--post_trigger_samples <= 0)
				break;
		} else if (t1 == abort_time) {
			local_irq_enable();
			printk(KERN_INFO "Aborted, state %d, mask %08x, value %08x, sample %08x, state_samples %d\n",
					state, panctl.trigger[state].mask, panctl.trigger[state].value, sample, state_samples);
			return -EINTR;
		} else {
recheck:
			// TODO: Not convinced this logic is correct..
			if (state_samples == 0) {
				if (state == last_state) {
					state = -1;
					continue;
				} else if ((panctl.trigger[state+1].mask & sample) == panctl.trigger[state+1].value) {
					state++;
					state_samples = panctl.trigger[state].min_samples - 1;
					goto recheck;
				}
			} else {
				state_samples--;
			}
			if ((panctl.trigger[state].mask & sample) != panctl.trigger[state].value) {
				state = 0;
				state_samples = panctl.trigger[state].min_samples;
			}
		}
	}
#endif
	end_time = *ticker;
	end_tick = armtick[8];
	local_fiq_enable();
	local_irq_enable();
	first_data_index = buf_ptr - buf_start;

//	printk(KERN_INFO "%d samples in %dus, %d overruns\n", panctl.num_samples, end_time - start_time, overruns);
	printk(KERN_INFO "%d samples in %dus\n", end_tick - start_tick, end_time - start_time);

	return 0;
}

int init_module(void)
{
	int res;
	
	res = alloc_chrdev_region(&devno, 0, 1, "panalyzer");
	if (res < 0) {
		printk(KERN_WARNING "Panalyzer: Can't allocated device number\n");
		return res;
	}
	my_major = MAJOR(devno);
	cdev_init(&my_cdev, &fops);
	my_cdev.owner = THIS_MODULE;
	my_cdev.ops = &fops;
	res = cdev_add(&my_cdev, MKDEV(my_major, 0), 1);
	if (res) {
		printk(KERN_WARNING "Panalyzer: Error %d adding device\n", res);
		unregister_chrdev_region(devno, 1);
		return res;
	}

	data = (uint32_t *)ioremap(0x20200034, 4);
	ticker = (uint32_t *)ioremap(0x20003004, 4);
	armtick = (uint32_t *)ioremap(0x2000b400, 0x24);

	return 0;
}


void cleanup_module(void)
{
	iounmap(data);
	iounmap(ticker);
	iounmap(armtick);
	cdev_del(&my_cdev);
	unregister_chrdev_region(devno, 1);
}


static int dev_open(struct inode *inod,struct file *fil)
{
	memcpy(&panctl, &def_panctl, sizeof(panctl));

	return 0;
}

static ssize_t dev_read(struct file *filp,char *buf,size_t count,loff_t *f_pos)
{
	int res;

	if (panctl.magic != PAN_MAGIC)
		return -EINVAL;

	if (buffer == NULL) {
		buffer = (uint32_t *)vmalloc(panctl.num_samples * sizeof(uint32_t *));
		if (buffer == NULL) {
			printk(KERN_ALERT "vmalloc failed\n");
			return -EFAULT;
		}
		memset(buffer, 0, panctl.num_samples * sizeof(uint32_t *));

		res = capture();
		if (res)
			return res;
	}

	if (*f_pos >= sizeof(panctl_t) + panctl.num_samples * sizeof(uint32_t))
		return 0;
	if (*f_pos < sizeof(panctl_t)) {
		count = sizeof(panctl_t) - *f_pos;
//		printk(KERN_INFO "READ: returning %x bytes from %p\n", count, (char *)&panctl + *f_pos);
		if (copy_to_user(buf, (char *)&panctl + *f_pos, count))
			return -EFAULT;
	}
	else {
		char *start = (char *)buffer;
		char *data_start = start + first_data_index * sizeof(uint32_t);
		char *end = start + panctl.num_samples * sizeof(uint32_t);
		int index = *f_pos - sizeof(panctl);
		char *p = data_start + index;

		if (p >= end) {
			p -= end - start;
			if (count > data_start - p)
				count = data_start - p;
		} else if (count > end - p) {
			count = end - p;
		}

//		printk(KERN_INFO "READ: returning %x bytes from %p\n", count, (char *)buffer + *f_pos);
		if(copy_to_user(buf, p, count))
			return -EFAULT;
	}
	*f_pos += count;

	return count;
}

static ssize_t dev_write(struct file *filp,const char *buf,size_t count,loff_t *f_pos)
{
	if (*f_pos >= sizeof(panctl))
		return 0;
	count = sizeof(panctl) - *f_pos;
	if (copy_from_user((char *)&panctl + *f_pos, buf, count))
		return -EFAULT;
	if (panctl.magic != PAN_MAGIC)
		return -EINVAL;
	*f_pos += count;

	return count;
}

static loff_t dev_llseek(struct file *filp, loff_t off, int whence)
{
	if (whence == SEEK_SET && off >= 0 && off < sizeof(panctl) + panctl.num_samples * sizeof(uint32_t)) {
		filp->f_pos = off;
		return off;
	} else {
		return -EINVAL;
	}
}

static int dev_close(struct inode *inod,struct file *fil)
{
	vfree(buffer);
	buffer = NULL;

	return 0;
}

MODULE_DESCRIPTION("Panalyzer, a RaspberryPi based Logic Analyzer");
MODULE_AUTHOR("Richard Hirst <richardghirst@gmail.com>");
MODULE_LICENSE("GPL v2");

