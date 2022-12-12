#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define MAX_STR_SIZE    (101u)
#define b_TRUE          (1u)
#define b_FALSE         (0u)
#define OK              (0u)
#define ERROR           (-1)
#define NUM_OF_COMMANDS (7u)

MODULE_LICENSE("Dual BSD/GPL");

/// Enum which lists all commands for String editor
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

int	OpenStred(struct inode *pinode, struct file *pfile);
int	CloseStred(struct inode *pinode, struct file *pfile);
ssize_t	ReadStred(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t	WriteStred(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);

// Commands which need a subcommand.
static ssize_t CallCommandString(char subcmd[]); 	///< Format: string=abc -> sets the string to 'abc'.
static ssize_t CallCommandAppend(char subcmd[]);	///< Format: append=abc -> appends 'abc' to the string.
static ssize_t CallCommandTruncate(char subcmd[]);	///< Format: truncate=x -> truncates x characters from the string.
static ssize_t CallCommandRemove(char subcmd[]);	///< Format: remove=abc -> removes all occurances of 'abc' from the string.

// Commands which don't need a subcommand.
static ssize_t CallCommandClear(void);				///< Format: clear  -> clears the string.
static ssize_t CallCommandShrink(void);				///< Format: shrink -> removes all whitespace characters at the start and end of the string.
static ssize_t CallCommandHelp(void);				///< Format: help   -> lists all possible commands.

// Call command wrapper
static ssize_t CallCommand(Command_t command_id);
// Call command with subcommand wrapper
static ssize_t CallCommandWithSub(Command_t command_id, char subcmd[]);

static struct semaphore  sem;

const static char        help_msg[] = "----------  STRED COMMANDS ----------\nFormat: string=abc -> sets the string to 'abc'.\nFormat: append=abc -> appends 'abc' to the string.\nFormat: truncate=x -> truncates x characters from the string.\nFormat: remove=abc -> removes all occurances of 'abc' from the string.\nFormat: clear      -> clears the string.\nFormat: shrink     -> removes all whitespace characters at the start and end of the string.\n";

const static char        *commands[NUM_OF_COMMANDS] = {"string=%s", "append=%s", "truncate=%s", 
													   "remove=%s", "clear", "shrink", "help"};

static int               end_read = 0;		///< Indicates whether ReadStred should stop reading or not.

static size_t            char_cnt = 0u;		///< Number of characters currently inside the string.

static char				 string[MAX_STR_SIZE] = {0};

static wait_queue_head_t trunc_queue;
static wait_queue_head_t append_queue;

struct file_operations	 stred_fops =
	{
		.owner = THIS_MODULE,
		.open = OpenStred,
		.read = ReadStred,
		.write = WriteStred,
		.release = CloseStred,
};

int	OpenStred(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully opened String editor.\n");
	return (0);
}

int	CloseStred(struct inode *pinode, struct file *pfile)
{
	printk(KERN_INFO "Succesfully closed String editor.\n");
	return (0);
}

ssize_t	ReadStred(struct file *pfile, char __user *buffer, size_t length,
		loff_t *offset)
{
	int ret;

	long int len = 0;

	// cat stred will try to read from file as long as the return value is not 0 so we return 0 (OK) after reading once.
	if (end_read)
	{
		end_read = 0;
		return (OK);
	}

	len = strlen(string);
	ret = copy_to_user(buffer, string, len);

	if (ret) return (-EFAULT);

	printk(KERN_INFO "Succesfully read string %s.\n", string);

	end_read = 1;

	return (len);
}

ssize_t	WriteStred(struct file *pfile, const char __user *buffer, size_t length,
		loff_t *offset)
{
	char temp_buff[MAX_STR_SIZE] = {0};
	char subcmd[MAX_STR_SIZE]    = {0};

	int ret;
	int command_it;

	ret = copy_from_user(temp_buff, buffer, length);

	if (ret)
		return (-EFAULT);

	temp_buff[length - 1] = '\0';

	// Check if user input matches any of the predefined commands
	for (command_it = 0; command_it < NUM_OF_COMMANDS; command_it++)
	{
		if (command_it < 4)
		{
			ret = sscanf(temp_buff, commands[command_it], subcmd);

			if (ret == b_TRUE)
			{
				CallCommandWithSub((Command_t)command_it, subcmd);
				return length;
			}
		}
		else
		{
			ret = strcmp(temp_buff, commands[command_it]);

			if (ret == OK)
			{
				CallCommand((Command_t)command_it);
				return length;
			}
		}
	}

	// User input invalid
	printk(KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");

	return ERROR;
}

static int __init	StredInit(void)
{
	int ret;

	sema_init(&sem, 1);
	init_waitqueue_head(&append_queue);
	init_waitqueue_head(&trunc_queue);

	ret = alloc_chrdev_region(&stred_dev_id, 0, 1, "stred_module");

	if (ret)
	{
		printk(KERN_ERR "failed to register char device.\n");
		return (ret);
	}

	printk(KERN_INFO "char device region allocated.\n");
	stred_class = class_create(THIS_MODULE, "stred_class");
	if (stred_class == NULL)
	{
		printk(KERN_ERR "failed to create class.\n");
		goto FAIL_0;
	}

	printk(KERN_INFO "class created.\n");
	stred_device = device_create(stred_class, NULL, stred_dev_id, NULL,
			"stred_module");

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

	return (0);
FAIL_2:
	device_destroy(stred_class, stred_dev_id);
FAIL_1:
	class_destroy(stred_class);
FAIL_0:
	unregister_chrdev_region(stred_dev_id, 1);
	return (-1);
}
static void __exit	StredExit(void)
{
	cdev_del(stred_cdev);
	device_destroy(stred_class, stred_dev_id);
	class_destroy(stred_class);
	unregister_chrdev_region(stred_dev_id, 1);
	printk(KERN_INFO "'Goodbye, cruel world' String editor said right before its sad life ended.\n");
}

// Commands which need a subcommand.
static ssize_t CallCommandString(char subcmd[])
{	
	int ret;
	size_t len = strlen(subcmd);
	printk(KERN_INFO "Called STRING command with subcommand %s.\n", subcmd);

	if (len <= MAX_STR_SIZE)
	{
		printk(KERN_INFO "String successfully set to %s.\n", subcmd);
		strcpy(string, subcmd);

		if(down_interruptible(&sem)) return (-ERESTARTSYS);

		char_cnt = len;

		// One (or more) characters added to the string, truncate queue can be released
		wake_up_interruptible(&trunc_queue);
		
		up(&sem);

		ret = OK;
	} else
	{
		printk(KERN_INFO "String %s is too long.\n", subcmd);
		ret = ERROR;
	}

	return ret;
}

static ssize_t CallCommandAppend(char subcmd[])
{
	int ret;
	size_t len = strlen(subcmd);
	printk(KERN_INFO "Called APPEND command with subcommand %s.\n", subcmd);
	
	if(down_interruptible(&sem)) return (-ERESTARTSYS);

	// String full
	while((char_cnt + len) > (MAX_STR_SIZE - 1))
	{
		up(&sem);
		// Put process in write queue
		if(wait_event_interruptible(append_queue,((char_cnt + len) < MAX_STR_SIZE))) return (-ERESTARTSYS);
		
		if(down_interruptible(&sem)) return (-ERESTARTSYS);
	}

	// Multiple processes can be in write queue and will all be released at once so
	// an additional check is necessary since only one of them can write
	if((char_cnt + len) < MAX_STR_SIZE)
	{
		printk(KERN_INFO "Successfully appended %s to string.\n", subcmd);
		strcat(string, subcmd);
		
		char_cnt += len;

		printk(KERN_INFO "Character count is %zu.\n", char_cnt);

		ret = OK;
	}
	else
	{
	 	printk(KERN_WARNING "String max size reached.\n");
		
		ret = ERROR;
	}

	// One (or more) characters added to the string, truncate queue can be released
	wake_up_interruptible(&trunc_queue);
	up(&sem);

	return ret;
}

static ssize_t CallCommandTruncate(char subcmd[])
{
	int ret;
	size_t trunc_cnt;

	ret = sscanf(subcmd, "%zu", &trunc_cnt);

	if (ret)
	{
		if(down_interruptible(&sem)) return (-ERESTARTSYS);

		// Too many characters to truncate
		while(((int)char_cnt - (int)trunc_cnt) < 0)
		{
			up(&sem);
			// Put process in truncate queue
			if(wait_event_interruptible(trunc_queue,(((int)char_cnt - (int)trunc_cnt) >= 0))) return (-ERESTARTSYS);
		
			if(down_interruptible(&sem)) return (-ERESTARTSYS);
		}

		// Multiple processes can be in truncate queue and will all be released at once so
		// an additional check is necessary since only one of them can truncate
		if(((int)char_cnt - (int)trunc_cnt) >= 0)
		{
			printk(KERN_INFO "Successfully truncated %zu characters.\n", trunc_cnt);
			memset(&(string[(int)char_cnt - (int)trunc_cnt]), '\0', trunc_cnt);
			
			char_cnt -= trunc_cnt;
		
			printk(KERN_INFO "Character count is %zu.\n", char_cnt);

			ret = OK;
		}
		else
		{
	 		printk(KERN_WARNING "String is too short to truncate.\n");
			
			ret = ERROR;
		}

		// One (or more) characters truncated from the string, append queue can be released
		wake_up_interruptible(&append_queue);
		up(&sem);

		return ret;
	} else
	{
		printk(KERN_WARNING "Format incorrect. Please use the following format: truncate=x where x is a positive integer value.\n");
		return ERROR;
	}
}

static ssize_t CallCommandRemove(char subcmd[])
{
	printk(KERN_INFO "Called REMOVE command with subcommand %s.\n", subcmd);
	return 0;
}

// Commands which don't need a subcommand.
static ssize_t CallCommandClear(void)
{
	printk(KERN_INFO "Called CLEAR command.\n");

	if(down_interruptible(&sem)) return (-ERESTARTSYS);

	memset(string, '\0', MAX_STR_SIZE);
	char_cnt = 0;
	printk(KERN_INFO "String successfully cleared.\n");

	// One (or more) characters added to the string, truncate queue can be released
	wake_up_interruptible(&append_queue);
		
	up(&sem);
	
	return OK;
}

static ssize_t CallCommandShrink(void)
{
	printk(KERN_INFO "Called SHRINK command.\n");
	return 0;
}

static ssize_t CallCommandHelp(void)
{
	printk(KERN_INFO "%s", help_msg);
	return 0;
}

static ssize_t CallCommand(Command_t command_id)
{
	int ret; 

	switch (command_id)
	{
	case CLEAR:
	{
		ret = CallCommandClear();
	}
	break ;
	case SHRINK:
	{
		ret = CallCommandShrink();
	}
	break ;
	case HELP:
	{
		ret = CallCommandHelp();
	}
	break ;
	default:
		printk(KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");
		return (-ERESTARTSYS);
		break ;
	}

	return ret;
}

static ssize_t CallCommandWithSub(Command_t command_id, char subcmd[])
{
	int ret; 

	switch (command_id)
	{
	case STRING:
	{
		ret = CallCommandString(subcmd);
	}
	break ;
	case APPEND:
	{
		ret = CallCommandAppend(subcmd);
	}
	break ;
	case TRUNCATE:
	{
		ret = CallCommandTruncate(subcmd);
	}
	break ;
	case REMOVE:
	{
		ret = CallCommandRemove(subcmd);
	}
	break ;
	default:
		printk(KERN_WARNING "Command not recognized. Use echo \"help\" > stred_module to see the list of commands.\n");
		return (-ERESTARTSYS);
		break ;
	}

	return ret;
}

module_init(StredInit);
module_exit(StredExit);
