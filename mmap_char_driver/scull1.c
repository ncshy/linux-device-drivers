#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include "scull.h"

#define MAX_DEVICE 1
#define QLEN 1000
#define BLOCK_LEN 4000

MODULE_LICENSE("GPL");

dev_t scull_id;
int major;
int minor;
struct qset *curset;

struct scull_device {
	struct qset *qset_hd;
	int qset_len;
	int used_qblk;
	int datalen;
	struct semaphore sem;
	struct cdev chardev;
} scull_dev;

/* A block of memory consisting of QLEN blocks
   of BLOCK_LEN each
   data[0] - Block 0
   data[1] - Block 1
 */
struct qset {
	void **data;
	struct qset *next;
};

static int scull_trunc(struct scull_device *sculld)
{
	struct qset *qset, *next;
	//Clear all quantum sets.
	for (qset = sculld->qset_hd; qset != NULL; qset = next) {
		next = qset;
		if (qset->data != NULL) {
			int i = 0;

			for (i = 0; i < sculld->used_qblk; i++) { /*TODO: sculld->datalen has to be modified for multiple quantas */
				/*unlikely is a macro defined in kernel headers */
				if (unlikely(!qset->data[i])) {
					pr_info("Trying to free uninitialized block\n");
					return -1;
				}

				kfree(qset->data[i]);
			}
			kfree(qset->data);
			next = qset->next;
			kfree(qset);
		} else {
			kfree(qset);
			break;
		}
	}
	sculld->qset_hd = NULL;
	return 0;
}

static int scull_open(struct inode *idev, struct file *filp)
{
	struct scull_device *sculld;
	int ret = 0;

	pr_alert("Opening file\n");

	sculld = container_of(idev->i_cdev, struct scull_device, chardev);
	if (unlikely(!filp))
		return -1;

	filp->private_data = sculld;

	if (!sculld) {
		pr_alert("Failed to obtain container\n");
		return -1;
	}

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		pr_alert("Write only mode\n");
		ret = down_interruptible(&sculld->sem);
		if (ret < 0)
			return ret;
		ret = scull_trunc(sculld);
		if (ret < 0) {
			up(&sculld->sem);
			return ret;
		}
		sculld->used_qblk = 0;
		sculld->datalen = 0;
		up(&sculld->sem);
	}

	return ret;
}

static ssize_t scull_read(struct file *filp, char __user *ubuf, size_t buflen, loff_t *loff)
{
	struct scull_device *sculld = filp->private_data;
	int ret;
	int bytes2read;
	int readbytes;
	int off = 0;
	int i = 0;

	pr_alert("Reading file %lu\n", buflen);
	if (!sculld)
		return -ENODEV;
	if (!curset)
		return 0;

	bytes2read = (sculld->used_qblk * BLOCK_LEN) + sculld->datalen;
	pr_info("bytes2read: %d\n", bytes2read);
	pr_info("*loff in read : %lld\n", *loff);

	off = *loff % BLOCK_LEN;
	if (*loff >= BLOCK_LEN)
		i = *loff / BLOCK_LEN;

	/* Read 4KB/bytes2read/buflen at a time until limit is reached */
	if ((signed int)(*loff) < bytes2read) {  /* loff is current offset for this fd */
		if (buflen <= BLOCK_LEN) {
			if (bytes2read > (*loff + buflen)) {
				ret = copy_to_user(ubuf, curset->data[i] + off, buflen);
				readbytes = buflen - ret;
			} else {
				ret = copy_to_user(ubuf, curset->data[i] + off, bytes2read - off);
				readbytes = bytes2read - off - ret;
			}
		} else {
			ret = copy_to_user(ubuf, curset->data[i] + off, BLOCK_LEN);
			readbytes = buflen - ret;
		}
		*loff += readbytes;
	} else { /* Return value unspecified if bytes requested is more, prevent retry */
		readbytes = buflen;
	}


	return readbytes;
}

static int init_qset(struct scull_device *sculld, struct qset **curset)
{
	int i;
	int k = 0;

	if (!sculld)
		return -1;

	sculld->qset_hd = kmalloc(sizeof(struct qset), GFP_KERNEL);

	if (!sculld->qset_hd) {
		pr_alert("kmalloc failed for quantum\n ");
		return -1;
	}

	*curset = sculld->qset_hd;

	(*curset)->data = kmalloc_array(QLEN, sizeof(char *), GFP_KERNEL);
	if (!((*curset)->data)) {
		pr_alert("kmalloc failed for quantum->data\n ");
		kfree(*curset);
		return -1;
	}

	for (i = 0; i < QLEN; i++) {
		(*curset)->data[i] = kmalloc(BLOCK_LEN, GFP_KERNEL);
		if (!((*curset)->data[i])) {
			pr_alert("kmalloc failed for quantum->data\n ");
			for (k = 0; k < i; k++) {
				if (!(*curset)->data[k])
					continue;
				kfree((*curset)->data[k]);
			}

			kfree((*curset)->data);
			kfree(*curset);
			return -1;
		}
	}
	(*curset)->next = NULL;
	return 0;
}


