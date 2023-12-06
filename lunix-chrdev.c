/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * Spyridon Galanopoulos (03120093)
 * Efthymios Ntokas (03120631)
 *
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */
static int __attribute__((unused)) lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *);
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	
	WARN_ON ( !(sensor = state->sensor));

	// If sensor last update is not the same as the timestamp on the chrdev struct, then we need to update
	return (sensor->msr_data[state->type]->last_update != state->buf_timestamp);
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct __attribute__((unused)) *sensor;
	struct lunix_msr_data_struct *new_data_raw;
	long new_data;
	int i = 0;

	sensor = state->sensor;

	/*
	 * Grab the raw data quickly, hold the
	 * spinlock for as little as possible.
	 */
	spin_lock(&sensor->lock);

	new_data_raw = sensor->msr_data[state->type];

	spin_unlock(&sensor->lock);

	// If no new data just return with error
	if(state->buf_timestamp == new_data_raw->last_update) return -EAGAIN;

	/*
	 * Now we can take our time to format them,
	 * holding only the private state semaphore
	 */

	if(state->mode == CHRDEV_MODE_COOKED) {
		// Return the formatted decimal numbers

		// First, get measurement
		if(state->type == BATT) new_data = lookup_voltage[new_data_raw->values[0]];
		else if(state->type == TEMP) new_data = lookup_temperature[new_data_raw->values[0]];
		else new_data = lookup_light[new_data_raw->values[0]];

		debug("Received number %ld", new_data);

		// Update the timestamp in state
		state->buf_timestamp = new_data_raw->last_update;

		if(new_data < 0) {
			state->buf_data[(state->buf_lim)++] = '-';
			new_data = -1 * new_data;
		}

		for(i = 0; i < 3; i++) {
			state->buf_data[state->buf_lim + 5 - i] = '0' + new_data%10;
			new_data /= 10;
		}

		state->buf_data[state->buf_lim + 2] = '.';
		for(i = 0; i < 2; i++) {
			state->buf_data[state->buf_lim + 1 - i] = '0' + new_data%10;
			new_data /= 10;
		}

		state->buf_lim += 6;

		while(state->buf_lim % 10) {
			state->buf_data[state->buf_lim++] = ' ';
		}
	} else{
		// Just return the raw 16-bit values

		// First, get measurement
		if(state->type == BATT) new_data = new_data_raw->values[0];
		else if(state->type == TEMP) new_data = new_data_raw->values[0];
		else new_data = new_data_raw->values[0];

		// Update the timestamp in state
		state->buf_timestamp = new_data_raw->last_update;

		state->buf_data[(state->buf_lim)++] = new_data >> 8;
		state->buf_data[(state->buf_lim)++] = new_data & 0xFF;
	}

	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
	/* Declarations */
	/* ? */
	int ret;
	unsigned int imnr;
	struct lunix_chrdev_state_struct *chrdv;

	debug("entering\n");
	ret = -ENODEV;
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto out;

	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */
	imnr = iminor(inode);
	
	// Allocate a new Lunix character device private state structure
	chrdv = kmalloc(sizeof(struct lunix_chrdev_state_struct), GFP_KERNEL);
	if(!chrdv) {
		debug("Failed to allocate character device private structure");
		return -ENOMEM;
	}
	chrdv->type = imnr%8; // Last 3 bits of minor number indicate measurement type
	chrdv->sensor = &lunix_sensors[imnr>>3];
	chrdv->buf_lim = 0;
	chrdv->buf_timestamp = 0;
	sema_init(&chrdv->lock, 1);
	chrdv->mode = CHRDEV_MODE_COOKED;
	filp->private_data = chrdv;
	
	ret = 0;
