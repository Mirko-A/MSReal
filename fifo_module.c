#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/string.h>

#define BUFF_SIZE               (16u)
#define MAX_STR_SIZE        (64u)
#define BIN_FORMAT_SIZE  (8u)
#define b_TRUE                     (1u)
#define b_FALSE                   (0u)
#define ERROR                      (-1)


MODULE_LICENSE("Dual BSD/GPL");

dev_t fifo_dev_id;
static struct class *fifo_class;
static struct device *fifo_device;
static struct cdev *fifo_cdev;

static int binToDec(char binary_string[], int num_of_bits);
static void parseInput(char p_input_str[], int p_input_len);

int OpenFifo(struct inode *pinode, struct file *pfile);
int CloseFifo(struct inode *pinode, struct file *pfile);
ssize_t ReadFifo(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t WriteFifo(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);

static int end_read = 0;

static unsigned char read_pos    = 0u;
static unsigned char write_pos   = 0u;
static unsigned char element_cnt = 0u;

static int temp_values[BUFF_SIZE];
static int temp_value_cnt = 0u;

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
	char temp_buff[MAX_STR_SIZE];
	long int len = 0;

	if (end_read)
	{
		end_read = 0;
		return 0;
	}

	if(down_interruptible(&sem)) return -ERESTARTSYS;
	
	while(element_cnt == 0)
	{
		up(&sem);	
		
		if(wait_event_interruptible(read_queue,(element_cnt > 0))) return -ERESTARTSYS;
		
		if(down_interruptible(&sem)) return -ERESTARTSYS;
	}

	
	if(element_cnt > 0)
	{
		len = scnprintf(temp_buff, strlen(temp_buff), "%d ", fifo_buffer[read_pos]);
		ret = copy_to_user(buffer, temp_buff, len);
		
		if(ret) return -EFAULT;
		
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
	char temp_buff[MAX_STR_SIZE];

	int current_temp_value;
	int ret;
	
	ret = copy_from_user(temp_buff, buffer, length);
	
	if(ret) return -EFAULT;
	
	temp_buff[length-1] = '\0';
	
	parseInput(temp_buff, length);

	for (current_temp_value = 0; current_temp_value < temp_value_cnt; current_temp_value++)
	{
		if(down_interruptible(&sem)) return -ERESTARTSYS;

		while(element_cnt == BUFF_SIZE)
		{
			up(&sem);
			if(wait_event_interruptible(write_queue,(element_cnt < BUFF_SIZE))) return -ERESTARTSYS;
			if(down_interruptible(&sem)) return -ERESTARTSYS;
		}
		
		if(element_cnt < BUFF_SIZE)
		{
			fifo_buffer[write_pos] = temp_values[current_temp_value];
			printk(KERN_INFO "Succesfully wrote value %d", temp_values[current_temp_value]);
			
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
			printk(KERN_WARNING "Fifo is full.\n");
		}
		
		up(&sem);
		wake_up_interruptible(&read_queue);
	}
	
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

static int binToDec(char binary_string[], int num_of_bits) 
{
    int result = 0;
	int bit_cnt;
    
    for (bit_cnt = 0; bit_cnt < num_of_bits; bit_cnt++)
    {
		if ((binary_string[bit_cnt] < '0') || (binary_string[bit_cnt] > '1'))
        {
            return ERROR;
        } else if (binary_string[bit_cnt] == '1')
        {
            result |= (1 << (num_of_bits - bit_cnt - 1));
        }
    }

    return result;
}

static void parseInput(char p_input_str[], int p_input_len)
{
    char temp_bin[BIN_FORMAT_SIZE];
    int input_len = p_input_len;
    
	temp_value_cnt = 0u;
	
    int value = 0;
    char input_end_reached = b_FALSE;
	
	int input_char_cnt;
    
    for (input_char_cnt = 0; input_char_cnt < input_len; input_char_cnt++)
    {
        if (p_input_str[input_char_cnt] == ';' || p_input_str[input_char_cnt] == '\0')
        {
            if (p_input_str[input_char_cnt] == '\0')
            {
                input_end_reached = b_TRUE;
            }
            
            if ((input_char_cnt - BIN_FORMAT_SIZE - 2) >= 0)
            {
                if (p_input_str[(input_char_cnt - (int)BIN_FORMAT_SIZE - 2)] == '0' && p_input_str[input_char_cnt - BIN_FORMAT_SIZE - 1] == 'b')
                {
                    strncpy(temp_bin, &p_input_str[input_char_cnt - BIN_FORMAT_SIZE], BIN_FORMAT_SIZE);
                    value = binToDec(temp_bin, BIN_FORMAT_SIZE);
                    if (value == ERROR)
                    {
                        printk(KERN_WARNING "Invalid format.\n");
                    } else
                    {
                        temp_values[temp_value_cnt] = value;
						temp_value_cnt++;
                    }
                } else
                {
                    printk(KERN_WARNING "Invalid format.\n");
                }   
            } else 
            {
                printk(KERN_WARNING "Invalid format.\n");   
            }
        }
        
        if (input_end_reached == b_TRUE)
        {
            break;
        }
    }
}

module_init(FifoInit);
module_exit(FifoExit);