static ssize_t scull_write(struct file *filp, const char __user *ubuf, size_t buflen, loff_t *loff)
{
	/* Write buffer to memory. */
	struct scull_device *sculld = filp->private_data;
	int copied;
	int ret;
	int quanta = BLOCK_LEN * QLEN;

	pr_alert("Writing file %lu\n", buflen);

	if (!sculld)
		return -1;

	ret = down_interruptible(&sculld->sem);
	if (ret < 0)
		return ret;
	/*Allocate memory for the quantum set, assign qset address to head of qset list. */
	if (!sculld->qset_hd) {
		ret = init_qset(sculld, &curset);
		if (ret < 0) {
			up(&sculld->sem);
			return -ENOMEM;
		}
	} else {
		curset = sculld->qset_hd;
	}

	if ((*loff + buflen) <= quanta) {
		/* Copying at most a block at a time */
		if (buflen >= BLOCK_LEN) {
			/* copy_from_user returns bytes still to be copied */
			ret = copy_from_user(curset->data[sculld->used_qblk] + sculld->datalen, ubuf, BLOCK_LEN);
			if (ret < 0) {
				up(&sculld->sem);
				return -EFAULT;
			}
			/* So copied contains (requested copy length - bytes still to be copied) */
			copied = (BLOCK_LEN - ret);
		} else {
			ret = copy_from_user(curset->data[sculld->used_qblk] + sculld->datalen, ubuf, buflen);
			if (ret < 0) {
				up(&sculld->sem);
				return -EFAULT;
			}
			copied = (buflen - ret);
		}

		/*Update used blocks and offset within block */
		sculld->used_qblk = ((sculld->used_qblk * BLOCK_LEN) + copied) / BLOCK_LEN;
		sculld->datalen = ((sculld->used_qblk * BLOCK_LEN) + copied) % BLOCK_LEN;
		*loff = *loff + copied;


		up(&sculld->sem);
		return copied;
	} else {
		/*TODO: Copy remaining bytes in this quantaa

		  Create new quanta

		  Continue copy in new quanta

		  Updata next pointer and qset len
		 */
	}

	up(&sculld->sem);
	return buflen;
}

static long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long ret;

	switch (cmd) {
	case SCULL_IOC_QQUANTA:
		ret = QLEN * BLOCK_LEN;
		break;

	case SCULL_IOC_TQUANTA:
		break;

	default:
		return -ENOTTY;
	}

	return ret;
}

static void scull_vma_open(struct vm_area_struct *vma)
{
	pr_alert("Inside mmap vma open\n");
}

static void scull_vma_close(struct vm_area_struct *vma)
{
	pr_alert("Inside mmap vma close\n");
}

const struct vm_operations_struct scull_vma_ops = {
	.open = scull_vma_open,
	.close = scull_vma_close
};

static int scull_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	unsigned long physaddr;
	unsigned long pfn;
	struct scull_device *sculld = filp->private_data;

	pr_info("Inside mmap call\n");
	physaddr = __pa(sculld->qset_hd->data[0]);
	pfn = physaddr >> PAGE_SHIFT;
	pr_info("virt_addr: %lx; physaddr: %lx; pfn : %lu\n", (unsigned long)sculld->qset_hd->data, physaddr, pfn);
	pr_info("data[0] is %c\n", ((char *)sculld->qset_hd->data[0])[0]);

	ret = remap_pfn_range(vma, vma->vm_start, pfn, vma->vm_end - vma->vm_start, vma->vm_page_prot);
	if (ret < 0)
		return ret;


	vma->vm_ops = &scull_vma_ops;
	scull_vma_open(vma);

	return 0;
}

static int scull_close(struct inode *idev, struct file *filp)
{
	pr_info("Closing file\n");
	return 0;
}

const struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.open = scull_open,
	.read = scull_read,
	.write = scull_write,
	.unlocked_ioctl = scull_ioctl,
	.mmap = scull_mmap,
	.release = scull_close
};

static int cdev_setup(void)
{
	int err;
	int devno;

	devno = MKDEV(major, minor);
	//initialize cdev structure
	cdev_init(&(scull_dev.chardev), &scull_fops);
	scull_dev.chardev.owner = THIS_MODULE;
	scull_dev.chardev.ops = &scull_fops;
	scull_dev.qset_len = 1;
	scull_dev.used_qblk = 0;
	scull_dev.datalen = 0;
	sema_init(&scull_dev.sem, 1);

	//add cdev structurei
	err = cdev_add(&(scull_dev.chardev), devno, MAX_DEVICE);

	return err;
}

static int __init scull_init(void)
{
	int i = 0;
	int err;

	minor = 0;
	curset = NULL;
	//Ask system to assign major number for device
	while (i++ < MAX_DEVICE) {
		int ret = alloc_chrdev_region(&scull_id, minor, MAX_DEVICE, "scull1");

		if (ret)
			goto fail;
	}

	major = MAJOR(scull_id);
	pr_alert("scull id is %x\n", scull_id);
	err = cdev_setup();
	if (err)
		goto fail;
	pr_alert("Loaded %s\n", __func__);
	return 0;


fail:
	//unalloc_region
	unregister_chrdev_region(scull_id, MAX_DEVICE);

	pr_alert("Error in %s\n", __func__);
	return -1;
}

static void __exit scull_exit(void)
{
	//free buffer memory
	scull_trunc(&sculld);
	//Delete device
	cdev_del(&(scull_dev.chardev));
	//unregister driver
	unregister_chrdev_region(scull_id, MAX_DEVICE);

	pr_alert("Exited %s\n", __func__);
}

module_init(scull_init);
module_exit(scull_exit);
