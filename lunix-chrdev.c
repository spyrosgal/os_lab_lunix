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
/*
 * Declare a prototype so we can define the "unused" attribute and keep
 * the compiler happy. This function is not yet used, because this helpcode
 * is a stub.
 */
// Done for now (?)
static int __attribute__((unused)) lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *);
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	
	WARN_ON ( !(sensor = state->sensor));
	/* ? */
	// If sensor last update is not the same as the timestamp on the chrdev struct, then we need to update
	return (sensor->msr_data[state->type]->last_update != state->buf_timestamp);

	/* The following return is bogus, just for the stub to compile */
	// return 0; /* ? */
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

	// debug("leaving\n");

	sensor = state->sensor;

	/*
	 * Grab the raw data quickly, hold the
	 * spinlock for as little as possible.
	 */
	spin_lock(&sensor->lock);

	new_data_raw = sensor->msr_data[state->type];

	spin_unlock(&sensor->lock);
	/* ? */
	/* Why use spinlocks? See LDD3, p. 119 */

	/*
	 * Any new data available?
	 */
	/* ? */
	if(state->buf_timestamp == new_data_raw->last_update) return -EAGAIN;

	/*
	 * Now we can take our time to format them,
	 * holding only the private state semaphore
	 */
	if(state->type == BATT) new_data = lookup_voltage[new_data_raw->values[0]];
	else if(state->type == TEMP) new_data = lookup_temperature[new_data_raw->values[0]];
	else new_data = lookup_light[new_data_raw->values[0]];

	debug("%ld", new_data);

	state->buf_timestamp = new_data_raw->last_update;
	if(new_data < 0) {
		state->buf_data[(state->buf_lim)++] = '-';
		new_data = -1 * new_data;
	}

	new_data >>= 8;

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

	/* ? */

	debug("Received numbers %d %d %d %d %d %d %d", state->buf_data[0], state->buf_data[1], state->buf_data[2], state->buf_data[3], state->buf_data[4], state->buf_data[5], state->buf_data[6]);
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

// Done for now
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
	
	/* Allocate a new Lunix character device private state structure */
	/* ? */
	chrdv = kmalloc(sizeof(struct lunix_chrdev_state_struct), GFP_KERNEL);
	if(!chrdv) {
		debug("Failed to allocate character device private structure");
		return -ENOMEM;
	}
	chrdv->type = imnr%8; // Last 3 bits of minor number inticate measurement type
	chrdv->sensor = &lunix_sensors[imnr>>3];
	chrdv->buf_lim = 0;
	chrdv->buf_timestamp = 0;
	// TODO: See if I need to initialize semaphore
	sema_init(&chrdv->lock, 1);
	filp->private_data = chrdv;
	
	ret = 0;
out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

// Done for now
static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	/* ? */
	kfree(filp->private_data);
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Why? */
	return -EINVAL;
}

// Initially implement without synchronization
// TODO: Parallelize
static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	/* Lock? */
	if(down_interruptible(&state->lock)) return -ERESTARTSYS;

	/*
	 * If the cached character device state needs to be
	 * updated by actual sensor data (i.e. we need to report
	 * on a "fresh" measurement, do so
	 */
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			up(&state->lock);
			if(filp->f_flags & O_NONBLOCK) return -EAGAIN;

			debug("Going to sleep");
			if(wait_event_interruptible(sensor->wq, (lunix_chrdev_state_needs_refresh(state)))) return -ERESTARTSYS;

			debug("Waking up");
			if(down_interruptible(&state->lock)) return -ERESTARTSYS;
			/* ? */
			/* The process needs to sleep */
			/* See LDD3, page 153 for a hint */
		}
	}

	/* End of file */
	/* ? */
	if(*f_pos + cnt < state->buf_lim) {
		if(copy_to_user(usrbuf, state->buf_data + *f_pos, cnt)) {
			debug("Error copying to userspace, <");
			ret = -EFAULT;
			goto out;
		}

		ret = cnt;

		*f_pos += cnt;
	}else {
		if(copy_to_user(usrbuf, state->buf_data + *f_pos, state->buf_lim - *f_pos)) {
			debug("Error copying to userspace, >=");
			ret = -EFAULT;
			goto out;
		}

		ret = state->buf_lim - *f_pos;

		*f_pos = 0;
		state->buf_lim = 0;
	}
	
	/* Determine the number of cached bytes to copy to userspace */
	/* ? */

	/* Auto-rewind on EOF mode? */
	/* ? */

	/*
	 * The next two lines  are just meant to suppress a compiler warning
	 * for the "unused" out: label, and for the uninitialized "ret" value.
	 * It's true, this helpcode is a stub, and doesn't use them properly.
	 * Remove them when you've started working on this code.
	 */
	// ret = -ENODEV;
	// goto out;
out:
	/* Unlock? */
	up(&state->lock);
	return ret;
}

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return -EINVAL;
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

// Done for now
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

	lunix_minor_cnt = lunix_sensor_cnt >> 3;
	
	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;
	// lunix_chrdev_cdev.fops = &lunix_chrdev_fops // Probably unnecessary, but leaving it here just in case
	
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);

	/* ? */
	/* register_chrdev_region? */
	ret = register_chrdev_region(dev_no, lunix_minor_cnt, name);
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}	
	
	/* ? */
	/* cdev_add? */
	ret = cdev_add(&lunix_chrdev_cdev, dev_no, 1);
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

// Done for now
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
