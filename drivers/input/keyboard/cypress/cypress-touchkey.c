/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2012 sakuramilk <c.sakuramilk@gmail.com>
 * Copyright 2005 Phil Blundell
 * Modified by DvTonder
 * Full BLN compatibility and breathing effect by Fluxi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <mach/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <asm/gpio.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/earlysuspend.h>
#include <asm/io.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include "c1-cypress-gpio.h"

#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <plat/gpio-cfg.h>
#include <mach/gpio.h>

extern int ISSP_main(void);
extern int get_tsp_status(void);

#ifdef CONFIG_GENERIC_BLN
#include <linux/bln.h>
#include <linux/mutex.h>
#endif
#if defined(CONFIG_GENERIC_BLN) || defined(CONFIG_CM_BLN)
#include <linux/wakelock.h>
#endif

/*
touchkey register
*/
#define KEYCODE_REG 0x00
#define FIRMWARE_VERSION 0x01
#define TOUCHKEY_MODULE_VERSION 0x02
#define TOUCHKEY_ADDRESS	0x20

#define UPDOWN_EVENT_BIT 0x08
#define KEYCODE_BIT 0x07
#define ESD_STATE_BIT 0x10

#define I2C_M_WR 0		/* for i2c */

#define DEVICE_NAME "sec_touchkey"
#define TOUCH_FIRMWARE_V04  0x04
#define TOUCH_FIRMWARE_V07  0x07
#define DOOSUNGTECH_TOUCH_V1_2  0x0C

#define TK_FIRMWARE_VER	 0x04
#define TK_MODULE_VER    0x00

/*
 * Generic LED Notification functionality.
 */
#ifdef CONFIG_GENERIC_BLN
static struct wake_lock bln_wake_lock;
static bool touchkey_suspend = false;
static DEFINE_MUTEX(bln_sem);
#endif

#ifdef CONFIG_CM_BLN
/*
 * Standard CyanogenMod LED Notification functionality.
 */
#define ENABLE_BL        ( 1)
#define DISABLE_BL       ( 2)
#define BL_ALWAYS_ON     (-1)
#define BL_ALWAYS_OFF    (-2)

/* Breathing defaults */
#define BREATHING_STEP_INCR   (  50)
#define BREATHING_STEP_INT    ( 100)
#define BREATHING_MIN_VOLT    (2500)
#define BREATHING_MAX_VOLT    (3300)
#define BREATHING_PAUSE       ( 700)
/* Blinking defaults */
#define BLINKING_INTERVAL_ON  (1000) /* 1 second on */
#define BLINKING_INTERVAL_OFF (1000) /* 1 second off */

static int led_on = 0;
static int screen_on = 1;
static bool touch_led_control_enabled = false;
static int led_timeout = BL_ALWAYS_ON; /* never time out */
static int notification_enabled = -1; /* disabled by default */
static int notification_timeout = -1; /* never time out */
static struct wake_lock led_wake_lock;
static DEFINE_MUTEX(enable_sem);

static int led_brightness;
static bool fade_out = true;
static bool breathing_enabled = false;
static bool breathe_in = true;
static unsigned int breathe_volt;

static struct breathing {
	unsigned int min;
	unsigned int max;
	unsigned int step_incr;
	unsigned int step_int;
	unsigned int pause;
} breathe = {
	.min = BREATHING_MIN_VOLT,
	.max = BREATHING_MAX_VOLT,
	.step_incr = BREATHING_STEP_INCR,
	.step_int = BREATHING_STEP_INT,
	.pause = BREATHING_PAUSE,
};

static bool blinking_enabled = false;
static bool blink_on = true;

static struct blinking {
	unsigned int int_on;
	unsigned int int_off;
} blink = {
	.int_on = BLINKING_INTERVAL_ON,
	.int_off = BLINKING_INTERVAL_OFF,
};

/* timer related declares */
static struct timer_list led_timer;
static void bl_off(struct work_struct *bl_off_work);
static DECLARE_WORK(bl_off_work, bl_off);
static struct timer_list notification_timer;
static void notification_off(struct work_struct *notification_off_work);
static DECLARE_WORK(notification_off_work, notification_off);
static struct timer_list breathing_timer;
static void breathing_timer_action(struct work_struct *breathing_off_work);
static DECLARE_WORK(breathing_off_work, breathing_timer_action);
#endif

static int touchkey_keycode[3] = { 0, KEY_MENU, KEY_BACK };
static const int touchkey_count = sizeof(touchkey_keycode) / sizeof(int);

static int get_touchkey_module_version(void);

static u8 menu_sensitivity = 0;
static u8 back_sensitivity = 0;

static int touchkey_enable = 0;
static bool touchkey_probe = true;
/*sec_class sysfs*/
extern struct class *sec_class;
struct device *sec_touchkey;

struct i2c_touchkey_driver {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct early_suspend early_suspend;
};
struct i2c_touchkey_driver *touchkey_driver = NULL;
struct work_struct touchkey_work;
struct workqueue_struct *touchkey_wq;

struct work_struct touch_update_work;
struct delayed_work touch_resume_work;

