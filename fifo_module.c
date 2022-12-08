#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>

#define BUFF_SIZE 16

MODULE_LICENSE("Dual BSD/GPL");

dev_t fifo_dev_id;
static struct class *fifo_class;
static struct device *fifo_device;
static struct cdev *fifo_cdev;

int OpenFifo(struct inode *pinode, struct file *pfile);
int CloseFifo(struct inode *pinode, struct file *pfile);
ssize_t ReadFifo(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t WriteFifo(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);

static int end_read = 0;

static unsigned char read_pos    = 0u;
static unsigned char write_pos   = 0u;
static unsigned char element_cnt = 0u;

static unsigned char fifo_buffer[BUFF_SIZE];

static struct semaphore sem;

static wait_queue_head_t read_queue;
static wait_queue_head_t write_queue;

struct file_operations fifo_fops =
{
.owner = THIS_MODULE,
.open = OpenFifo,
.read = ReadFifo,
.write = WriteFifo,
.release = CloseFifo,
};

int OpenFifo(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully opened FIFO buffer.\n");
	return 0;
}

int CloseFifo(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully closed FIFO buffer.\n");
	return 0;
}

ssize_t ReadFifo(struct file *pfile, char __user *buffer, size_t length, loff_t *offset)
{
	int ret;
	char temp_buff[20];
	long int len = 0;

	if (end_read)
	{
		end_read = 0;
		return 0;
	}

	if(down_interruptible(&sem))
		return -ERESTARTSYS;
	
	while(element_cnt == 0)
	{
		up(&sem);	
		if(wait_event_interruptible(read_queue,(element_cnt > 0)))
			return -ERESTARTSYS;
		if(down_interruptible(&sem))
		return -ERESTARTSYS;
	}

	
	if(element_cnt > 0)
	{
		len = scnprintf(temp_buff, strlen(temp_buff), "%d ", fifo_buffer[read_pos]);
		ret = copy_to_user(buffer, temp_buff, len);
		if(ret)
			return -EFAULT;
		printk(KERN_INFO "Succesfully read %s from FIFO buffer.\n", temp_buff);
		
		if (read_pos == (BUFF_SIZE-1))
		{
			read_pos = 0;
		} else
		{
			read_pos++;
		}
		
		element_cnt--;
	}
	else
	{
		printk(KERN_WARNING "FIFO is empty.\n");
		return 0;
	}
	
	up(&sem);
	wake_up_interruptible(&write_queue);
	end_read = 1;
	
	return len;
}

ssize_t WriteFifo(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset)
{
	char temp_buff[BUFF_SIZE];
	int value;
	int ret;
	
	ret = copy_from_user(temp_buff, buffer, length);
	
	if(ret)
		return -EFAULT;
	
	temp_buff[length-1] = '\0';
	
	if(down_interruptible(&sem))
		return -ERESTARTSYS;

	while(element_cnt == BUFF_SIZE)
	{
		up(&sem);
		if(wait_event_interruptible(write_queue,(element_cnt < BUFF_SIZE)))
			return -ERESTARTSYS;
		if(down_interruptible(&sem))
			return -ERESTARTSYS;
	}

	if(element_cnt < BUFF_SIZE)
	{
		ret = sscanf(temp_buff,"%d",&value);
		if(ret==1)//one parameter parsed in sscanf
		{
			fifo_buffer[write_pos] = value;
			printk(KERN_INFO "Succesfully wrote value %d", value);
			
			if (write_pos == (BUFF_SIZE-1))
			{
				write_pos = 0;
			} else
			{
				write_pos++;
			}
			
			element_cnt++;
		}
		else
		{
			printk(KERN_WARNING "Wrong command format\n");
		}
	}
	else
	{
		printk(KERN_WARNING "Fifo is full.\n");
	}
	
	up(&sem);
	wake_up_interruptible(&read_queue);
	
	return length;
}

static int __init FifoInit(void)
{
    sema_init(&sem, 1);
	init_waitqueue_head(&write_queue);
	init_waitqueue_head(&read_queue);
	
	int ret = 0;
	ret = alloc_chrdev_region(&fifo_dev_id, 0, 1, "fifo_module");
	
	if (ret)
	{
		printk(KERN_ERR "failed to register char device.\n");
		return ret;
	}
	
	printk(KERN_INFO "char device region allocated.\n");
	fifo_class = class_create(THIS_MODULE, "fifo_class");
	if (fifo_class == NULL)
	{
		printk(KERN_ERR "failed to create class.\n");
		goto FAIL_0;
	}	
	
	printk(KERN_INFO "class created.\n");
	fifo_device = device_create(fifo_class, NULL, fifo_dev_id, NULL, "fifo_module");
	
	if (fifo_device == NULL)
	{
		printk(KERN_ERR "failed to create device.\n");
		goto FAIL_1;
	}
	
	printk(KERN_INFO "device created.\n");

	fifo_cdev = cdev_alloc();
	fifo_cdev->ops = &fifo_fops;
	fifo_cdev->owner = THIS_MODULE;

	ret = cdev_add(fifo_cdev, fifo_dev_id, 1);
	
	if (ret)
	{
		printk(KERN_ERR "failed to add cdev.\n");
		goto FAIL_2;
	}
 
	printk(KERN_INFO "cdev added.\n");
	printk(KERN_INFO "'Hello world' a newly born FIFO buffer said.\n");
	
	return 0;
FAIL_2:
	device_destroy(fifo_class, fifo_dev_id);
FAIL_1:
	class_destroy(fifo_class);
FAIL_0:
	unregister_chrdev_region(fifo_dev_id, 1);
	return -1;
}
static void __exit FifoExit(void)
{
	cdev_del(fifo_cdev);
	device_destroy(fifo_class, fifo_dev_id);
	class_destroy(fifo_class);
	unregister_chrdev_region(fifo_dev_id,1);
	printk(KERN_INFO "'Goodbye, cruel world' FIFO buffer said right before its sad life ended.\n");
}

module_init(FifoInit);
module_exit(FifoExit);
