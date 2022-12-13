#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/string.h>

#define BUFF_SIZE               (16u)
#define MAX_STR_SIZE            (64u)
#define BIN_FORMAT_SIZE         (8u)
#define READ_CHANGE_FORMAT_SIZE (5u)
#define b_TRUE                  (1u)
#define b_FALSE                 (0u)
#define OK                      (0u)
#define ERROR                   (-1)

MODULE_LICENSE("Dual BSD/GPL");

dev_t fifo_dev_id;
static struct class *fifo_class;
static struct device *fifo_device;
static struct cdev *fifo_cdev;

/** 
* @brief		Function converts n-bit binary number into integer and returns it.
* @param	char binary_string[] -> binary number in string format.
* @param	int num_of_bits		  -> number of bits by which the binary number is represented.
* @return	Returns the integer value of the binary number.
*/
static int BinToDec(char binary_string[], int num_of_bits);

/** 
* @brief		Function parses user-input string and does one of two functionalities:\n\n
*					1) Function converts a string containing binary numbers in the format "0bxxxxxxxx;0byyyyyyyy;0bzzzzzzzz...".\n
*                   into integers and stores them inside a global array called temp_values[]. \n
*                   The number of converted characters is stored inside a global variable called temp_value_cnt;\n\n
*
*                   2) If the format is "num=x" where x is a number between 1 and 16, function parses that number as an integer\n
*                   and sets a global variabled called read_count to that number.                  
* @param	char p_input_str[]  -> string which needs to be parsed.
* @return	Returns OK if parsing was successful or ERROR if the format is invalid.
*/
static int ParseInput(char p_input_str[]);