static const struct i2c_device_id melfas_touchkey_id[] = {
	{"melfas_touchkey", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, melfas_touchkey_id);

static void init_hw(void);
static int i2c_touchkey_probe(struct i2c_client *client, const struct i2c_device_id *id);
extern int get_touchkey_firmware(char *version);
static int touchkey_led_status = 0;
static int touchled_cmd_reversed = 0;

struct i2c_driver touchkey_i2c_driver = {
	.driver = {
	   .name = "melfas_touchkey_driver",
	},
	.id_table = melfas_touchkey_id,
	.probe = i2c_touchkey_probe,
};

static int touchkey_debug_count = 0;
static char touchkey_debug[104];
static int touch_version = 0;
static int module_version = 0;
static int store_module_version = 0;
extern int touch_is_pressed;

static int touchkey_update_status;

/* led i2c write value convert helper */
static int inline touchkey_convert_led_value(int data) {
#if defined(CONFIG_MACH_Q1_BD)
	if (data == 1)
		return 0x10;
	else if (data == 2)
		return 0x20;
#elif defined(CONFIG_TARGET_LOCALE_NA)
	if (store_module_version >= 8) {
		if (data == 1)
			return 0x10;
		else if (data == 2)
			return 0x20;
	}
#endif
	return data;
}

int touchkey_led_ldo_on(bool on)
{
	struct regulator *regulator;

	if (on) {
		regulator = regulator_get(NULL, "touch_led");
		if (IS_ERR(regulator)) {
			printk(KERN_ERR "[TouchKey] touchkey_led_ldo_on(1): regulator error\n");
			return 0;
		}
		regulator_enable(regulator);
		regulator_put(regulator);
	} else {
		regulator = regulator_get(NULL, "touch_led");
		if (IS_ERR(regulator)) {
			printk(KERN_ERR "[TouchKey] touchkey_led_ldo_on(0): regulator error\n");
			return 0;
		}
		if (regulator_is_enabled(regulator))
			regulator_force_disable(regulator);
		regulator_put(regulator);
	}
	return 0;
}

int touchkey_ldo_on(bool on)
{
	struct regulator *regulator;

	if (on) {
		regulator = regulator_get(NULL, "touch");
		if (IS_ERR(regulator)) {
			printk(KERN_ERR "[TouchKey] touchkey_ldo_on(1): regulator error\n");
			return 0;
		}
		regulator_enable(regulator);
		regulator_put(regulator);
	} else {
		regulator = regulator_get(NULL, "touch");
		if (IS_ERR(regulator))
			return 0;
		if (regulator_is_enabled(regulator))
			regulator_force_disable(regulator);
		regulator_put(regulator);
	}

	return 1;
}

static void change_touch_key_led_voltage(int vol_mv)
{
	struct regulator *tled_regulator;

	tled_regulator = regulator_get(NULL, "touch_led");
	if (IS_ERR(tled_regulator)) {
		pr_err("%s: failed to get resource %s\n", __func__, "touch_led");
		return;
	}
	regulator_set_voltage(tled_regulator, vol_mv * 1000, vol_mv * 1000);
	regulator_put(tled_regulator);
}

static void get_touch_key_led_voltage(void)
{
	struct regulator *tled_regulator;

	tled_regulator = regulator_get(NULL, "touch_led");
	if (IS_ERR(tled_regulator)) {
		pr_err("%s: failed to get resource %s\n", __func__,
		       "touch_led");
		return;
	}

	led_brightness = regulator_get_voltage(tled_regulator) / 1000;

}

static ssize_t brightness_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", led_brightness);
}

static ssize_t brightness_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int data;

	if (sscanf(buf, "%d\n", &data) == 1) {
		printk(KERN_ERR "[TouchKey] touch_led_brightness: %d\n", data);
		change_touch_key_led_voltage(data);
		led_brightness = data;
	} else {
		printk(KERN_ERR "[TouchKey] touch_led_brightness Error\n");
	}

	return size;
}

static void set_touchkey_debug(char value)
{
	if (touchkey_debug_count == 100)
		touchkey_debug_count = 0;

	touchkey_debug[touchkey_debug_count] = value;
	touchkey_debug_count++;
}

static int i2c_touchkey_read(u8 reg, u8 *val, unsigned int len)
{
	int err = 0;
	int retry = 2;
	struct i2c_msg msg[1];

	if ((touchkey_driver == NULL) || !(touchkey_enable == 1) || !touchkey_probe) {
		printk(KERN_ERR "[TouchKey] touchkey is not enabled. %d\n", __LINE__);
		return -ENODEV;
	}

	while (retry--) {
		msg->addr = touchkey_driver->client->addr;
		msg->flags = I2C_M_RD;
		msg->len = len;
		msg->buf = val;
		err = i2c_transfer(touchkey_driver->client->adapter, msg, 1);

		if (err >= 0)
			return 0;
		printk(KERN_ERR "[TouchKey] %s %d i2c transfer error\n", __func__, __LINE__);
		mdelay(10);
	}
	return err;

}

static int i2c_touchkey_write(u8 *val, unsigned int len)
{
	int err = 0;
	struct i2c_msg msg[1];
	int retry = 2;

	if ((touchkey_driver == NULL) || !(touchkey_enable == 1) || !touchkey_probe) {
		printk(KERN_ERR "[TouchKey] touchkey is not enabled. %d\n", __LINE__);
		return -ENODEV;
	}

	while (retry--) {
		msg->addr = touchkey_driver->client->addr;
		msg->flags = I2C_M_WR;
		msg->len = len;
		msg->buf = val;
		err = i2c_transfer(touchkey_driver->client->adapter, msg, 1);

		if (err >= 0)
			return 0;

		printk(KERN_DEBUG "[TouchKey] %s %d i2c transfer error\n",__func__, __LINE__);
		mdelay(10);
	}
	return err;
}

void touchkey_firmware_update(void)
{
	char data[3];
	int retry;
	int ret = 0;

	ret = i2c_touchkey_read(KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_DEBUG "[TouchKey] i2c read fail. do not excute firm update.\n");
		return;
	}

	printk(KERN_ERR "%s F/W version: 0x%x, Module version:0x%x\n", __func__, data[1], data[2]);
	retry = 3;

	touch_version = data[1];
	module_version = data[2];

	if (touch_version < 0x0A) {
		touchkey_update_status = 1;
		while (retry--) {
			if (ISSP_main() == 0) {
				printk(KERN_ERR "[TouchKey] Touchkey_update succeeded\n");
				touchkey_update_status = 0;
				break;
			}
			printk(KERN_ERR "[TouchKey] touchkey_update failed...retry...\n");
		}
		if (retry <= 0) {
			touchkey_ldo_on(0);
			touchkey_update_status = -1;
			msleep(300);
		}

		init_hw();
	} else {
		if (touch_version >= 0x0A) {
			printk(KERN_ERR "[TouchKey] Not F/W update. Cypess touch-key F/W version is latest\n");
		} else {
			printk(KERN_ERR "[TouchKey] Not F/W update. Cypess touch-key version(module or F/W) is not valid\n");
		}
	}
}

