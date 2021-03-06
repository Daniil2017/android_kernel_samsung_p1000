/* 

 * drivers/i2c/chips/akm8973.c - akm8973 compass driver

 *

 * Copyright (C) 2008-2009 HTC Corporation.

 * Author: viral wang <viralwang@gmail.com>

 *

 * This software is licensed under the terms of the GNU General Public

 * License version 2, as published by the Free Software Foundation, and

 * may be copied, distributed, and modified under those terms.

 *

 * This program is distributed in the hope that it will be useful,

 * but WITHOUT ANY WARRANTY; without even the implied warranty of

 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the

 * GNU General Public License for more details.

 *

 */

 

/*

 * Revised by AKM 2010/03/25

 * 

 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/earlysuspend.h>
#include <mach/hardware.h>
#include <plat/gpio-cfg.h>
#include <mach/regs-gpio.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include "ak8973b.h"


#define E_COMPASS_ADDRESS	0x1c	/* CAD0 : 0, CAD1 : 0 */
#define I2C_DF_NOTIFY       0x01
#define IRQ_COMPASS_INT IRQ_EINT(2) /* EINT(2) */

static struct i2c_client *this_client;

struct ak8973b_data {
	struct i2c_client		*client;
	struct input_dev *input_dev;
	struct early_suspend	early_suspend;
};

static DECLARE_WAIT_QUEUE_HEAD(open_wq);

static atomic_t open_count;
static atomic_t open_flag;
static atomic_t reserve_open_flag;

static atomic_t m_flag; 
static atomic_t a_flag; 
static atomic_t t_flag; 
static atomic_t mv_flag;
static atomic_t	p_flag;

static short akmd_delay = 0;

#ifdef CONFIG_HAS_EARLYSUSPEND
static atomic_t suspend_flag = ATOMIC_INIT(0);
#endif /* CONFIG_HAS_EARLYSUSPEND */ 

/* following are the sysfs callback functions */

#define config_ctrl_reg(name,address) \
	static ssize_t name##_show(struct device *dev, struct device_attribute *attr, \
		char *buf) \
{ \
	struct i2c_client *client = to_i2c_client(dev); \
	return sprintf(buf, "%u\n", i2c_smbus_read_byte_data(client,address)); \
} \
static ssize_t name##_store(struct device *dev, struct device_attribute *attr, \
	const char *buf,size_t count) \
{ \
	struct i2c_client *client = to_i2c_client(dev); \
	unsigned long val = simple_strtoul(buf, NULL, 10); \
	if (val > 0xff) \
		return -EINVAL; \
	i2c_smbus_write_byte_data(client,address, val); \
		return count; \
} \
static DEVICE_ATTR(name, S_IWUSR | S_IRUGO, name##_show, name##_store)

config_ctrl_reg(ms1, AKECS_REG_MS1);

static char ak_e2prom_data[3];

static int AKI2C_RxData(char *rxData, int length)
{
	struct i2c_msg msgs[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = rxData,
		 },
		{
		 .addr = this_client->addr,
		 .flags = I2C_M_RD,
		 .len = length,
		 .buf = rxData,
		 },
	};

	if (i2c_transfer(this_client->adapter, msgs, 2) < 0) {
		printk(KERN_ERR "AKI2C_RxData: transfer error \n");
		return -EIO;
	} else
		return 0;
}

static int AKI2C_TxData(char *txData, int length)
{
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = length,
		 .buf = txData,
		 },
	};

	if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
		printk(KERN_ERR "AKI2C_TxData: transfer error addr \n");
		return -EIO;
	} else
		return 0;
}

static int AKECS_Init(void)
{
	return 0;
}

static int akm_aot_open(struct inode *inode, struct file *file)
{
	int ret = -1;
	if (atomic_cmpxchg(&open_count, 0, 1) == 0) {
		if (atomic_cmpxchg(&open_flag, 0, 1) == 0) {
			atomic_set(&reserve_open_flag, 1);
			wake_up(&open_wq);
			ret = 0;
		}
	}
	return ret;
}

