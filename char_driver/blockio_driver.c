#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/jiffies.h>
#include <linux/param.h>

#define CONFIG_TIMEOUT

MODULE_LICENSE("GPL");

static unsigned int buflen = 10;
module_param(buflen, uint, 0444);

struct scull_device {
	struct miscdevice miscd;
	/* Queue for blocked readers */
	wait_queue_head_t inq;
	/* Queue for blocked writers */
	wait_queue_head_t outq;
	/* Read and write trackers */
	int wp;
	int rp;
	/* Data buffer */
	char *buf;
	unsigned int buflen;
	struct semaphore sem;
} scullb;
/*
 * Simulates a blocking read using
 * wait queues.
 */
static ssize_t scullb_read(struct file *filp, char __user *ubuf, size_t bytes, loff_t *loff)
{
	int ret;
	int bytes2read;
	int bytesread;
	unsigned long timeout;

	pr_info("%s entre %d\n", __func__, scullb.wp);
	pr_info("%s entre %d\n", __func__, scullb.rp);

	ret = down_interruptible(&scullb.sem);
	if (ret < 0)
		return ret;
	/* Read sleeps if buffer is empty */
	while (scullb.wp == scullb.rp) {         /* Buffer empty */
		up(&scullb.sem);
		if ((filp->f_flags & O_NONBLOCK) == O_NONBLOCK)
			return -EAGAIN;

		/* Setting timeout to 10seconds, on x86 'HZ' is 100/250(?) */
		#ifdef CONFIG_TIMEOUT
		timeout = (HZ * 10);
		ret = wait_event_interruptible_timeout(scullb.inq, scullb.wp != scullb.rp, timeout);
		#else
		ret = wait_event_interruptible(scullb.inq, scullb.wp != scullb.rp);
		#endif
		if (ret < 0)
			return ret;
		#ifdef CONFIG_TIMEOUT
		pr_info("timeout is %lu\n", timeout);
		#endif
		if (down_interruptible(&scullb.sem) < 0)
			return -ERESTARTSYS;
	}

	if (scullb.wp > scullb.rp) {
		bytes2read = scullb.wp - scullb.rp;
		bytes2read = (bytes2read < bytes) ? bytes2read : bytes;
		bytesread = copy_to_user(ubuf, &scullb.buf[scullb.rp], bytes2read);
	} else {
		bytes2read = bytes;
		if (bytes2read > (scullb.buflen - scullb.rp)) {
			bytes2read = scullb.buflen - scullb.rp;
			bytesread = copy_to_user(ubuf, &scullb.buf[scullb.rp], bytes2read);
		} else {
			bytesread = copy_to_user(ubuf, &scullb.buf[scullb.rp], bytes2read);
		}
	}
	scullb.rp = (scullb.rp + (bytes2read - bytesread)) % scullb.buflen;
	up(&scullb.sem);

	/* I believe wakeup_interruptible can be scheduled out before call completes */
	wake_up_interruptible(&scullb.outq);
	pr_info("%s extre %d\n", __func__, scullb.wp);
	pr_info("%s extre %d\n", __func__, scullb.rp);
	return bytes - bytesread;
}

static ssize_t scullb_write(struct file *filp, const char __user *ubuf, size_t bytes, loff_t *loff)
{
	int ret;
	int bytes2write;
	int byteswritten;
	unsigned long timeout;

	timeout = HZ * 10;
	pr_info("%s entre %d\n", __func__, scullb.wp);
	pr_info("%s entre %d\n", __func__, scullb.rp);

	ret = down_interruptible(&scullb.sem);
	if (ret < 0)
		return ret;
	/* Write sleeps if buffer is full */	
	while ((scullb.wp + 1) % scullb.buflen == scullb.rp) {             /* Buffer full */
		up(&scullb.sem);
		if ((filp->f_flags & O_NONBLOCK) == O_NONBLOCK)
			return -EAGAIN;

		#ifdef CONFIG_TIMEOUT
		ret = wait_event_interruptible_timeout(scullb.outq, ((scullb.wp + 1) % scullb.buflen) != scullb.rp, timeout);
		#else
		ret = wait_event_interruptible(scullb.outq, ((scullb.wp + 1) % scullb.buflen) != scullb.rp);
		#endif
		if (ret < 0)
			return ret;
		if (down_interruptible(&scullb.sem) < 0)
			return -ERESTARTSYS;
	}
	
	/* Find the correct size dependent on bytes, rp and wp, end of buffer */
	if (scullb.wp  < scullb.rp) {
		bytes2write = scullb.rp - scullb.wp - 1;
		bytes2write = (bytes2write < bytes) ? bytes2write : bytes;
		byteswritten = copy_from_user(&scullb.buf[scullb.wp], ubuf, bytes2write);

	} else {
		bytes2write = bytes;
		if (bytes2write > (scullb.buflen - scullb.wp)) {
			bytes2write = scullb.buflen - scullb.wp - 1;
			byteswritten = copy_from_user(&scullb.buf[scullb.wp], ubuf, bytes2write);
		} else {
			byteswritten = copy_from_user(&scullb.buf[scullb.wp], ubuf, bytes2write);
		}
	}
	scullb.wp = (scullb.wp + (bytes2write - byteswritten)) % scullb.buflen;
	up(&scullb.sem);

	wake_up_interruptible(&scullb.inq);
	pr_info("%s extre %d\n", __func__, scullb.wp);
	pr_info("%s extre %d\n", __func__, scullb.rp);
	return bytes2write - byteswritten;
}

static int scullb_close(struct inode *inode, struct file *filp)
{
	pr_info("scullb close\n");
	return 0;
}

static const struct file_operations scullb_fops = {
	.read = scullb_read,
	.write = scullb_write,
	.release = scullb_close
};

static int __init init_world(void)
{
	int ret;

	pr_info("init sleep driver\n");
	init_waitqueue_head(&scullb.inq);
	init_waitqueue_head(&scullb.outq);
	pr_info("init waitqueue head\n");

	scullb.buflen = buflen;
	/* Request memory for circ buffer */
	scullb.buf = kmalloc_array(buflen, sizeof(char *), GFP_KERNEL);
	if (!scullb.buf)
		return -ENOMEM;

	/* Print the size of memory block allocated by kmalloc from slab cache */
	pr_info("size allocated for buffer %lu\n", ksize(scullb.buf));
	scullb.wp = 0;
	scullb.rp = scullb.wp;

	/* Initialize Semaphore */
	sema_init(&scullb.sem, 1);

	/* Register char device as misc device */
	scullb.miscd.name = "chsleep1";
	scullb.miscd.minor = MISC_DYNAMIC_MINOR;
	scullb.miscd.fops = &scullb_fops;
	ret = misc_register(&scullb.miscd);
	if (ret < 0)
		return ret;

	pr_info("init miscellaneous dev\n");

	return 0;
}

static void __exit exit_world(void)
{
	pr_info("Goodbye world");
	misc_deregister(&scullb.miscd);
	kfree(scullb.buf);
}

module_init(init_world);
module_exit(exit_world);