void touchkey_work_func(struct work_struct *p)
{
	u8 data[3];
	int ret;
	int retry = 10;
	int keycode_type = 0;
	int pressed;

	set_touchkey_debug('a');

	retry = 3;
	while (retry--) {
		ret = i2c_touchkey_read(KEYCODE_REG, data, 3);
		if (!ret) {
			break;
		} else {
			printk(KERN_DEBUG "[TouchKey] i2c read failed, ret:%d, retry: %d\n", ret, retry);
			continue;
		}
	}
	if (ret < 0) {
		enable_irq(IRQ_TOUCH_INT);
		return;
	}
	set_touchkey_debug(data[0]);

	keycode_type = (data[0] & KEYCODE_BIT);
	pressed = !(data[0] & UPDOWN_EVENT_BIT);

	if (keycode_type <= 0 || keycode_type >= touchkey_count) {
		printk(KERN_DEBUG "[Touchkey] keycode_type err\n");
		enable_irq(IRQ_TOUCH_INT);
		return;
	}

	if (pressed)
		set_touchkey_debug('P');

	if (get_tsp_status() && pressed)
		printk(KERN_DEBUG "[TouchKey] touchkey pressed but don't send event because touch is pressed.\n");
	else {
		input_report_key(touchkey_driver->input_dev, touchkey_keycode[keycode_type], pressed);
		input_sync(touchkey_driver->input_dev);
		/* printk(KERN_DEBUG "[TouchKey] keycode:%d pressed:%d\n", touchkey_keycode[keycode_index], pressed); */
	}

	/* we have timed out or the lights should be on */
	if (led_timer.expires > jiffies || led_timeout != BL_ALWAYS_OFF) {
		int status = touchkey_convert_led_value(1);
		change_touch_key_led_voltage(led_brightness);
		i2c_touchkey_write((u8 *)&status, 1); /* turn on */
	}

	/* restart the timer */
	if (led_timeout > 0) {
		mod_timer(&led_timer, jiffies + msecs_to_jiffies(led_timeout));
	}

	set_touchkey_debug('A');
	enable_irq(IRQ_TOUCH_INT);
}

static irqreturn_t touchkey_interrupt(int irq, void *dummy)
{
	set_touchkey_debug('I');
	disable_irq_nosync(IRQ_TOUCH_INT);
	queue_work(touchkey_wq, &touchkey_work);

	return IRQ_HANDLED;
}

#ifdef CONFIG_GENERIC_BLN
static void touchkey_bln_wakeup(void)
{
	if (!wake_lock_active(&bln_wake_lock)) {
		printk(KERN_DEBUG "[TouchKey] touchkey wakeup wake_lock");
		wake_lock(&bln_wake_lock);
	}
	touchkey_ldo_on(1);
	msleep(50);
	touchkey_led_ldo_on(1);
	touchkey_enable = 1;
}

static void touchkey_bln_sleep(void)
{
	touchkey_led_ldo_on(0);
	touchkey_ldo_on(0);
	touchkey_enable = 0;
	printk(KERN_DEBUG "[TouchKey] touchkey sleep wake_unlock");
	wake_unlock(&bln_wake_lock);
}

static void melfas_enable_touchkey_backlights(void) {
	int value;

	printk(KERN_DEBUG "[TouchKey] %s\n", __func__);
	mutex_lock(&bln_sem);
	if (touchkey_suspend)
	{
		if (touchkey_enable == 0) {
			touchkey_bln_wakeup();
		}
		change_touch_key_led_voltage(led_brightness);
		value = touchkey_convert_led_value(1);
		printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, value);
		i2c_touchkey_write((u8 *)&value, 1);
		touchkey_led_status = 2;
		touchled_cmd_reversed = 1;
	}
	mutex_unlock(&bln_sem);
}

static void melfas_disable_touchkey_backlights(void) {
	int value;

	printk(KERN_DEBUG "[TouchKey] %s\n", __func__);
	mutex_lock(&bln_sem);
	if (touchkey_suspend)
	{
		value = touchkey_convert_led_value(2);
		printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, value);
		i2c_touchkey_write((u8 *)&value, 1);
		if (touchkey_enable == 1) {
			touchkey_bln_sleep();
		}
		touchkey_led_status = 1;
		touchled_cmd_reversed = 1;
	}
	mutex_unlock(&bln_sem);
}

static struct bln_implementation cypress_touchkey_bln = {
	.enable = melfas_enable_touchkey_backlights,
	.disable = melfas_disable_touchkey_backlights,
};
#endif // CONFIG_GENERIC_BLN

#ifdef CONFIG_CM_BLN
/*
 * Start of the main LED Notify code block
 */
static void enable_touchkey_backlights(void)
{
        int status = touchkey_convert_led_value(1);
        i2c_touchkey_write((u8 *)&status, 1);
}

static void disable_touchkey_backlights(void)
{
        int status = touchkey_convert_led_value(2);
        i2c_touchkey_write((u8 *)&status, 1);
}

static void reset_breathing(void)
{
	breathe_in = true;
	breathe_volt = breathe.min;
	if (breathing_enabled)
		change_touch_key_led_voltage(breathe.min);
}

static void led_fadeout(void)
{
	int i, status = 2;

	for (i = led_brightness; i >= BREATHING_MIN_VOLT; i -= 50) {
		change_touch_key_led_voltage(i);
		msleep(50);
	}

	i2c_touchkey_write((u8 *)&status, 1);
}

static void bl_off(struct work_struct *bl_off_work)
{
	int status;

	/* do nothing if there is an active notification */
	if (led_on || !touchkey_enable)
		return;

	/* we have timed out, turn the lights off */
	if (fade_out) {
		led_fadeout();
	} else {
		status = touchkey_convert_led_value(2);
		printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status);
		i2c_touchkey_write((u8 *)&status, 1);
	}

	return;
}

static void handle_led_timeout(unsigned long data)
{
	/* we cannot call the timeout directly as it causes a kernel spinlock BUG, schedule it instead */
	schedule_work(&bl_off_work);
}

static void notification_off(struct work_struct *notification_off_work)
{
	int status;

	/* do nothing if there is no active notification */
	if (!led_on || !touchkey_enable)
		return;

	/* we have timed out, turn the lights off */
	/* disable the regulators */
	touchkey_led_ldo_on(0);	/* "touch_led" regulator */
	touchkey_ldo_on(0);	/* "touch" regulator */

	/* turn off the backlight */
	status = touchkey_convert_led_value(2); /* light off */
	printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status);
	i2c_touchkey_write((u8 *)&status, 1);
	touchkey_enable = 0;
	led_on = 0;

	/* we were using a wakelock, unlock it */
	if (wake_lock_active(&led_wake_lock)) {
		wake_unlock(&led_wake_lock);
	}

	return;
}