static int akm_aot_release(struct inode *inode, struct file *file)
{
	atomic_set(&reserve_open_flag, 0);
	atomic_set(&open_flag, 0);
	atomic_set(&open_count, 0);
	wake_up(&open_wq);
	return 0;
}

static void AKECS_Report_Value(short *rbuf)
{
	struct ak8973b_data *data = i2c_get_clientdata(this_client);
	#if 0
	gprintk("Orientaion: yaw = %d, pitch = %d, roll = %d\n", rbuf[0],
			rbuf[1], rbuf[2]);
	gprintk("tmp = %d, m_stat= %d, g_stat=%d\n", rbuf[3],
			rbuf[4], rbuf[5]);
	gprintk("Acceleration:   x = %d LSB, y = %d LSB, z = %d LSB\n",
			rbuf[6], rbuf[7], rbuf[8]);
	gprintk("Magnetic:   x = %d LSB, y = %d LSB, z = %d LSB\n\n",
			rbuf[9], rbuf[10], rbuf[11]);
	#endif
	/*if flag is set, execute report */
	/* Report magnetic sensor information */
	if (atomic_read(&m_flag)) {		
		input_report_abs(data->input_dev, ABS_RX, rbuf[0]);
		input_report_abs(data->input_dev, ABS_RY, rbuf[1]);
		input_report_abs(data->input_dev, ABS_RZ, rbuf[2]);
		input_report_abs(data->input_dev, ABS_RUDDER, rbuf[4]);
	}

	/* Report acceleration sensor information */
	if (atomic_read(&a_flag)) {		
		input_report_abs(data->input_dev, ABS_X, rbuf[6]);
		input_report_abs(data->input_dev, ABS_Y, rbuf[7]);
		input_report_abs(data->input_dev, ABS_Z, rbuf[8]);
		input_report_abs(data->input_dev, ABS_WHEEL, rbuf[5]);
	}

	/* Report temperature information */
	if (atomic_read(&t_flag)) {		
		input_report_abs(data->input_dev, ABS_THROTTLE, rbuf[3]);
	}

	if (atomic_read(&mv_flag)) {		
		input_report_abs(data->input_dev, ABS_HAT0X, rbuf[9]);
		input_report_abs(data->input_dev, ABS_HAT0Y, rbuf[10]);
		input_report_abs(data->input_dev, ABS_BRAKE, rbuf[11]);
	}
#if 0
	/* Report proximity information */
	if (atomic_read(&p_flag)) {
		rbuf[12]=gp2a_get_proximity_value();
		input_report_abs(data->input_dev, ABS_DISTANCE, rbuf[12]);
		gprintk("Proximity = %d\n", rbuf[12]);
	}
#endif	
	input_sync(data->input_dev);
}

static int AKECS_GetOpenStatus(void)
{
	wait_event_interruptible(open_wq, (atomic_read(&open_flag) != 0));
	return atomic_read(&open_flag);
}

static int AKECS_GetCloseStatus(void)
{
	wait_event_interruptible(open_wq, (atomic_read(&open_flag) <= 0));
	return atomic_read(&open_flag);
}

static void AKECS_CloseDone(void)
{
	atomic_set(&m_flag, 1);
	atomic_set(&a_flag, 1);
	atomic_set(&t_flag, 1);
	atomic_set(&mv_flag, 1);
	atomic_set(&p_flag, 1);
}



/*----------------------------------------------------------------------------*/
//Description : Resetting AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static void AKECS_Reset (void)
{
      
	gpio_set_value(GPIO_MSENSE_nRST, GPIO_LEVEL_LOW);
	udelay(120);
	gpio_set_value(GPIO_MSENSE_nRST, GPIO_LEVEL_HIGH);
	gprintk("AKECS RESET COMPLETE\n");
}