int OpenFifo(struct inode *pinode, struct file *pfile);
int CloseFifo(struct inode *pinode, struct file *pfile);
ssize_t ReadFifo(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t WriteFifo(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);


static int end_read = 0;										///< Indicates whether ReadFifo should stop reading or not.
static size_t read_count = 1;								///< Indicates how many values should be read from FIFO buffer.

static unsigned char read_pos    = 0u;				///< Current read position of FIFO buffer.
static unsigned char write_pos   = 0u;				///< Current write position of FIFO buffer.
static unsigned char element_cnt = 0u;			///< Number of elements currently inside FIFO buffer.

static int temp_values[BUFF_SIZE] = { 0 };		///< Temporary buffer to store integer values after parsing them from user-input but before storing them inside FIFO buffer.
static int temp_value_cnt = 0u;							///< Number of values inside temp_values[] buffer ie. number of values to write into FIFO buffer.

static unsigned char fifo_buffer[BUFF_SIZE] = { 0 };

static struct semaphore sem;

static wait_queue_head_t read_queue;				///< Wait queue for processes trying to read from empty FIFO.
static wait_queue_head_t write_queue;				///< Wait queue for processes trying to write into full FIFO.

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
	int num_of_reads;
	
	char temp_buff[MAX_STR_SIZE] = { 0 };
	long int len = 0;
	
	// cat fifo_module will try to read from file as long as the return value is not 0 so we return 0 (OK) after reading once.
	if (end_read)
	{
		end_read = 0;
		return OK;
	}

	// Loop to read multiple elements from FIFO 
	for (num_of_reads = 0; num_of_reads < read_count; num_of_reads++)
	{
		if(down_interruptible(&sem)) return -ERESTARTSYS;
	
		// FIFO is empty
		while(element_cnt == 0)
		{
			up(&sem);	
		
			// Put process in read queue
			if(wait_event_interruptible(read_queue,(element_cnt > 0))) return -ERESTARTSYS;
		
			if(down_interruptible(&sem)) return -ERESTARTSYS;
		}
	
		/* Check if FIFO is empty once again since multiple processes could have been in the read queue and 
		*  they will all be released from the queue at the same time as soon as there is at least one element in it but
		*  only one of them can read that element. 
		*/
		if(element_cnt > 0)
		{
			// Read from FIFO and convert to string
			len = scnprintf(temp_buff, strlen(temp_buff), "%d ", fifo_buffer[read_pos]);
			ret = copy_to_user(buffer, temp_buff, len);
		
			if(ret) return -EFAULT;
		
			printk(KERN_INFO "Succesfully read %d from FIFO buffer.\n", fifo_buffer[read_pos]);
		
			// Update read position in a circular manner
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
		// One (or more) element has been read, write queue can be released
		wake_up_interruptible(&write_queue);	
	}

	end_read = 1;
	
	return len;
}

ssize_t WriteFifo(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset)
{
	char temp_buff[MAX_STR_SIZE] = { 0 };

	int current_temp_value;
	int ret;
	
	ret = copy_from_user(temp_buff, buffer, length);
	
	if(ret) return -EFAULT;
	
	temp_buff[length-1] = '\0';
	
	ret = ParseInput(temp_buff);
	
	if (ret) return -EFAULT;
	
	// Loop to write all binary numbers the user provided
	for (current_temp_value = 0; current_temp_value < temp_value_cnt; current_temp_value++)
	{
		if(down_interruptible(&sem)) return -ERESTARTSYS;

		// FIFO full
		while(element_cnt == BUFF_SIZE)
		{
			up(&sem);
			// Put process in write queue
			if(wait_event_interruptible(write_queue,(element_cnt < BUFF_SIZE))) return -ERESTARTSYS;
			if(down_interruptible(&sem)) return -ERESTARTSYS;
		}
		
		// Multiple processes can be in write queue and will all be released at once so
		// an additional check is necessary since only one of them can write 
		if(element_cnt < BUFF_SIZE)
		{
			fifo_buffer[write_pos] = temp_values[current_temp_value];
			printk(KERN_INFO "Succesfully wrote value %d.", temp_values[current_temp_value]);
			
			// Update write position in circular manner
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
		// One (or more) element has been written, read queue can be released
		wake_up_interruptible(&read_queue);
	}
	
	return length;
}

static int __init FifoInit(void)
{
	int ret;
	
    sema_init(&sem, 1);
	init_waitqueue_head(&write_queue);
	init_waitqueue_head(&read_queue);
	
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

static int BinToDec(char binary_string[], int num_of_bits) 
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

static int ParseInput(char p_input_str[])
{
    int input_len;
	int temp_read_count;
	int input_char_cnt;
    int value;
	int ret;
	
	char temp_bin[BIN_FORMAT_SIZE] = { 0 };
	
	input_len = strlen(p_input_str);
	
	// Reset number of temporary values extracted from user input
	temp_value_cnt = 0u; 
	
	// Check if user requested to update read count
	strncpy(temp_bin, p_input_str, READ_CHANGE_FORMAT_SIZE);
	
	ret = sscanf(temp_bin, "num=%d", &temp_read_count);
	
    if (ret)
	{
		read_count = (size_t) temp_read_count;
		printk(KERN_INFO "Read count changed to %lu.\n", read_count);
		return OK;
	} else if ( (temp_bin[0] == 'n') && (temp_bin[1] == 'u') && (temp_bin[2] == 'm') && (temp_bin[3] == '=') )
	{
        printk(KERN_WARNING "Invalid format. Read count must be 0-9. \n");
		return ERROR;
	}
	
	// Scan every character from user input until we reach ';' or '\0'
    for (input_char_cnt = 0; input_char_cnt <= input_len; input_char_cnt++)
    {
        if (p_input_str[input_char_cnt] == ';' || p_input_str[input_char_cnt] == '\0')
        {   
			// Check if user input format is correct
            if ( (input_char_cnt - BIN_FORMAT_SIZE - 2) >= 0 )
            {	//                     0b10110111                                                     0b10110111
				//                     ^ check                                                         ^ check
                if ( (p_input_str[(input_char_cnt - (int)BIN_FORMAT_SIZE - 2)] == '0') && (p_input_str[input_char_cnt - (int)BIN_FORMAT_SIZE - 1] == 'b') )
                {
					// Extract just the bits of a binary number
                    strncpy(temp_bin, &p_input_str[input_char_cnt - BIN_FORMAT_SIZE], BIN_FORMAT_SIZE);
                    value = BinToDec(temp_bin, BIN_FORMAT_SIZE);
					
                    if (value == ERROR)
                    {
						printk(KERN_WARNING "Invalid format. Format is: 0bxxxxxxxx. Each x must be '0' or '1'.\n");
                    } else
                    {
						// Value successfully converted, place it in a temporary buffer and increment temp_value_cnt
                        temp_values[temp_value_cnt] = value;
						temp_value_cnt++;
                    }
                } else
                {
                    printk(KERN_WARNING "Invalid format. Format is: 0bxxxxxxxx. Each x must be '0' or '1'.\n");
                }   
            } else 
            {
                printk(KERN_WARNING "Invalid format. Input too short.\n");
				return ERROR;
            }

			// Check if end of user input is reached
            if (p_input_str[input_char_cnt] == '\0')
            {
                break;
            }
        }
    }
	
	return OK;
}

module_init(FifoInit);
module_exit(FifoExit);