static void handle_notification_timeout(unsigned long data)
{
	/* we cannot call the timeout directly as it causes a kernel spinlock BUG, schedule it instead */
	schedule_work(&notification_off_work);
}

static void start_breathing_timer(void)
{
	mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(10));
}

static void breathing_timer_action(struct work_struct *breathing_off_work)
{
	if (breathing_enabled && led_on) {
		if (breathe_in) {
			change_touch_key_led_voltage(breathe_volt);
			breathe_volt += breathe.step_incr;
			if (breathe_volt >= breathe.max) {
				breathe_volt = breathe.max;
				breathe_in = false;
			}
			mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(breathe.step_int));
		} else {
			change_touch_key_led_voltage(breathe_volt);
			breathe_volt -= breathe.step_incr;
			if (breathe_volt <= breathe.min) {
				reset_breathing();
				mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(breathe.pause));
			} else {
				mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(breathe.step_int));
			}
		}
	} else if (blinking_enabled && led_on) {
		if (blink_on) {
			enable_touchkey_backlights();
			mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(blink.int_on));
			blink_on = false;
		} else {
			disable_touchkey_backlights();
			mod_timer(&breathing_timer, jiffies + msecs_to_jiffies(blink.int_off));
			blink_on = true;
		}
	}

	return;
}

static void handle_breathing_timeout(unsigned long data)
{
	/* we cannot call the timeout directly as it causes a kernel spinlock BUG, schedule it instead */
	schedule_work(&breathing_off_work);
}

static ssize_t led_status_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%u\n", led_on);
}

static ssize_t led_status_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int status;

	if(sscanf(buf,"%u\n", &data ) == 1) {

		switch (data) {
		case ENABLE_BL:
			printk(KERN_DEBUG "[LED] ENABLE_BL\n");
			if (notification_enabled > 0) {
				/* we are using a wakelock, activate it */
				if (!wake_lock_active(&led_wake_lock)) {
					wake_lock(&led_wake_lock);
				}

				if (!screen_on) {
					/* enable regulators */
					touchkey_ldo_on(1);         /* "touch" regulator */
					touchkey_led_ldo_on(1);		/* "touch_led" regulator */
					touchkey_enable = 1;
				}

				/* enable the backlight */
				change_touch_key_led_voltage(led_brightness);
				status = touchkey_convert_led_value(1);
				printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status);
				i2c_touchkey_write((u8 *)&status, 1);
				led_on = 1;

				/* start breathing timer */
				if (breathing_enabled || blinking_enabled) {
					reset_breathing();
					start_breathing_timer();
				}

				/* See if a timeout value has been set for the notification */
				if (notification_timeout > 0) {
					/* restart the timer */
					mod_timer(&notification_timer, jiffies + msecs_to_jiffies(notification_timeout));
				}
			}
			break;

		case DISABLE_BL:
			printk(KERN_DEBUG "[LED] DISABLE_BL\n");

			/* prevent race with late resume*/
			mutex_lock(&enable_sem);

			/* only do this if a notification is on already, do nothing if not */
			if (led_on) {

				/* turn off the backlight */
				status = touchkey_convert_led_value(2); /* light off */
				/* printk(KERN_DEBUG "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status); */
				i2c_touchkey_write((u8 *)&status, 1);
				led_on = 0;

				if (!screen_on) {
					/* disable the regulators */
					touchkey_led_ldo_on(0);	/* "touch_led" regulator */
					touchkey_ldo_on(0);	/* "touch" regulator */
					touchkey_enable = 0;
				}

				/* a notification timeout was set, disable the timer */
				if (notification_timeout > 0) {
					del_timer(&notification_timer);
				}

				/* disable the breathing timer */
				if (breathing_enabled || blinking_enabled) {
					del_timer(&breathing_timer);
				}

				/* we were using a wakelock, unlock it */
				if (wake_lock_active(&led_wake_lock)) {
					wake_unlock(&led_wake_lock);
				}
			}

			/* prevent race */
			mutex_unlock(&enable_sem);

			break;
		}
	}

	return size;
}

static ssize_t led_timeout_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", led_timeout);
}

static ssize_t led_timeout_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	sscanf(buf,"%d\n", &led_timeout);
	return size;
}

static ssize_t notification_enabled_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", notification_enabled);
}

static ssize_t notification_enabled_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	sscanf(buf,"%d\n", &notification_enabled);
	return size;
}

static ssize_t notification_timeout_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", notification_timeout);
}

static ssize_t notification_timeout_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	sscanf(buf,"%d\n", &notification_timeout);
	return size;
}

static ssize_t enable_breathing_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf, "%u\n", (breathing_enabled ? 1 : 0));
}

static ssize_t enable_breathing_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 0 || data > 1)
		return -EINVAL;

	breathing_enabled = (data ? true : false);

	if (blinking_enabled)
		blinking_enabled = false;

	return size;
}

static ssize_t breathing_step_incr_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", breathe.step_incr);
}

static ssize_t breathing_step_incr_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 10 || data > 100)
		return -EINVAL;

	breathe.step_incr = data;
	return size;
}

static ssize_t breathing_step_int_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", breathe.step_int);
}

static ssize_t breathing_step_int_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 10 || data > 100)
		return -EINVAL;

	breathe.step_int = data;
	return size;
}

static ssize_t breathing_max_volt_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", breathe.max);
}

static ssize_t breathing_max_volt_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < BREATHING_MIN_VOLT || data > BREATHING_MAX_VOLT)
		return -EINVAL;

	breathe.max = data;
	return size;
}

static ssize_t breathing_min_volt_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", breathe.min);
}

static ssize_t breathing_min_volt_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < BREATHING_MIN_VOLT || data > BREATHING_MAX_VOLT)
		return -EINVAL;

	breathe.min = data;
	return size;
}

static ssize_t breathing_pause_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", breathe.pause);
}

static ssize_t breathing_pause_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 100 || data > 5000)
		return -EINVAL;

	breathe.pause = data;
	return size;
}

static ssize_t enable_blinking_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf, "%u\n", (blinking_enabled ? 1 : 0));
}

static ssize_t enable_blinking_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 0 || data > 1)
		return -EINVAL;

	blinking_enabled = (data ? true : false);

	if (breathing_enabled)
		breathing_enabled = false;

	return size;
}

static ssize_t blinking_int_on_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", blink.int_on);
}