/*----------------------------------------------------------------------------*/
//Description : Setting measurement to AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static int AKECS_SetMeasure (void)
{
	char buffer[2];

	gprintk("MEASURE MODE\n");
	/* Set measure mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_MEASURE;

	/* Set data */
	return AKI2C_TxData(buffer, 2);
}

/*----------------------------------------------------------------------------*/
//Description : Setting EEPROM to AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static int AKECS_SetE2PRead ( void )
{
	char buffer[2];

	/* Set measure mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_E2P_READ;
	/* Set data */
	return AKI2C_TxData(buffer, 2);
}


/*----------------------------------------------------------------------------*/
//Description : Power Down to AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static int AKECS_PowerDown (void)
{
	char buffer[2];
	int ret;

	/* Set powerdown mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_POWERDOWN;
	/* Set data */
	ret = AKI2C_TxData(buffer, 2);
	if (ret < 0)
		return ret;

	/* Dummy read for clearing INT pin */
	buffer[0] = AKECS_REG_TMPS;
	/* Read data */
	ret = AKI2C_RxData(buffer, 1);
	if (ret < 0)
		return ret;

 return ret;
}

/*----------------------------------------------------------------------------*/
//Description : Get EEPROM Data to AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static int AKECS_GetEEPROMData (void)
{ 
	int ret;
	char buffer[RBUFF_SIZE + 1];

	ret = AKECS_SetE2PRead();
	if (ret < 0) return ret;

	memset(buffer, 0, RBUFF_SIZE + 1);
	buffer[0] = AKECS_EEP_EHXGA;
	ret =  AKI2C_RxData(buffer, 3);

	if (ret < 0) return ret;

	ak_e2prom_data[0]= buffer[0];
	ak_e2prom_data[1]= buffer[1];
	ak_e2prom_data[2]= buffer[2];
	gprintk("AKE2PROM_Data -->%d , %d, %d ----\n", ak_e2prom_data[0],ak_e2prom_data[1],ak_e2prom_data[2]);
	
	return ret;
}

static int AKECS_SetMode(char mode)
{
	int ret = 0;	
	
	switch (mode) {
		case AKECS_MODE_MEASURE:
			ret = AKECS_SetMeasure();
			break;
		case AKECS_MODE_E2P_READ:
			ret = AKECS_SetE2PRead();
			break;
		case AKECS_MODE_POWERDOWN:
			ret = AKECS_PowerDown();
			break;
		default:
			return -EINVAL;
	}

	/* wait at least 300us after changing mode */
	msleep(1);
	return ret;
}

static int AKECS_TransRBuff(char *rbuf, int size)
{
	if(size < RBUFF_SIZE + 1)
		return -EINVAL;

	// read C0 - C4
	rbuf[0] = AKECS_REG_ST;
	return AKI2C_RxData(rbuf, RBUFF_SIZE + 1);

}