out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct lunix_chrdev_state_struct *state;
	int ret = 0;

	state = filp->private_data;
	WARN_ON(!state);
	
	switch(cmd) {
		case LUNIX_IOC_MODE:
			if(arg == CHRDEV_MODE_RAW || arg == CHRDEV_MODE_COOKED) {
				state->mode = arg;
				ret = 0;
			}else ret = -ENOTTY;
			break;
		default:
			ret = -ENOTTY;
	}
	return ret;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	// Only one device should be reading at a given moment, so we have to get the lock first
	if(down_interruptible(&state->lock)) return -ERESTARTSYS;

	/*
	 * If the cached character device state needs to be
	 * updated by actual sensor data (i.e. we need to report
	 * on a "fresh" measurement, do so
	 */
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			// There was no new data. We should either leave or go to sleep until there is and retry later.
			// Either way we have to unlock.
			up(&state->lock);

			if(filp->f_flags & O_NONBLOCK) return 0; // If O_NONBLOCK is chosen, we should just leave

			debug("Going to sleep");
			if(wait_event_interruptible(sensor->wq, (lunix_chrdev_state_needs_refresh(state)))) return -ERESTARTSYS;

			debug("Waking up");
			if(down_interruptible(&state->lock)) return -ERESTARTSYS;
		}
	}

	if(*f_pos + cnt < state->buf_lim) {
		// If we do not consume entire measurement, just move pointer to start of unread and get out
		if(copy_to_user(usrbuf, state->buf_data + *f_pos, cnt)) {
			debug("Error copying to userspace, <");
			ret = -EFAULT;
			goto out;
		}

		ret = cnt;

		*f_pos += cnt;
	}else {
		// If we want more than the rest of the measurement, return all the bytes left and reset f_pos
		if(copy_to_user(usrbuf, state->buf_data + *f_pos, state->buf_lim - *f_pos)) {
			debug("Error copying to userspace, >=");
			ret = -EFAULT;
			goto out;
		}

		ret = state->buf_lim - *f_pos;

		*f_pos = 0;
		state->buf_lim = 0;
	}

out:
	/* Unlock? */
	up(&state->lock);
	return ret;
}

void lunix_chrdev_vma_open(struct vm_area_struct *vma)
{
	debug("Calling open\n");
}

void lunix_chrdev_vma_close(struct vm_area_struct *vma)
{
	debug("Calling close\n");
}

static struct vm_operations_struct lunix_chrdev_vm_ops = {
.open = lunix_chrdev_vma_open,
.close = lunix_chrdev_vma_close,
};

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct lunix_chrdev_state_struct *state;
	struct lunix_sensor_struct *sensor;
	struct lunix_msr_data_struct *msr_data;
	unsigned long pfn;

	state = filp->private_data;
	sensor = state->sensor;
	msr_data = sensor->msr_data[state->type];

	pfn = page_to_pfn(virt_to_page(msr_data));
	
	if (remap_pfn_range(vma, vma->vm_start, pfn, vma->vm_end - vma->vm_start, vma->vm_page_prot)) return -EAGAIN;
	
	vma->vm_ops = &lunix_chrdev_vm_ops;
	lunix_chrdev_vma_open(vma);
	return 0;
}

static struct file_operations lunix_chrdev_fops = 
{
        .owner          = THIS_MODULE,
	.open           = lunix_chrdev_open,
	.release        = lunix_chrdev_release,
	.read           = lunix_chrdev_read,
	.unlocked_ioctl = lunix_chrdev_ioctl,
	.mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
	/*
	 * Register the character device with the kernel, asking for
	 * a range of minor numbers (number of sensors * 8 measurements / sensor)
	 * beginning with LINUX_CHRDEV_MAJOR:0
	 */
	int ret;
	dev_t dev_no;
	unsigned int lunix_minor_cnt;
	char name[] = "LUNIX:TNG";

	lunix_minor_cnt = lunix_sensor_cnt << 3;
	
	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;
	// lunix_chrdev_cdev.fops = &lunix_chrdev_fops // Probably unnecessary, but leaving it here just in case
	
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);

	ret = register_chrdev_region(dev_no, lunix_minor_cnt, name);
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}	
	
	ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);
	if (ret < 0) {
		debug("failed to add character device\n");
		goto out_with_chrdev_region;
	}

	debug("completed successfully\n");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
	return ret;
}

void lunix_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;
		
	debug("entering\n");
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	cdev_del(&lunix_chrdev_cdev);
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
	debug("leaving\n");
}