static ssize_t blinking_int_on_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 1 || data > 10000)
		return -EINVAL;

	blink.int_on = data;
	return size;
}

static ssize_t blinking_int_off_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf,"%d\n", blink.int_off);
}

static ssize_t blinking_int_off_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 1 || data > 10000)
		return -EINVAL;

	blink.int_off = data;
	return size;
}

static ssize_t led_fadeout_read( struct device *dev, struct device_attribute *attr, char *buf )
{
	return sprintf(buf, "%u\n", (fade_out ? 1 : 0));
}

static ssize_t led_fadeout_write( struct device *dev, struct device_attribute *attr, const char *buf, size_t size )
{
	unsigned int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1 || data < 0 || data > 1)
		return -EINVAL;

	fade_out = (data ? true : false);

	return size;
}

static DEVICE_ATTR(led, S_IRUGO | S_IWUGO, led_status_read, led_status_write );
static DEVICE_ATTR(led_timeout, S_IRUGO | S_IWUGO, led_timeout_read, led_timeout_write );
static DEVICE_ATTR(notification_enabled, S_IRUGO | S_IWUGO, notification_enabled_read, notification_enabled_write );
static DEVICE_ATTR(notification_timeout, S_IRUGO | S_IWUGO, notification_timeout_read, notification_timeout_write );
static DEVICE_ATTR(breathing_enabled, S_IRUGO | S_IWUGO, enable_breathing_read, enable_breathing_write );
static DEVICE_ATTR(breathing_step_increment, S_IRUGO | S_IWUGO, breathing_step_incr_read, breathing_step_incr_write );
static DEVICE_ATTR(breathing_step_interval, S_IRUGO | S_IWUGO, breathing_step_int_read, breathing_step_int_write );
static DEVICE_ATTR(breathing_max_volt, S_IRUGO | S_IWUGO, breathing_max_volt_read, breathing_max_volt_write );
static DEVICE_ATTR(breathing_min_volt, S_IRUGO | S_IWUGO, breathing_min_volt_read, breathing_min_volt_write );
static DEVICE_ATTR(breathing_pause, S_IRUGO | S_IWUGO, breathing_pause_read, breathing_pause_write );
static DEVICE_ATTR(blinking_enabled, S_IRUGO | S_IWUGO, enable_blinking_read, enable_blinking_write );
static DEVICE_ATTR(blinking_int_on, S_IRUGO | S_IWUGO, blinking_int_on_read, blinking_int_on_write );
static DEVICE_ATTR(blinking_int_off, S_IRUGO | S_IWUGO, blinking_int_off_read, blinking_int_off_write );
static DEVICE_ATTR(led_fadeout, S_IRUGO | S_IWUGO, led_fadeout_read, led_fadeout_write );

static struct attribute *bl_led_attributes[] = {
	&dev_attr_led.attr,
	&dev_attr_led_timeout.attr,
	&dev_attr_notification_enabled.attr,
	&dev_attr_notification_timeout.attr,
	&dev_attr_breathing_enabled.attr,
	&dev_attr_breathing_step_increment.attr,
	&dev_attr_breathing_step_interval.attr,
	&dev_attr_breathing_max_volt.attr,
	&dev_attr_breathing_min_volt.attr,
	&dev_attr_breathing_pause.attr,
	&dev_attr_blinking_enabled.attr,
	&dev_attr_blinking_int_on.attr,
	&dev_attr_blinking_int_off.attr,
	&dev_attr_led_fadeout.attr,
	NULL
};

static struct attribute_group bln_notification_group = {
	.attrs = bl_led_attributes,
};

static struct miscdevice led_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "notification",
};

/*
 * End of the main LED Notification code block, minor ones below
 */
#endif // CONFIG_CM_BLN

#ifdef CONFIG_HAS_EARLYSUSPEND
static int melfas_touchkey_early_suspend(struct early_suspend *h)
{
	int ret;
	int i;

	disable_irq(IRQ_TOUCH_INT);
	ret = cancel_work_sync(&touchkey_work);
	if (ret) {
		printk(KERN_DEBUG "[Touchkey] enable_irq ret=%d\n", ret);
		enable_irq(IRQ_TOUCH_INT);
	}

	/* release keys */
	for (i = 1; i < touchkey_count; ++i) {
		input_report_key(touchkey_driver->input_dev, touchkey_keycode[i], 0);
	}

#ifdef CONFIG_GENERIC_BLN
	mutex_lock(&bln_sem);
	touchkey_suspend = true;
#endif

	touchkey_enable = 0;
	set_touchkey_debug('S');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_early_suspend\n");
	if (touchkey_enable < 0) {
		printk(KERN_DEBUG "[TouchKey] ---%s---touchkey_enable: %d\n", __func__, touchkey_enable);
#ifdef CONFIG_GENERIC_BLN
		mutex_unlock(&bln_sem);
#endif
		return 0;
	}

	gpio_direction_input(_3_GPIO_TOUCH_INT);

	/* disable ldo18 */
	touchkey_led_ldo_on(0);

	/* disable ldo11 */
	touchkey_ldo_on(0);

#ifdef CONFIG_CM_BLN
	screen_on = 0;
#endif
#ifdef CONFIG_GENERIC_BLN
	mutex_unlock(&bln_sem);
#endif
	return 0;
}