/*----------------------------------------------------------------------------*/
//Description : Get EEPROM Data to AKECS 
//WHO : AKEK GWLEE
//DATE : 2008 12 08
/*----------------------------------------------------------------------------*/
static int AKECS_GetData (short *rbuf)
{ 
	int ret;
  	char buffer[RBUFF_SIZE + 1];

	memset(buffer, 0, RBUFF_SIZE + 1);
	buffer[0] = AKECS_REG_TMPS;

	ret =  AKI2C_RxData(buffer, 4);  //temp, x, y, z
	if(ret<0) {
		printk(KERN_ERR "AKECS_GetData  failed--------------\n");
		return ret;
		}
	else	{
			
			rbuf[0] = 35 + (120 -buffer[0])*10/16;
			rbuf[1] = buffer[1];
			rbuf[2] = buffer[2];
			rbuf[3] = buffer[3];
			printk("AKECS_Get_MAG: temp = %d, x = %d, y = %d, z = %d\n", rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
		}
  return ret;
}

static void AKECS_DATA_Measure(void)
{
	short  value[12];
	short mag_sensor[4] ={0, 0, 0, 0};

	AKECS_GetData(mag_sensor);
	value[0]=0;
	value[1]=0;
	value[2]=0;
	value[3]=mag_sensor[0];		/* temparature */
	value[4]=0;
	value[5]=0;
	value[6]=0;
	value[7]=0;
	value[8]=0;
	value[9]=mag_sensor[1];		/* mag_x */
	value[10]=mag_sensor[2];	/* mag_y */
	value[11]=mag_sensor[3];	/* mag_z */

	AKECS_Report_Value(value);
}


static int 
akm_aot_ioctl(struct inode *inode, struct file *file,
	      unsigned int cmd, unsigned long arg)
{
#if 0
	struct ak8973b_data *akm = i2c_get_clientdata(this_client);

	switch(cmd)
	{
		case EMCS_IOCTL_OPEN:
			gprintk("hrtimer_start!\n");
			ak8973b_timer_oper = TIMER_ON;
			hrtimer_start(&akm->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
			break;
		case EMCS_IOCTL_CLOSE:
			gprintk("hrtimer_cancel!\n");
			ak8973b_timer_oper = TIMER_OFF;
			hrtimer_cancel(&akm->timer);
			break;
		default:
			break;
	}
#endif

	void __user *argp = (void __user *)arg;
	short flag;

	switch (cmd) {
		case ECS_IOCTL_APP_SET_MFLAG:
		case ECS_IOCTL_APP_SET_AFLAG:
		case ECS_IOCTL_APP_SET_TFLAG:
		case ECS_IOCTL_APP_SET_MVFLAG:
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
			if (flag < 0 || flag > 1)
				return -EINVAL;
			break;
		case ECS_IOCTL_APP_SET_DELAY:
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
			break;
		default:
			break;
	}

	switch (cmd) {
		case ECS_IOCTL_APP_SET_MFLAG:
			atomic_set(&m_flag, flag);			
			break;
		case ECS_IOCTL_APP_GET_MFLAG:
			flag = atomic_read(&m_flag);
			break;
		case ECS_IOCTL_APP_SET_AFLAG:
			atomic_set(&a_flag, flag);			
			break;
		case ECS_IOCTL_APP_GET_AFLAG:
			flag = atomic_read(&a_flag);
			break;
		case ECS_IOCTL_APP_SET_TFLAG:
			atomic_set(&t_flag, flag);			
			break;
		case ECS_IOCTL_APP_GET_TFLAG:
			flag = atomic_read(&t_flag);
			break;
		case ECS_IOCTL_APP_SET_MVFLAG:
			atomic_set(&mv_flag, flag);			
			break;
		case ECS_IOCTL_APP_GET_MVFLAG:
			flag = atomic_read(&mv_flag);
			break;
		case ECS_IOCTL_APP_SET_PFLAG:
			atomic_set(&p_flag, flag);
			break;
		case ECS_IOCTL_APP_GET_PFLAG:
			flag = atomic_read(&p_flag);
			break;
		case ECS_IOCTL_APP_SET_DELAY:
			akmd_delay = flag;
			break;
		case ECS_IOCTL_APP_GET_DELAY:
			flag = akmd_delay;
			break;
		case ECS_IOCTL_DEVMGR_GET_DATA:
			AKECS_DATA_Measure();
			break;
		default:
			return -ENOTTY;
	}

	switch (cmd) {
		case ECS_IOCTL_APP_GET_MFLAG:
		case ECS_IOCTL_APP_GET_AFLAG:
		case ECS_IOCTL_APP_GET_TFLAG:
		case ECS_IOCTL_APP_GET_MVFLAG:
		case ECS_IOCTL_APP_GET_DELAY:
			if (copy_to_user(argp, &flag, sizeof(flag)))
				return -EFAULT;
			break;
		default:
			break;
	}

	return 0;
}

static int akmd_open(struct inode *inode, struct file *file)
{
	gprintk(KERN_INFO "[AK8973B] %s\n", __FUNCTION__);
	return nonseekable_open(inode, file);
}

static int akmd_release(struct inode *inode, struct file *file)
{
	gprintk(KERN_INFO "[AK8973B] %s\n", __FUNCTION__);
	AKECS_CloseDone();
	return 0;
}

	static int
akmd_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int i;
	void __user *argp = (void __user *)arg;

	char msg[RBUFF_SIZE + 1], rwbuf[16];//, numfrq[2];
	int ret = -1, status;
	short mode, value[12], delay; /* step_count,*/
	//  char *pbuffer = 0;
	gprintk("start\n");

	switch (cmd) {
		case ECS_IOCTL_READ:
		case ECS_IOCTL_WRITE:
			if (copy_from_user(&rwbuf, argp, sizeof(rwbuf)))
				return -EFAULT;
			break;
		case ECS_IOCTL_SET_MODE:
			if (copy_from_user(&mode, argp, sizeof(mode)))
				return -EFAULT;
			break;
		case ECS_IOCTL_SET_YPR:
			if (copy_from_user(&value, argp, sizeof(value)))
				return -EFAULT;
			break;
			//case ECS_IOCTL_SET_STEP_CNT:
			// if (copy_from_user(&step_count, argp, sizeof(step_count)))
			//     return -EFAULT;
			// break;
		default:
			break;
	}

	switch (cmd) {
		case ECS_IOCTL_INIT:
			gprintk("[AK8973B] ECS_IOCTL_INIT %x\n", cmd);
			ret = AKECS_Init();
			if (ret < 0)
				return ret;
			break;
		case ECS_IOCTL_RESET:
			gprintk("[AK8973B] ECS_IOCTL_RESET %x\n", cmd);
			AKECS_Reset();
			break;
		case ECS_IOCTL_READ:
			gprintk("[AK8973B] ECS_IOCTL_READ %x\n", cmd);
			gprintk(" len %02x:", rwbuf[0]);
			gprintk(" addr %02x:", rwbuf[1]);
			gprintk("\n");
			if (rwbuf[0] < 1)
				return -EINVAL;
			ret = AKI2C_RxData(&rwbuf[1], rwbuf[0]);
			//for(i=0; i<rwbuf[0]; i++){
			//	printk(" %02x", rwbuf[i+1]);
			//}
			gprintk(" ret = %d\n", ret);
			if (ret < 0)
				return ret;
			break;
		case ECS_IOCTL_WRITE:
			gprintk("[AK8973B] ECS_IOCTL_WRITE %x\n", cmd);
			gprintk(" len %02x:", rwbuf[0]);
			for(i=0; i<rwbuf[0]; i++){
				gprintk(" %02x", rwbuf[i+1]);
			}
			gprintk("\n");
			if (rwbuf[0] < 2)
				return -EINVAL;
			ret = AKI2C_TxData(&rwbuf[1], rwbuf[0]);
			gprintk(" ret = %d\n", ret);
			if (ret < 0)
				return ret;
			break;
		case ECS_IOCTL_SET_MODE:
			gprintk("[AK8973B] ECS_IOCTL_SET_MODE %x mode=%x\n", cmd, mode);
			ret = AKECS_SetMode((char)mode);
			gprintk(" ret = %d\n", ret);
			if (ret < 0)
				return ret;
			break;
		case ECS_IOCTL_GETDATA:
			gprintk("[AK8973B] ECS_IOCTL_GETDATA %x\n", cmd);
			ret = AKECS_TransRBuff(msg, RBUFF_SIZE+1);
			gprintk(" ret = %d\n", ret);
			if (ret < 0)
				return ret;
			for(i=0; i<ret; i++){
				gprintk(" %02x", msg[i]);
			}
			gprintk("\n");
			break;
		case ECS_IOCTL_SET_YPR:
			gprintk("[AK8973B] ECS_IOCTL_SET_YPR %x ypr=%x\n", cmd, (unsigned int)value);
			AKECS_Report_Value(value);
			break;
		case ECS_IOCTL_GET_OPEN_STATUS:
			gprintk("[AK8973B] ECS_IOCTL_GET_OPEN_STATUS %x start\n", cmd);
			status = AKECS_GetOpenStatus();
			printk("[AK8973B] ECS_IOCTL_GET_OPEN_STATUS %x end status=%x\n", cmd, status);

			break;
		case ECS_IOCTL_GET_CLOSE_STATUS:
			gprintk("[AK8973B] ECS_IOCTL_GET_CLOSE_STATUS %x start\n", cmd);
			status = AKECS_GetCloseStatus();
			gprintk("[AK8973B] ECS_IOCTL_GET_CLOSE_STATUS %x end status=%x\n", cmd, status);
			break;
		case ECS_IOCTL_GET_DELAY:
			delay = akmd_delay;
			gprintk("[AK8973B] ECS_IOCTL_GET_DELAY %x delay=%x\n", cmd, delay);
			break;
		default:
			gprintk("Unknown cmd %x\n", cmd);
			return -ENOTTY;
	}

	switch (cmd) {
		case ECS_IOCTL_READ:
			if (copy_to_user(argp, &rwbuf, sizeof(rwbuf)))
				return -EFAULT;
			break;
		case ECS_IOCTL_GETDATA:
			if (copy_to_user(argp, &msg, sizeof(msg)))
				return -EFAULT;
			break;
		case ECS_IOCTL_GET_OPEN_STATUS:
		case ECS_IOCTL_GET_CLOSE_STATUS:
			if (copy_to_user(argp, &status, sizeof(status)))
				return -EFAULT;
			break;

		case ECS_IOCTL_GET_DELAY:
			if (copy_to_user(argp, &delay, sizeof(delay)))
				return -EFAULT;
			break;
		default:
			break;
	}

	return 0;
}




//#ifdef CONFIG_HAS_EARLYSUSPEND
#if 0
static void ak8973b_early_suspend(struct early_suspend *handler)
{	
	atomic_set(&suspend_flag, 1);
	if (atomic_read(&open_flag) == 2)
	{
		printk("[%s] Set Mode AKECS_MODE_POWERDOWN\n", __func__);
		AKECS_SetMode(AKECS_MODE_POWERDOWN);		
	}

	atomic_set(&reserve_open_flag, atomic_read(&open_flag));
	atomic_set(&open_flag, 0);
	wake_up(&open_wq);
}

static void ak8973b_early_resume(struct early_suspend *handler)
{	
	atomic_set(&suspend_flag, 0);
	atomic_set(&open_flag, atomic_read(&reserve_open_flag));
	wake_up(&open_wq);
}
#endif /* CONFIG_HAS_EARLYSUSPEND */ 

static void ak8973b_init_hw(void)
{
#if 0	
	s3c_gpio_cfgpin(GPIO_MSENSE_INT, S3C_GPIO_SFN(GPIO_MSENSE_INT_AF));
	s3c_gpio_setpull(GPIO_MSENSE_INT, S3C_GPIO_PULL_NONE);
	set_irq_type(IRQ_COMPASS_INT, IRQ_TYPE_EDGE_RISING);
#endif

	if(gpio_is_valid(GPIO_MSENSE_nRST)){
		if(gpio_request(GPIO_MSENSE_nRST, "GPB"))
		{
			printk(KERN_ERR "Failed to request GPIO_MSENSE_nRST!\n");
		}
		gpio_direction_output(GPIO_MSENSE_nRST, GPIO_LEVEL_LOW);
	}

//	s3c_gpio_setpull(GPIO_MSENSE_nRST, S3C_GPIO_PULL_NONE);
/*VNVS: 14-MAY'10 Pulling the RST Pin HIGH instead of keeping in floating state(Pulling NONE)*/
	s3c_gpio_setpull(GPIO_MSENSE_nRST, S3C_GPIO_PULL_NONE);

	gprintk("gpio setting complete!\n");
}

static struct file_operations akmd_fops = {
	.owner = THIS_MODULE,
	.open = akmd_open,
	.release = akmd_release,
	.ioctl = akmd_ioctl,
};

static struct file_operations akm_aot_fops = {
	.owner = THIS_MODULE,
	.open = akm_aot_open,
	.release = akm_aot_release,
	.ioctl = akm_aot_ioctl,
};

static struct miscdevice akm_aot_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "akm8973_aot",
	.fops = &akm_aot_fops,
};

