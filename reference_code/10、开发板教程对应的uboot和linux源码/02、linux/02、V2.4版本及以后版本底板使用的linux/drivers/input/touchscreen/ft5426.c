#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/string.h>
#include <linux/irq.h>
#include <asm/mach/map.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>

#define MAX_SUPPORT_POINTS		5			/* 5点触摸 */
#define TOUCH_EVENT_DOWN		0x00		/* 按下 */
#define TOUCH_EVENT_UP			0x01		/* 抬起 */
#define TOUCH_EVENT_ON			0x02		/* 接触 */
#define TOUCH_EVENT_RESERVED	0x03		/* 保留 */

/* FT5X06寄存器相关宏定义 */
#define FT5X06_TD_STATUS_REG	0X02		/*	状态寄存器地址 */
#define FT5x06_DEVICE_MODE_REG	0X00 		/* 模式寄存器 		*/
#define FT5426_IDG_MODE_REG		0XA4		/* 中断模式				*/
#define FT5X06_READLEN			29			/* 要读取的寄存器个数 */

struct ft5x06_dev {
    struct device_node *nd;
    int irq_pin,reset_pin;
    int irq_num;
    void *private_data;
    struct i2c_client *client;
    struct input_dev *input;
};

static struct ft5x06_dev ft5x06;

/* 读取FT5426的N个寄存器值 */
static int ft5x06_read_regs(struct ft5x06_dev *dev, u8 reg, void *val, int len)
{

    struct i2c_msg msg[2];
    struct i2c_client *client = (struct i2c_client*)dev->client;

    /*  msg[0]发送要读取的寄存器首地址 */
    msg[0].addr  = client->addr; /* 从机地址，也就是FT5X06地址 */
    msg[0].flags = 0;           /* 表示为要发送的数据 */
    msg[0].buf = &reg;          /* 要发送的数据，也就是寄存器地址 */
    msg[0].len = 1;             /* 要发送的寄存器地址长度为1 */

    /* msg[1]读取数据 */
    msg[1].addr  = client->addr; /* 从机地址，也就是FT5X06地址 */
    msg[1].flags = I2C_M_RD;     /* 表示读数据 */
    msg[1].buf = val;           /* 接收到的从机发送的数据 */
    msg[1].len = len;             /* 要读取的寄存器长度 */

	return i2c_transfer(client->adapter, msg, 2);
}

/* 向FT5X06写N个寄存器的数据 */
static int ft5x06_write_regs(struct ft5x06_dev *dev, u8 reg, u8 *buf, u8 len)
{
    u8 b[256];
    struct i2c_msg msg;
    struct i2c_client *client = (struct i2c_client*)dev->client;
    
    /* 构建要发送的数据，也就是寄存器首地址+实际的数据 */
    b[0] = reg;
    memcpy(&b[1], buf, len);

    msg.addr  = client->addr; /* 从机地址，也就是FT5X06地址 */
    msg.flags = 0;           /* 表示为要发送的数据 */
    msg.buf = b;            /* 要发送的数据，寄存器地址+实际数据 */
    msg.len = len + 1;       /* 要发送的数据长度：寄存器地址长度+实际的数据长度 */

	return i2c_transfer(client->adapter, &msg, 1);
}

/* 向FT5X06一个寄存器写数据 */
static void ft5x06_write_reg(struct ft5x06_dev *dev, u8 reg, u8 data)
{
    u8 buf  = 0;
    buf = data;
    ft5x06_write_regs(dev, reg, &buf, 1);
}

#if 0
/* 读取FT5426一个寄存器 */
static u8 ft5x06_read_reg(struct ft5x06_dev *dev, u8 reg)
{
    u8 data  = 0;
    ft5x06_read_regs(dev, reg, &data, 1);
    return data;
}
#endif

/* ft5x06中断处理函数 */
static irqreturn_t ft5x06_handler(int irq, void *dev_id)
{
    struct ft5x06_dev *multidata = dev_id;
    u8 rdbuf[29];
    int i, type, x, y, id;
	int offset, tplen;
	int ret;
	bool down;

	offset = 1; 	/* 偏移1，也就是0X02+1=0x03,从0X03开始是触摸值 */
	tplen = 6;		/* 一个触摸点有6个寄存器来保存触摸值 */

	memset(rdbuf, 0, sizeof(rdbuf));		/* 清除 */

    /* 从FT5X06芯片读取触摸点信息 */
    ft5x06_read_regs(multidata, FT5X06_TD_STATUS_REG, rdbuf, FT5X06_READLEN);

    for(i = 0; i < MAX_SUPPORT_POINTS; i++) {
        /* 提取出来每个触摸点的坐标，上报TypeB格式 */
		u8 *buf = &rdbuf[i * tplen + offset];  /* 获取每个触摸点原始数据起始地址 */

		/* 以第一个触摸点为例，寄存器TOUCH1_XH(地址0X03),各位描述如下：
		 * bit7:6  Event flag  0:按下 1:释放 2：接触 3：没有事件
		 * bit5:4  保留
		 * bit3:0  X轴触摸点的11~8位。
		 */
		type = buf[0] >> 6;     /* 获取触摸类型 */
		if (type == TOUCH_EVENT_RESERVED)
			continue;
 
		/* 我们所使用的触摸屏和FT5X06是反过来的 */
		x = ((buf[2] << 8) | buf[3]) & 0x0fff;
		y = ((buf[0] << 8) | buf[1]) & 0x0fff;
		
		/* 以第一个触摸点为例，寄存器TOUCH1_YH(地址0X05),各位描述如下：
		 * bit7:4  Touch ID  触摸ID，表示是哪个触摸点
		 * bit3:0  Y轴触摸点的11~8位。
		 */
		id = (buf[2] >> 4) & 0x0f;
		down = type != TOUCH_EVENT_UP;
    
        /* 上报数据 */
        input_mt_slot(multidata->input, id);   /* ABS_MT_SLOT */
		input_mt_report_slot_state(multidata->input, MT_TOOL_FINGER, down);  /*  ABS_MT_TRACKING_ID */
		
        if(!down) {
            continue;
        }

	    input_report_abs(multidata->input, ABS_MT_POSITION_X, x);       /* ABS_MT_POSITION_X */
		input_report_abs(multidata->input, ABS_MT_POSITION_Y, y);       /* ABS_MT_POSITION_Y */
    }

    input_mt_report_pointer_emulation(multidata->input,true);
	input_sync(multidata->input);          /*  SYN_REPORT	 */

    return IRQ_HANDLED;
}