static int melfas_touchkey_late_resume(struct early_suspend *h)
{
	set_touchkey_debug('R');
	printk(KERN_DEBUG "[TouchKey] sec_touchkey_late_resume\n");

#ifdef CONFIG_GENERIC_BLN
	mutex_lock(&bln_sem);
#endif
#ifdef CONFIG_CM_BLN
	/* Avoid race condition with LED notification disable */
	mutex_lock(&enable_sem);
#endif

	/* enable ldo11 */
	touchkey_ldo_on(1);

	if (touchkey_enable < 0) {
		printk(KERN_DEBUG "[TouchKey] ---%s---touchkey_enable: %d\n", __func__, touchkey_enable);
#ifdef CONFIG_CM_BLN
		mutex_unlock(&enable_sem);
#endif
#ifdef CONFIG_GENERIC_BLN
		mutex_unlock(&bln_sem);
#endif
		return 0;
	}
	gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
	gpio_direction_output(_3_TOUCH_SDA_28V, 1);
	gpio_direction_output(_3_TOUCH_SCL_28V, 1);

	gpio_direction_output(_3_GPIO_TOUCH_INT, 1);
	set_irq_type(IRQ_TOUCH_INT, IRQF_TRIGGER_FALLING);
	s3c_gpio_cfgpin(_3_GPIO_TOUCH_INT, _3_GPIO_TOUCH_INT_AF);
	s3c_gpio_setpull(_3_GPIO_TOUCH_INT, S3C_GPIO_PULL_NONE);
	msleep(50);
	touchkey_led_ldo_on(1);

	touchkey_enable = 1;

#ifdef CONFIG_CM_BLN
	touch_led_control_enabled = true;
	screen_on = 1;
	/* see if late_resume is running before DISABLE_BL */
	if (led_on) {
		/* if a notification timeout was set, disable the timer */
		if (notification_timeout > 0) {
			del_timer(&notification_timer);
		}

		/* we were using a wakelock, unlock it */
		if (wake_lock_active(&led_wake_lock)) {
			wake_unlock(&led_wake_lock);
		}
		/* force DISABLE_BL to ignore the led state because we want it left on */
		led_on = 0;
	}

	if (led_timeout != BL_ALWAYS_OFF) {
		/* ensure the light is ON */
		int status = touchkey_convert_led_value(1);
		change_touch_key_led_voltage(led_brightness);
		printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status);
		i2c_touchkey_write((u8 *)&status, 1);
	}

	/* restart the timer if needed */
	if (led_timeout > 0) {
		mod_timer(&led_timer, jiffies + msecs_to_jiffies(led_timeout));
	}
#endif

#ifdef CONFIG_GENERIC_BLN
	touchkey_suspend = false;
	printk(KERN_DEBUG "[TouchKey] bln_wake_unlock\n");
	wake_unlock(&bln_wake_lock);
#endif

	/* all done, turn on IRQ */
	enable_irq(IRQ_TOUCH_INT);

#ifdef CONFIG_CM_BLN
	/* Avoid race condition with LED notification disable */
	mutex_unlock(&enable_sem);
#endif
#ifdef CONFIG_GENERIC_BLN
	mutex_unlock(&bln_sem);
#endif
	return 0;
}
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int i2c_touchkey_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct input_dev *input_dev;
	int err = 0;
	unsigned char data;
	int i;
	int module_version;

	printk(KERN_DEBUG "[TouchKey] i2c_touchkey_probe\n");

	touchkey_driver = kzalloc(sizeof(struct i2c_touchkey_driver), GFP_KERNEL);
	if (touchkey_driver == NULL) {
		dev_err(dev, "failed to create our state\n");
		return -ENOMEM;
	}

	touchkey_driver->client = client;
	touchkey_driver->client->irq = IRQ_TOUCH_INT;
	strlcpy(touchkey_driver->client->name, "melfas-touchkey", I2C_NAME_SIZE);

	input_dev = input_allocate_device();

	if (!input_dev)
		return -ENOMEM;

	touchkey_driver->input_dev = input_dev;

	input_dev->name = DEVICE_NAME;
	input_dev->phys = "melfas-touchkey/input0";
	input_dev->id.bustype = BUS_HOST;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);

	for (i = 1; i < touchkey_count; i++)
		set_bit(touchkey_keycode[i], input_dev->keybit);

	err = input_register_device(input_dev);
	if (err) {
		input_free_device(input_dev);
		return err;
	}

	/* enable ldo18 */
	touchkey_ldo_on(1);

	msleep(50);

	touchkey_enable = 1;
	data = 1;

	module_version = get_touchkey_module_version();
	if (module_version < 0) {
		printk(KERN_ERR "[TouchKey] Probe fail\n");
		input_unregister_device(input_dev);
		touchkey_probe = false;
		return -ENODEV;
	}

	if (request_irq(IRQ_TOUCH_INT, touchkey_interrupt, IRQF_TRIGGER_FALLING, DEVICE_NAME, NULL)) {
		printk(KERN_ERR "[TouchKey] %s Can't allocate irq ..\n", __func__);
		return -EBUSY;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	touchkey_driver->early_suspend.suspend = (void *) melfas_touchkey_early_suspend;
	touchkey_driver->early_suspend.resume = (void *) melfas_touchkey_late_resume;
	register_early_suspend(&touchkey_driver->early_suspend);
#endif

	touchkey_led_ldo_on(1);

	set_touchkey_debug('K');

#ifdef CONFIG_CM_BLN
	err = misc_register(&led_device);
	if (err) {
		printk(KERN_ERR "[LED Notify] sysfs misc_register failed.\n");
	} else {
		if( sysfs_create_group( &led_device.this_device->kobj, &bln_notification_group) < 0){
			printk(KERN_ERR "[LED Notify] sysfs create group failed.\n");
		}
	}

	/* Setup the timer for the timeouts */
	setup_timer(&led_timer, handle_led_timeout, 0);
	setup_timer(&notification_timer, handle_notification_timeout, 0);
	setup_timer(&breathing_timer, handle_breathing_timeout, 0);

	/* wake lock for LED Notify */
	wake_lock_init(&led_wake_lock, WAKE_LOCK_SUSPEND, "led_wake_lock");

	/* turn off the LED if it is not supposed to be always on */
	if (led_timeout != BL_ALWAYS_ON) {
		int status = touchkey_convert_led_value(2);
		printk(KERN_ERR "[TouchKey] %d : %s(%d)\n", __LINE__, __func__, status);
		i2c_touchkey_write((u8 *)&status, 1);
	}
#endif /* CONFIG_CM_BLN */

	return 0;
}

static void init_hw(void)
{
	gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
	msleep(200);
	s3c_gpio_setpull(_3_GPIO_TOUCH_INT, S3C_GPIO_PULL_NONE);
	set_irq_type(IRQ_TOUCH_INT, IRQF_TRIGGER_FALLING);
	s3c_gpio_cfgpin(_3_GPIO_TOUCH_INT, _3_GPIO_TOUCH_INT_AF);
}

static int get_touchkey_module_version()
{
	char data[3] = { 0, };
	int ret = 0;

	ret = i2c_touchkey_read(KEYCODE_REG, data, 3);
	if (ret < 0) {
		printk(KERN_ERR "[TouchKey] module version read fail\n");
		return ret;
	} else {
		printk(KERN_DEBUG "[TouchKey] Module Version: %d\n", data[2]);
		return data[2];
	}
}