static struct miscdevice akmd_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "akm8973_daemon",
	.fops = &akmd_fops,
};



//TEST
static ssize_t ak_fs_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	short mag_sensor[4] ={0, 0, 0, 0};

	s3c_gpio_cfgpin(GPIO_MSENSE_IRQ, GPIO_INPUT);
	s3c_gpio_setpull(GPIO_MSENSE_IRQ, S3C_GPIO_PULL_NONE);

	AKECS_SetMeasure();
	while(!gpio_get_value(GPIO_MSENSE_IRQ));
	AKECS_GetData(mag_sensor);
	AKECS_DATA_Measure();
	count = sprintf(buf,"x: %d, y: %d, z: %d\n", mag_sensor[1], mag_sensor[2], mag_sensor[3] );
	
	printk("ak_fs_read: success full\n");

	return count;
}

static ssize_t ak_fs_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	//buf[size]=0;
	printk("input data --> %s\n", buf);

	return size;
}
static DEVICE_ATTR(ak_file, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH, ak_fs_read, ak_fs_write);
//TEST

static int ak8973_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err = 0;
	struct ak8973b_data *akm;
	struct device *dev = &client->dev;

	gprintk("start\n");
	printk("[%s] ak8973 started...\n",__func__);

	akm = kzalloc(sizeof(struct ak8973b_data), GFP_KERNEL);
	if (!akm) {
		err = -ENOMEM;
		goto exit_alloc_data_failed;
	}

    this_client = client;
    i2c_set_clientdata(client, akm);

	if(this_client == NULL)
	{
		gprintk("i2c_client is NULL\n");
		return -ENODEV;
	}
	
	akm->input_dev = input_allocate_device();

	if (!akm->input_dev) {
		err = -ENOMEM;
		gprintk("input_allocate_device failed\n");
		goto exit_input_dev_alloc_failed;
	}
	
	set_bit(EV_ABS, akm->input_dev->evbit);
	set_bit(EV_SYN, akm->input_dev->evbit);

	/* yaw */
	input_set_abs_params(akm->input_dev, ABS_RX, 0, 23040, 0, 0);
	/* pitch */
	input_set_abs_params(akm->input_dev, ABS_RY, -11520, 11520, 0, 0);
	/* roll */
	input_set_abs_params(akm->input_dev, ABS_RZ, -5760, 5760, 0, 0);
	/* x-axis acceleration */
	input_set_abs_params(akm->input_dev, ABS_X, -5760, 5760, 0, 0);
	/* y-axis acceleration */
	input_set_abs_params(akm->input_dev, ABS_Y, -5760, 5760, 0, 0);
	/* z-axis acceleration */
	input_set_abs_params(akm->input_dev, ABS_Z, -5760, 5760, 0, 0);
	/* temparature */
	input_set_abs_params(akm->input_dev, ABS_THROTTLE, -30, 85, 0, 0);
	/* status of magnetic sensor */
	input_set_abs_params(akm->input_dev, ABS_RUDDER, 0, 3, 0, 0);
	/* status of acceleration sensor */
	input_set_abs_params(akm->input_dev, ABS_WHEEL, 0, 3, 0, 0);
	/* x-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_HAT0X, -2048, 2032, 0, 0);
	/* y-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_HAT0Y, -2048, 2032, 0, 0);
	/* z-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_BRAKE, -2048, 2032, 0, 0);
	
	akm->input_dev->name = "compass";

	err = input_register_device(akm->input_dev);
	if (err) {
		gprintk("Unable to register input device: %s\n",akm->input_dev->name);
		goto exit_input_register_device_failed;
	}

	err = misc_register(&akmd_device);
	if (err) {
		printk(KERN_ERR "akm8973_probe: akmd_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	err = misc_register(&akm_aot_device);
	if (err) {
		gprintk("akm_aot_device register failed\n");
		goto exit_misc_device_register_failed;
	}
	//TEST
	if (device_create_file(akm_aot_device.this_device, &dev_attr_ak_file) < 0)
		pr_err("Failed to create device file(%s)!\n", dev_attr_ak_file.attr.name);
	
	if (err) {
		gprintk("device_create_file failed %d\n", err);
	}

	AKECS_GetEEPROMData();

//#ifdef CONFIG_HAS_EARLYSUSPEND
#if 0
	akm->early_suspend.suspend = ak8973b_early_suspend;
	akm->early_suspend.resume = ak8973b_early_resume;
	register_early_suspend(&akm->early_suspend);
#endif /* CONFIG_HAS_EARLYSUSPEND */ 

	return 0;

