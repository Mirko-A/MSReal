#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/string.h>

#define MAX_STR_SIZE 			(100u)
#define b_TRUE                  		(1u)
#define b_FALSE                		(0u)
#define OK									(0u)
#define ERROR                   		(-1)
#define NUM_OF_COMMANDS 	(7u)

MODULE_LICENSE("Dual BSD/GPL");

typedef enum 
{
	STRING = 0,
	APPEND,
	TRUNCATE,
	REMOVE,
	CLEAR,
	SHRINK,
	HELP
} Command_t;

dev_t stred_dev_id;
static struct class *stred_class;
static struct device *stred_device;
static struct cdev *stred_cdev;

int OpenStred(struct inode *pinode, struct file *pfile);
int CloseStred(struct inode *pinode, struct file *pfile);
ssize_t ReadStred(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t WriteStred(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);

// Commands which need a subcommand.
ssize_t CallCommandString(void);
ssize_t CallCommandAppend(void);
ssize_t CallCommandTruncate(void);
ssize_t CallCommandRemove(void);

// Commands which don't need a subcommand.
ssize_t CallCommandClear(void);
ssize_t CallCommandShrink(void);
ssize_t CallCommandHelp(void);

static struct semaphore sem;

static int end_read = 0;													///< Indicates whether ReadStred should stop reading or not.

const static char *commands[NUM_OF_COMMANDS] = {"string=%s", "append=%s", "truncate=%s", "remove=%s", "clear", "shrink", "help"};

static size_t char_cnt = 0u;											///< Number of characters currently inside the string.
static char string[MAX_STR_SIZE] = { 0 };

static wait_queue_head_t read_queue;
static wait_queue_head_t write_queue;

struct file_operations stred_fops =
{
.owner = THIS_MODULE,
.open = OpenStred,
.read = ReadStred,
.write = WriteStred,
.release = CloseStred,
};

int OpenStred(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully opened String editor.\n");
	return 0;
}

int CloseStred(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully closed String editor.\n");
	return 0;
}

ssize_t ReadStred(struct file *pfile, char __user *buffer, size_t length, loff_t *offset)
{
	int ret;
	
	long int len = 0;
	
	// cat stred will try to read from file as long as the return value is not 0 so we return 0 (OK) after reading once.
	if (end_read)
	{
		end_read = 0;
		return OK;
	}

	len = strlen(string);
	ret = copy_to_user(buffer, string, len);
		
	if(ret) return -EFAULT;
		
	end_read = 1;
	
	return len;
}

ssize_t WriteStred(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset)
{
	char temp_buff[MAX_STR_SIZE] = { 0 };
	char subcommand[MAX_STR_SIZE] = { 0 };

	int ret;
	int command_it;
	
	ret = copy_from_user(temp_buff, buffer, length);
	
	if(ret) return -EFAULT;
	
	temp_buff[length-1] = '\0';
	
	for (command_it = 0; command_it < NUM_OF_COMMANDS; command_it++)
	{
		if (command_it < 4)
		{
			ret = sscanf(temp_buff, commands[command_it], subcommand);	
			
			if (ret == b_TRUE)
			{
				switch (command_it)
				{
					case STRING:
					{
						ret = CallCommandString();
					}
					break;
					case APPEND:
					{
						ret = CallCommandAppend();
					}
					break;
					case TRUNCATE:
					{
						ret = CallCommandTruncate();
					}
					break;
					case REMOVE:
					{
						ret = CallCommandRemove();
					}
					break;
					default:
						printk (KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");
						return -ERESTARTSYS;
						break;
				}
			}
		} else
		{
			ret = strcmp(temp_buff, commands[command_it]);
			
			if (ret == OK)
			{
				switch (command_it)
				{
					case CLEAR:
					{
						ret = CallCommandClear();
					}
					break;
					case SHRINK:
					{
						ret = CallCommandShrink();
					}
					break;
					case HELP:
					{
						ret = CallCommandHelp();
					}
					break;
					default:
						printk (KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");
						return -ERESTARTSYS;
						break;
				}
			}
		}
	}
	
	printk (KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");
	
	return ERROR;
	// if(down_interruptible(&sem)) return -ERESTARTSYS;

	// String full
	// while(char_cnt == MAX_STR_SIZE)
	// {
	// 	up(&sem);
	// 	// Put process in write queue
	// 	if(wait_event_interruptible(write_queue,(char_cnt < MAX_STR_SIZE))) return -ERESTARTSYS;
	// 	if(down_interruptible(&sem)) return -ERESTARTSYS;
	// }
		
	// Multiple processes can be in write queue and will all be released at once so
	// an additional check is necessary since only one of them can write 
	// if(char_cnt < MAX_STR_SIZE)
	// {
		// Write implementation TODO
			

	// }
	// else
	// {
	// 	printk(KERN_WARNING "String max size reached.\n");
	// }
		
	// up(&sem);
	// One (or more) element has been written, read queue can be released
	// wake_up_interruptible(&read_queue);
}

static int __init StredInit(void)
{
	int ret;
	
    sema_init(&sem, 1);
	init_waitqueue_head(&write_queue);
	init_waitqueue_head(&read_queue);
	
	ret = alloc_chrdev_region(&stred_dev_id, 0, 1, "stred_module");
	
	if (ret)
	{
		printk(KERN_ERR "failed to register char device.\n");
		return ret;
	}
	
	printk(KERN_INFO "char device region allocated.\n");
	stred_class = class_create(THIS_MODULE, "stred_class");
	if (stred_class == NULL)
	{
		printk(KERN_ERR "failed to create class.\n");
		goto FAIL_0;
	}	
	
	printk(KERN_INFO "class created.\n");
	stred_device = device_create(stred_class, NULL, stred_dev_id, NULL, "stred_module");
	
	if (stred_device == NULL)
	{
		printk(KERN_ERR "failed to create device.\n");
		goto FAIL_1;
	}
	
	printk(KERN_INFO "device created.\n");

	stred_cdev = cdev_alloc();
	stred_cdev->ops = &stred_fops;
	stred_cdev->owner = THIS_MODULE;

	ret = cdev_add(stred_cdev, stred_dev_id, 1);
	
	if (ret)
	{
		printk(KERN_ERR "failed to add cdev.\n");
		goto FAIL_2;
	}
 
	printk(KERN_INFO "cdev added.\n");
	printk(KERN_INFO "'Hello world' a newly born String editor said.\n");
	
	return 0;
FAIL_2:
	device_destroy(stred_class, stred_dev_id);
FAIL_1:
	class_destroy(stred_class);
FAIL_0:
	unregister_chrdev_region(stred_dev_id, 1);
	return -1;
}
static void __exit StredExit(void)
{
	cdev_del(stred_cdev);
	device_destroy(stred_class, stred_dev_id);
	class_destroy(stred_class);
	unregister_chrdev_region(stred_dev_id,1);
	printk(KERN_INFO "'Goodbye, cruel world' String editor said right before its sad life ended.\n");
}

// Commands which need a subcommand.
ssize_t CallCommandString()
{
	printk(KERN_INFO "'Called STRING command.\n");
	return 0;
}

ssize_t CallCommandAppend()
{
	printk(KERN_INFO "'Called APPEND command.\n");
	return 0;
}

ssize_t CallCommandTruncate()
{
	printk(KERN_INFO "'Called TRUNCATE command.\n");
	return 0;
}

ssize_t CallCommandRemove()
{
	printk(KERN_INFO "'Called REMOVE command.\n");
	return 0;
}

// Commands which don't need a subcommand.
ssize_t CallCommandClear()
{
	printk(KERN_INFO "'Called CLEAR command.\n");
	return 0;
}

ssize_t CallCommandShrink()
{
	printk(KERN_INFO "'Called SHRINK command.\n");
	return 0;
}

ssize_t CallCommandHelp()
{
	printk(KERN_INFO "'Called HELP command.\n");
	return 0;
}

module_init(StredInit);
module_exit(StredExit);