int touchkey_update_open(struct inode *inode, struct file *filp)
{
	return 0;
}

ssize_t touchkey_update_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	char data[3] = { 0, };

	get_touchkey_firmware(data);
	put_user(data[1], buf);

	return 1;
}

int touchkey_update_release(struct inode *inode, struct file *filp)
{
	return 0;
}

struct file_operations touchkey_update_fops = {
	.owner = THIS_MODULE,
	.read = touchkey_update_read,
	.open = touchkey_update_open,
	.release = touchkey_update_release,
};

static struct miscdevice touchkey_update_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "melfas_touchkey",
	.fops = &touchkey_update_fops,
};

static ssize_t touch_version_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	char data[3] = { 0, };
	int count;

	init_hw();
	i2c_touchkey_read(KEYCODE_REG, data, 3);

	count = sprintf(buf, "0x%x\n", data[1]);

	printk(KERN_DEBUG "[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk(KERN_DEBUG "[TouchKey] module_version_read 0x%x\n", data[2]);

	return count;
}

static ssize_t touch_version_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	printk(KERN_DEBUG "[TouchKey] input data --> %s\n", buf);

	return size;
}

void touchkey_update_func(struct work_struct *p)
{
	int retry = 10;
	touchkey_update_status = 1;
	printk(KERN_DEBUG "[TouchKey] %s start\n", __func__);
	touchkey_enable = 0;
	while (retry--) {
		if (ISSP_main() == 0) {
			printk(KERN_DEBUG "[TouchKey] touchkey_update succeeded\n");
			init_hw();
			enable_irq(IRQ_TOUCH_INT);
			touchkey_enable = 1;
			touchkey_update_status = 0;
			return;
		}
	}

	touchkey_update_status = -1;
	printk(KERN_DEBUG "[TouchKey] touchkey_update failed\n");
	return;
}

static ssize_t touch_update_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	printk(KERN_DEBUG "[TouchKey] touchkey firmware update\n");
	if (*buf == 'S') {
		disable_irq(IRQ_TOUCH_INT);
		INIT_WORK(&touch_update_work, touchkey_update_func);
		queue_work(touchkey_wq, &touch_update_work);
	}
	return size;
}

static ssize_t touch_update_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count = 0;

	printk(KERN_DEBUG "[TouchKey] touch_update_read: touchkey_update_status %d\n", touchkey_update_status);

	if (touchkey_update_status == 0)
		count = sprintf(buf, "PASS\n");
	else if (touchkey_update_status == 1)
		count = sprintf(buf, "Downloading\n");
	else if (touchkey_update_status == -1)
		count = sprintf(buf, "Fail\n");

	return count;
}

static ssize_t touch_led_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int data, value;
	int errnum;

	if (sscanf(buf, "%d\n", &data) == 1) {
#ifdef CONFIG_CM_BLN
		if ((led_timeout == BL_ALWAYS_OFF && data == 1) || !touch_led_control_enabled) {
			return size;
		}
		touch_led_control_enabled = false;
#endif
		printk(KERN_ERR "[TouchKey] %s: %d \n", __func__, data);
		value = touchkey_convert_led_value(data);
		printk("[TouchKey] %s led %s\n", __func__, (value == 0x1 || value == 0x10) ? "on" : "off");
		errnum = i2c_touchkey_write((u8 *)&value, 1);
		if (errnum == -ENODEV)
			touchled_cmd_reversed = 1;

		touchkey_led_status = data;
	} else {
		printk(KERN_DEBUG "[TouchKey] touch_led_control Error\n");
	}

	return size;
}

static ssize_t touchkey_enable_disable(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	return size;
}

static ssize_t touchkey_menu_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(KEYCODE_REG, data, 10);
	menu_sensitivity = data[7];
	return sprintf(buf, "%d\n", menu_sensitivity);
}

static ssize_t touchkey_back_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 data[10];
	int ret;

	printk(KERN_DEBUG "called %s\n", __func__);
	ret = i2c_touchkey_read(KEYCODE_REG, data, 10);
	back_sensitivity = data[9];
	return sprintf(buf, "%d\n", back_sensitivity);
}

static ssize_t touch_sensitivity_control(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned char data = 0x40;
	i2c_touchkey_write(&data, 1);
	return size;
}

static ssize_t set_touchkey_firm_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", TK_FIRMWARE_VER);
}

static ssize_t set_touchkey_update_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* TO DO IT */
	int count = 0;
	int retry = 3;
	touchkey_update_status = 1;

	while (retry--) {
		if (ISSP_main() == 0) {
			printk(KERN_ERR "[TouchKey]Touchkey_update succeeded\n");
			touchkey_update_status = 0;
			count = 1;
			break;
		}
		printk(KERN_ERR "[TouchKey] touchkey_update failed... retry...\n");
	}
	if (retry <= 0) {
		/* disable ldo11 */
		touchkey_ldo_on(0);
		msleep(300);
		count = 0;
		printk(KERN_ERR "[TouchKey]Touchkey_update fail\n");
		touchkey_update_status = -1;
		return count;
	}

	init_hw();		/* after update, re initalize. */

	return count;
}

static ssize_t set_touchkey_firm_version_read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	char data[3] = { 0, };
	int count;

	init_hw();
	i2c_touchkey_read(KEYCODE_REG, data, 3);
	count = sprintf(buf, "0x%x\n", data[1]);

	printk(KERN_DEBUG "[TouchKey] touch_version_read 0x%x\n", data[1]);
	printk(KERN_DEBUG "[TouchKey] module_version_read 0x%x\n", data[2]);
	return count;
}

static ssize_t set_touchkey_firm_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count = 0;

	printk(KERN_DEBUG "[TouchKey] touch_update_read: touchkey_update_status %d\n", touchkey_update_status);

	if (touchkey_update_status == 0)
		count = sprintf(buf, "PASS\n");
	else if (touchkey_update_status == 1)
		count = sprintf(buf, "Downloading\n");
	else if (touchkey_update_status == -1)
		count = sprintf(buf, "Fail\n");

	return count;
}