/* 复位FT5x06 */
static int ft5x06_ts_reset(struct i2c_client *client, struct ft5x06_dev *dev)
{
	int ret = 0;

	if (gpio_is_valid(dev->reset_pin)) {  		/* 检查IO是否有效 */
		/* 申请复位IO，并且默认输出低电平 */
		ret = devm_gpio_request_one(&client->dev,	
					dev->reset_pin, GPIOF_OUT_INIT_LOW,
					"edt-ft5x06 reset");
		if (ret) {
			return ret;
		}

		msleep(5);
		gpio_set_value(dev->reset_pin, 1);	/* 输出高电平，停止复位 */
		msleep(300);
	}

	return 0;

}

/* FT5x06中断 */
static int ft5x06_ts_irq(struct i2c_client *client, struct ft5x06_dev *dev)
{
	int ret = 0;

	/* 1,申请中断GPIO */
	if (gpio_is_valid(dev->irq_pin)) {
		ret = devm_gpio_request_one(&client->dev, dev->irq_pin,
					GPIOF_IN, "edt-ft5x06 irq");
		if (ret) {
			dev_err(&client->dev,
				"Failed to request GPIO %d, error %d\n",
				dev->irq_pin, ret);
			return ret;
		}
	}

	/* 2，申请中断,client->irq就是IO中断， */
	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					ft5x06_handler, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					client->name, &ft5x06);
	if (ret) {
		dev_err(&client->dev, "Unable to request touchscreen IRQ.\n");
		return ret;
	}

	return 0;    

}

static int ft5x06_probe(struct i2c_client *client, 
                        const struct i2c_device_id *id)
{
    int ret  = 0;
    printk("ft5x06_probe!\r\n");

    ft5x06.client = client;

    /* 获取irq和reset引脚 */
    ft5x06.irq_pin = of_get_named_gpio(client->dev.of_node, "interrupt-gpios", 0);
    ft5x06.reset_pin = of_get_named_gpio(client->dev.of_node, "reset-gpios", 0);

    ft5x06_ts_reset(client, &ft5x06);
    ft5x06_ts_irq(client, &ft5x06);

    /* 初始化FT5426 */
	ft5x06_write_reg(&ft5x06, FT5x06_DEVICE_MODE_REG, 0); 	/* 进入正常模式 	*/
	ft5x06_write_reg(&ft5x06, FT5426_IDG_MODE_REG, 1); 		/* FT5426中断模式	*/

    /* input框架 */
    ft5x06.input = devm_input_allocate_device(&client->dev);

    ft5x06.input->name = client->name;
    ft5x06.input->id.bustype = BUS_I2C;
    ft5x06.input->dev.parent = &client->dev;

	__set_bit(EV_SYN, ft5x06.input ->evbit);
	__set_bit(EV_KEY, ft5x06.input ->evbit);
	__set_bit(EV_ABS, ft5x06.input ->evbit);
	__set_bit(BTN_TOUCH, ft5x06.input ->keybit);

	/* Single touch */
	input_set_abs_params(ft5x06.input, ABS_X, 0, 1024, 0, 0);
	input_set_abs_params(ft5x06.input, ABS_Y, 0, 600, 0, 0);

	/* Multi touch */
	input_mt_init_slots(ft5x06.input, MAX_SUPPORT_POINTS, 0);
	input_set_abs_params(ft5x06.input, ABS_MT_POSITION_X, 0, 1024, 0, 0);
	input_set_abs_params(ft5x06.input, ABS_MT_POSITION_Y, 0, 600, 0, 0);

    ret = input_register_device(ft5x06.input);

    return ret;
}

static int ft5x06_remove(struct i2c_client *client)
{
    input_unregister_device(ft5x06.input);
    return 0;
}

/* 传统的匹配表 */
static struct i2c_device_id ft5x06_id[] = {
    {"edt-ft5426", 0},
    {}
};

/* 设备树匹配表 */
static struct of_device_id ft5x06_of_match[] = {
    {.compatible = "edt,edt-ft5426",}, 
    {}
};

/* i2c_driver */
static struct i2c_driver ft5x06_driver = {
    .probe = ft5x06_probe,
    .remove = ft5x06_remove,
    .driver = {
        .name = "edt_ft5x06",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(ft5x06_of_match),
    },
    .id_table = ft5x06_id,
};

/* 驱动入口函数 */
static int __init ft5x06_init(void)
{

    int ret = 0;

    ret = i2c_add_driver(&ft5x06_driver);

    return ret;

}

/* 驱动出口函数 */
static void __exit ft5x06_exit(void)
{
    i2c_del_driver(&ft5x06_driver);
}

module_init(ft5x06_init);
module_exit(ft5x06_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zuozhongkai");