exit_misc_device_register_failed:
exit_input_register_device_failed:
	input_free_device(akm->input_dev);
exit_input_dev_alloc_failed:
	kfree(akm);
exit_alloc_data_failed:
	return err;
}

static int __exit ak8973_remove(struct i2c_client *client)
{
	// int err;
	struct ak8973b_data *akm = i2c_get_clientdata(client);

	printk("[%s] ak8973 removed...\n",__func__);
	
	input_unregister_device(akm->input_dev);

	misc_deregister(&akm_aot_device);
	misc_deregister(&akmd_device);
	
	kfree(akm);
	kfree(client); 
	akm = NULL;	
	this_client = NULL;
	
	gpio_free(GPIO_MSENSE_nRST);
	
	gprintk("end\n");
	return 0;
}

static int ak8973_suspend( struct platform_device* pdev, pm_message_t state )
{
	printk("############## %s \n",__func__);	
	atomic_set(&suspend_flag, 1);
	printk("[%s] Set Mode AKECS_MODE_POWERDOWN\n", __func__);
	AKECS_SetMode(AKECS_MODE_POWERDOWN);			

	atomic_set(&reserve_open_flag, atomic_read(&open_flag));
	atomic_set(&open_flag, 0);	
	return 0;
}


static int ak8973_resume( struct platform_device* pdev )
{
	printk("@@@@ %s \n",__func__); 
	atomic_set(&suspend_flag, 0);
	
	printk("[%s] Set Mode AKECS_MODE_MEASURE\n", __func__);
	AKECS_SetMode(AKECS_MODE_MEASURE);	
	
	atomic_set(&open_flag, atomic_read(&reserve_open_flag));
	wake_up(&open_wq);
	return 0;
}

static const struct i2c_device_id ak8973_id[] = {
	{"ak8973", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ak8973_id);

static struct i2c_driver ak8973b_i2c_driver = {
	.driver = {
		   .name = "ak8973",
		   },
	.probe = ak8973_probe,
	.remove = __exit_p(ak8973_remove),
	.suspend = ak8973_suspend,
	.resume = ak8973_resume,
	.id_table = ak8973_id,
};



static int __init ak8973b_init(void)
{
	gprintk("__start\n");
	ak8973b_init_hw();
	AKECS_Reset(); /*module reset */
	udelay(200);

	return i2c_add_driver(&ak8973b_i2c_driver);
}

static void __exit ak8973b_exit(void)
{
	gprintk("__exit\n");
	i2c_del_driver(&ak8973b_i2c_driver);
}

module_init(ak8973b_init);
module_exit(ak8973b_exit);

MODULE_AUTHOR("Asahi Kasei Microdevices");
MODULE_DESCRIPTION("AK8973B compass driver");
MODULE_LICENSE("GPL");