#ifdef CONFIG_GENERIC_BLN
static ssize_t touchkey_bln_control(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	int data, value, errnum;

	if (sscanf(buf, "%d\n", &data) == 1) {
		printk(KERN_ERR "[TouchKey] %s: %d \n", __func__, data);
		value = touchkey_convert_led_value(data);
		printk("[TouchKey] %s led %s\n", __func__, (value == 0x1 || value == 0x10) ? "on" : "off");
		errnum = i2c_touchkey_write((u8 *)&value, 1);
		if (errnum == -ENODEV) {
			touchled_cmd_reversed = 1;
		}
		touchkey_led_status = data;
	} else {
		printk(KERN_ERR "[TouchKey] touchkey_bln_control Error\n");
	}

	return size;
}
#endif

static DEVICE_ATTR(touch_version, S_IRUGO | S_IWUSR | S_IWGRP, touch_version_read, touch_version_write);
static DEVICE_ATTR(touch_update, S_IRUGO | S_IWUSR | S_IWGRP, touch_update_read, touch_update_write);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL, touch_led_control);
static DEVICE_ATTR(enable_disable, S_IRUGO | S_IWUSR | S_IWGRP, NULL, touchkey_enable_disable);
static DEVICE_ATTR(touchkey_menu, S_IRUGO | S_IWUSR | S_IWGRP, touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO | S_IWUSR | S_IWGRP, touchkey_back_show, NULL);
static DEVICE_ATTR(touch_sensitivity, S_IRUGO | S_IWUSR | S_IWGRP, NULL, touch_sensitivity_control);
/*20110223N1 firmware sync*/
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_update_show, NULL);/* firmware update */
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_status_show, NULL);/* firmware update status */
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_version_show, NULL);/* PHONE */
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO | S_IWUSR | S_IWGRP, set_touchkey_firm_version_read_show, NULL);
/*PART*/
/*end N1 firmware sync*/
static DEVICE_ATTR(touchkey_brightness, S_IRUGO | S_IWUSR | S_IWGRP, brightness_read, brightness_control);
#ifdef CONFIG_GENERIC_BLN
static DEVICE_ATTR(touchkey_bln_control, S_IWUGO, NULL, touchkey_bln_control);
#endif

static int __init touchkey_init(void)
{
	int ret = 0;

	sec_touchkey = device_create(sec_class, NULL, 0, NULL, "sec_touchkey");

	if (IS_ERR(sec_touchkey))
		printk(KERN_ERR "Failed to create device(sec_touchkey)!\n");

	if (device_create_file(sec_touchkey, &dev_attr_touchkey_firm_update) < 0) {
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_touchkey_firm_update.attr.name);
	}
	if (device_create_file(sec_touchkey, &dev_attr_touchkey_firm_update_status) < 0) {
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_touchkey_firm_update_status.attr.name);
	}
	if (device_create_file(sec_touchkey, &dev_attr_touchkey_firm_version_phone) < 0) {
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_touchkey_firm_version_phone.attr.name);
	}
	if (device_create_file(sec_touchkey, &dev_attr_touchkey_firm_version_panel) < 0) {
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_touchkey_firm_version_panel.attr.name);
	}
	if (device_create_file(sec_touchkey, &dev_attr_touchkey_brightness) < 0) {
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_touchkey_brightness.attr.name);
	}

	ret = misc_register(&touchkey_update_device);
	if (ret) {
		printk(KERN_ERR "[TouchKey] %s misc_register fail\n", __func__);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_touch_version) < 0) {
		printk(KERN_ERR "[TouchKey] %s device_create_file fail dev_attr_touch_version\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_touch_version.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_touch_update) < 0) {
		printk(KERN_ERR "[TouchKey] %s device_create_file fail dev_attr_touch_update\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_touch_update.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_brightness) < 0) {
		printk(KERN_ERR "[TouchKey] %s device_create_file fail dev_attr_touch_update\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_brightness.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_enable_disable) < 0) {
		printk(KERN_ERR "[TouchKey] %s device_create_file fail dev_attr_touch_update\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_enable_disable.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_touchkey_menu) < 0) {
		printk(KERN_ERR "%s device_create_file fail dev_attr_touchkey_menu\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_touchkey_menu.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_touchkey_back) < 0) {
		printk(KERN_ERR "%s device_create_file fail dev_attr_touchkey_back\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_touchkey_back.attr.name);
	}

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_touch_sensitivity) < 0) {
		printk("%s device_create_file fail dev_attr_touch_sensitivity\n", __func__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_touch_sensitivity.attr.name);
	}

#ifdef CONFIG_GENERIC_BLN
	if (device_create_file(sec_touchkey, &dev_attr_touchkey_bln_control) < 0) {
		printk(KERN_ERR "%s device_create_file fail dev_attr_touchkey_bln_control\n", __func__);
	}
#endif

	touchkey_wq = create_singlethread_workqueue("melfas_touchkey_wq");
	if (!touchkey_wq)
		return -ENOMEM;

	INIT_WORK(&touchkey_work, touchkey_work_func);

	init_hw();

	ret = i2c_add_driver(&touchkey_i2c_driver);

	if (ret) {
		printk(KERN_ERR "[TouchKey] registration failed, module not inserted.ret= %d\n", ret);
	}

#ifdef CONFIG_GENERIC_BLN
	wake_lock_init(&bln_wake_lock, WAKE_LOCK_SUSPEND, "bln_wake_lock");
	register_bln_implementation(&cypress_touchkey_bln);
#endif

	/* read key led voltage */
	get_touch_key_led_voltage();
	return ret;
}

static void __exit touchkey_exit(void)
{
	printk(KERN_DEBUG "[TouchKey] %s\n", __func__);
	i2c_del_driver(&touchkey_i2c_driver);
	misc_deregister(&touchkey_update_device);

#ifdef CONFIG_CM_BLN
	misc_deregister(&led_device);
	wake_lock_destroy(&led_wake_lock);
	del_timer(&led_timer);
	del_timer(&notification_timer);
	del_timer(&breathing_timer);
#endif

#ifdef CONFIG_GENERIC_BLN
	wake_lock_destroy(&bln_wake_lock);
#endif

	if (touchkey_wq)
		destroy_workqueue(touchkey_wq);

	gpio_free(_3_TOUCH_SDA_28V);
	gpio_free(_3_TOUCH_SCL_28V);
	gpio_free(_3_GPIO_TOUCH_EN);
	gpio_free(_3_GPIO_TOUCH_INT);
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@@@");
MODULE_DESCRIPTION("melfas touch keypad");
