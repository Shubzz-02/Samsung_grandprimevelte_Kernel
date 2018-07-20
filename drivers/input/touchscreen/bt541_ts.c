/*
 *
 * Zinitix bt541 touchscreen driver
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
 * GNU General Public License for more details.
 *
 */


#define TSP_VERBOSE_DEBUG
#define SEC_FACTORY_TEST
#define SUPPORTED_TOUCH_KEY

#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#if defined(CONFIG_PM_RUNTIME)
#include <linux/pm_runtime.h>
#endif
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>

#include <linux/input/bt541_ts.h>
#include <linux/input/mt.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/async.h>

/* added header file */
/* #define SUPPORTED_USE_DUAL_FW */
#ifdef SUPPORTED_USE_DUAL_FW
#include "zinitix_bt541_firmware_00.h"
#include "zinitix_bt541_firmware_01.h"
u8 *m_firmware_data;
#else
#include "bt541_firmware.h"
#endif

#ifdef CONFIG_EXTCON_SM5504
#include <linux/extcon.h>
#endif

#ifdef CONFIG_INPUT_BOOSTER
#include <linux/input/input_booster.h>
#endif

extern void (*tsp_charger_status_cb)(int);
extern u32 boot_panel_id;

#ifdef SUPPORTED_USE_DUAL_FW
#define TSP_TYPE_COUNT				2
u8 *m_pFirmware[TSP_TYPE_COUNT] = {(u8 *)m_firmware_data_00, (u8 *)m_firmware_data_01,};
u8 m_FirmwareIdx = 0;
#define CHECK_HWID				1
static void bt541_firmware_check(void);
#else
#define CHECK_HWID				0
#endif

static bool ta_connected = 0;
#define ZINITIX_DEBUG				1
#define ZINITIX_I2C_CHECKSUM			1
#define TOUCH_BOOSTER				0
#define NOT_SUPPORTED_TOUCH_DUMMY_KEY
#if TOUCH_BOOSTER
#include <linux/cpufreq.h>
#include <linux/cpufreq_limit.h>
extern int _store_cpu_num_min_limit(unsigned int input);
struct cpufreq_limit_handle *min_handle = NULL;
static const unsigned long touch_cpufreq_lock = 1200000;
#endif

#ifdef SUPPORTED_PALM_TOUCH
#define TOUCH_POINT_MODE			2
#else
#define TOUCH_POINT_MODE			0
#endif

#define MAX_SUPPORTED_FINGER_NUM		2 /* max 10 */

#ifdef SUPPORTED_TOUCH_KEY
#ifdef NOT_SUPPORTED_TOUCH_DUMMY_KEY
#define MAX_SUPPORTED_BUTTON_NUM		2 /* max 8 */
#define SUPPORTED_BUTTON_NUM			2
#else
#define MAX_SUPPORTED_BUTTON_NUM		2 /* max 8 */
#define SUPPORTED_BUTTON_NUM			2
#endif
#endif

/* Upgrade Method*/
#define TOUCH_ONESHOT_UPGRADE			1
/* if you use isp mode, you must add i2c device :
   name = "zinitix_isp" , addr 0x50*/

/* resolution offset */
#define ABS_PT_OFFSET				(0)

#define TOUCH_FORCE_UPGRADE			1
#define USE_CHECKSUM				1

#define CHIP_OFF_DELAY				50 /*ms*/
#define CHIP_ON_DELAY				20 /*ms*/
#define FIRMWARE_ON_DELAY			60 /*ms*/

#define DELAY_FOR_SIGNAL_DELAY			30 /*us*/
#define DELAY_FOR_TRANSCATION			50
#define DELAY_FOR_POST_TRANSCATION		10

/* PMIC Regulator based supply to TSP */
#define TSP_REGULATOR_SUPPLY			1
/* gpio controlled LDO based supply to TSP */
#define TSP_LDO_SUPPLY				0

enum power_control {
	POWER_OFF,
	POWER_ON,
	POWER_ON_SEQUENCE,
};

/* Key Enum */
enum key_event {
	ICON_BUTTON_UNCHANGE,
	ICON_BUTTON_DOWN,
	ICON_BUTTON_UP,
};

/* ESD Protection */
/*second : if 0, no use. if you have to use, 3 is recommended*/
#define ESD_TIMER_INTERVAL			1
#define SCAN_RATE_HZ				100
#define CHECK_ESD_TIMER				3

/*Test Mode (Monitoring Raw Data) */
#define MAX_RAW_DATA_SZ				576 /* 32x18 */
#define MAX_TRAW_DATA_SZ	\
	(MAX_RAW_DATA_SZ + 4*MAX_SUPPORTED_FINGER_NUM + 2)
/* preriod raw data interval */

#define RAWDATA_DELAY_FOR_HOST			100

struct raw_ioctl {
	u32 sz;
	u32 buf;
};

struct reg_ioctl {
	u32 addr;
	u32 val;
};

#define TOUCH_SEC_MODE				48
#define TOUCH_REF_MODE				10
#define TOUCH_NORMAL_MODE			5
#define TOUCH_DELTA_MODE			3
#define TOUCH_SDND_MODE				6
#define TOUCH_DND_MODE				11

/*  Other Things */
#define INIT_RETRY_CNT				1
#define I2C_SUCCESS				0
#define I2C_FAIL				1

/*---------------------------------------------------------------------*/

/* Register Map*/
#define BT541_SWRESET_CMD			0x0000
#define BT541_WAKEUP_CMD			0x0001

#define BT541_IDLE_CMD				0x0004
#define BT541_SLEEP_CMD				0x0005

#define BT541_CLEAR_INT_STATUS_CMD		0x0003
#define BT541_CALIBRATE_CMD			0x0006
#define BT541_SAVE_STATUS_CMD			0x0007
#define BT541_SAVE_CALIBRATION_CMD		0x0008
#define BT541_RECALL_FACTORY_CMD		0x000f

#define BT541_THRESHOLD				0x0020

#define BT541_DEBUG_REG				0x0115 /* 0~7 */

#define BT541_TOUCH_MODE			0x0010
#define BT541_CHIP_REVISION			0x0011
#define BT541_FIRMWARE_VERSION			0x0012

#define BT541_MINOR_FW_VERSION			0x0121

#define BT541_VENDOR_ID				0x001C
#define BT541_MODULE_ID				0x001E
#define BT541_HW_ID				0x0014

#define BT541_DATA_VERSION_REG			0x0013
#define BT541_SUPPORTED_FINGER_NUM		0x0015
#define BT541_EEPROM_INFO			0x0018
#define BT541_INITIAL_TOUCH_MODE		0x0019

#define BT541_TOTAL_NUMBER_OF_X			0x0060
#define BT541_TOTAL_NUMBER_OF_Y			0x0061

#define BT541_DELAY_RAW_FOR_HOST		0x007f

#define BT541_BUTTON_SUPPORTED_NUM		0x00B0
#define BT541_BUTTON_SENSITIVITY		0x00B2
#define BT541_DUMMY_BUTTON_SENSITIVITY		0X00C8
#define BT541_BTN_WIDTH				0x016d
#define BT541_REAL_WIDTH			0x01c0


#define BT541_X_RESOLUTION			0x00C0
#define BT541_Y_RESOLUTION			0x00C1

#define BT541_POINT_STATUS_REG			0x0080
#define BT541_ICON_STATUS_REG			0x00AA

#define BT541_AFE_FREQUENCY			0x0100
#define BT541_DND_N_COUNT			0x0122
#define BT541_DND_U_COUNT			0x0135
#define BT541_SHIFT_VALUE			0x012B

#define BT541_RAWDATA_REG			0x0200

#define BT541_EEPROM_INFO_REG			0x0018

#define BT541_INT_ENABLE_FLAG			0x00f0
#define BT541_PERIODICAL_INTERRUPT_INTERVAL	0x00f1

#define BT541_CHECKSUM_RESULT			0x012c

#define BT541_INIT_FLASH			0x01d0
#define BT541_WRITE_FLASH			0x01d1
#define BT541_READ_FLASH			0x01d2

#define ZINITIX_INTERNAL_FLAG_02		0x011e

#define BT541_OPTIONAL_SETTING			0x0116

/* Interrupt & status register flag bit
   -------------------------------------------------
 */
#define BIT_PT_CNT_CHANGE			0
#define BIT_DOWN				1
#define BIT_MOVE				2
#define BIT_UP					3
#define BIT_PALM				4
#define BIT_PALM_REJECT				5
#define RESERVED_0				6
#define RESERVED_1				7
#define BIT_WEIGHT_CHANGE			8
#define BIT_PT_NO_CHANGE			9
#define BIT_REJECT				10
#define BIT_PT_EXIST				11
#define RESERVED_2				12
#define BIT_MUST_ZERO				13
#define BIT_DEBUG				14
#define BIT_ICON_EVENT				15

/* button */
#define BIT_O_ICON0_DOWN			0
#define BIT_O_ICON1_DOWN			1
#define BIT_O_ICON2_DOWN			2
#define BIT_O_ICON3_DOWN			3
#define BIT_O_ICON4_DOWN			4
#define BIT_O_ICON5_DOWN			5
#define BIT_O_ICON6_DOWN			6
#define BIT_O_ICON7_DOWN			7

#define BIT_O_ICON0_UP				8
#define BIT_O_ICON1_UP				9
#define BIT_O_ICON2_UP				10
#define BIT_O_ICON3_UP				11
#define BIT_O_ICON4_UP				12
#define BIT_O_ICON5_UP				13
#define BIT_O_ICON6_UP				14
#define BIT_O_ICON7_UP				15


#define SUB_BIT_EXIST				0
#define SUB_BIT_DOWN				1
#define SUB_BIT_MOVE				2
#define SUB_BIT_UP				3
#define SUB_BIT_UPDATE				4
#define SUB_BIT_WAIT				5


#define zinitix_bit_set(val, n)		((val) &= ~(1<<(n)), (val) |= (1<<(n)))
#define zinitix_bit_clr(val, n)		((val) &= ~(1<<(n)))
#define zinitix_bit_test(val, n)	((val) & (1<<(n)))
#define zinitix_swap_v(a, b, t)		((t) = (a), (a) = (b), (b) = (t))
#define zinitix_swap_16(s)		(((((s) & 0xff) << 8) | (((s) >> 8) & 0xff)))

/* end header file */

#ifdef SEC_FACTORY_TEST
/* Touch Screen */
#define TSP_CMD_STR_LEN				32
#define TSP_CMD_RESULT_STR_LEN			512
#define TSP_CMD_PARAM_NUM			8
#define TSP_CMD_X_NUM				19
#define TSP_CMD_Y_NUM				10
#define TSP_CMD_NODE_NUM			(TSP_CMD_X_NUM * TSP_CMD_Y_NUM)

struct tsp_factory_info {
	struct list_head cmd_list_head;
	char cmd[TSP_CMD_STR_LEN];
	char cmd_param[TSP_CMD_PARAM_NUM];
	char cmd_result[TSP_CMD_RESULT_STR_LEN];
	char cmd_buff[TSP_CMD_RESULT_STR_LEN];
	struct mutex cmd_lock;
	bool cmd_is_running;
	u8 cmd_state;
};

struct tsp_raw_data {
	s16 dnd_data[TSP_CMD_NODE_NUM];
	s16 hfdnd_data[TSP_CMD_NODE_NUM];
	s32 hfdnd_data_sum[TSP_CMD_NODE_NUM];
	s16 delta_data[TSP_CMD_NODE_NUM];
	s16 vgap_data[TSP_CMD_NODE_NUM];
	s16 hgap_data[TSP_CMD_NODE_NUM];
};

enum {
	WAITING = 0,
	RUNNING,
	OK,
	FAIL,
	NOT_APPLICABLE,
};

struct tsp_cmd {
	struct list_head list;
	const char *cmd_name;
	void (*cmd_func)(void *device_data);
};

static void fw_update(void *device_data);
static void get_fw_ver_bin(void *device_data);
static void get_fw_ver_ic(void *device_data);
static void module_off_master(void *device_data);
static void module_on_master(void *device_data);
static void module_off_slave(void *device_data);
static void module_on_slave(void *device_data);
static void get_chip_vendor(void *device_data);
static void get_chip_name(void *device_data);
static void get_threshold(void *device_data);
static void get_x_num(void *device_data);
static void get_y_num(void *device_data);
static void not_support_cmd(void *device_data);

/* Vendor dependant command */
#ifdef SUPPORTED_USE_DUAL_FW
static void get_module_vendor(void *device_data);
#endif
static void get_reference(void *device_data);
static void get_dnd(void * device_data);
static void get_hfdnd(void * device_data);
static void get_dnd_v_gap(void * device_data);
static void get_dnd_h_gap(void * device_data);
static void get_hfdnd_v_gap(void * device_data);
static void get_hfdnd_h_gap(void * device_data);
static void get_delta(void *device_data);
static void run_reference_read (void *device_data);
static void run_dnd_read(void *device_data);
static void run_hfdnd_read(void *device_data);
static void run_dnd_v_gap_read(void *device_data);
static void run_dnd_h_gap_read(void * device_data);
static void run_hfdnd_v_gap_read(void *device_data);
static void run_hfdnd_h_gap_read(void * device_data);
static void run_delta_read(void *device_data);
static void hfdnd_spec_adjust(void *device_data);
static void clear_reference_data(void *device_data);
static void run_ref_calibration(void *device_data);
#ifdef CONFIG_INPUT_BOOSTER
static void boost_level(void *device_data);
#endif
static void dead_zone_enable(void *device_data);

#define TSP_CMD(name, func) .cmd_name = name, .cmd_func = func
static struct tsp_cmd tsp_cmds[] = {
	{TSP_CMD("fw_update", fw_update),},
	{TSP_CMD("get_fw_ver_bin", get_fw_ver_bin),},
	{TSP_CMD("get_fw_ver_ic", get_fw_ver_ic),},
	{TSP_CMD("get_threshold", get_threshold),},
	{TSP_CMD("module_off_master", module_off_master),},
	{TSP_CMD("module_on_master", module_on_master),},
	{TSP_CMD("module_off_slave", module_off_slave),},
	{TSP_CMD("module_on_slave", module_on_slave),},
	{TSP_CMD("get_chip_vendor", get_chip_vendor),},
	{TSP_CMD("get_chip_name", get_chip_name),},
	{TSP_CMD("get_x_num", get_x_num),},
	{TSP_CMD("get_y_num", get_y_num),},
	{TSP_CMD("not_support_cmd", not_support_cmd),},

	/* vendor dependant command */
#ifdef SUPPORTED_USE_DUAL_FW
	{TSP_CMD("get_module_vendor", get_module_vendor),},
#endif
	{TSP_CMD("run_reference_read", run_reference_read),},
	{TSP_CMD("get_dnd_all_data", get_reference),},
	{TSP_CMD("run_delta_read", run_delta_read),},
	{TSP_CMD("get_delta_all_data", get_delta),},
	{TSP_CMD("run_dnd_read", run_dnd_read),},
	{TSP_CMD("get_dnd", get_dnd),},
	{TSP_CMD("run_dnd_v_gap_read", run_dnd_v_gap_read),},
	{TSP_CMD("get_dnd_v_gap", get_dnd_v_gap),},
	{TSP_CMD("run_dnd_h_gap_read", run_dnd_h_gap_read),},
	{TSP_CMD("get_dnd_h_gap", get_dnd_h_gap),},
	{TSP_CMD("run_hfdnd_read", run_hfdnd_read),},
	{TSP_CMD("get_hfdnd", get_hfdnd),},
	{TSP_CMD("run_hfdnd_v_gap_read", run_hfdnd_v_gap_read),},
	{TSP_CMD("get_hfdnd_v_gap", get_hfdnd_v_gap),},
	{TSP_CMD("run_hfdnd_h_gap_read", run_hfdnd_h_gap_read),},
	{TSP_CMD("get_hfdnd_h_gap", get_hfdnd_h_gap),},
	{TSP_CMD("hfdnd_spec_adjust", hfdnd_spec_adjust),},
	{TSP_CMD("clear_reference_data", clear_reference_data),},
	{TSP_CMD("run_ref_calibration", run_ref_calibration),},
#ifdef CONFIG_INPUT_BOOSTER
	{TSP_CMD("boost_level", boost_level),},
#endif
	{TSP_CMD("dead_zone_enable", dead_zone_enable),},
};
#endif

#define TSP_NORMAL_EVENT_MSG			1
static int m_ts_debug_mode = ZINITIX_DEBUG;

#if ESD_TIMER_INTERVAL
static struct workqueue_struct *esd_tmr_workqueue;
#endif

struct coord {
	u16	x;
	u16	y;
	u8	width;
	u8	sub_status;
#if (TOUCH_POINT_MODE == 2)
	u8	minor_width;
	u8	angle;
#endif
};

struct point_info {
	u16	status;
#if (TOUCH_POINT_MODE == 1)
	u16	event_flag;
#else
	u8	finger_cnt;
	u8	time_stamp;
#endif
	struct coord coord[MAX_SUPPORTED_FINGER_NUM];
};

#define TOUCH_V_FLIP				0x01
#define TOUCH_H_FLIP				0x02
#define TOUCH_XY_SWAP				0x04

/*Test Mode (Monitoring Raw Data) */
/*---------------------------------------------------------------------*/
/* GF1 */
/*---------------------------------------------------------------------*/
#define TSP_INIT_TEST_RATIO			100

#define	SEC_DND_N_COUNT				14
#define	SEC_DND_U_COUNT				16
#define	SEC_DND_FREQUENCY			219 /*300khz*/

#define	SEC_HFDND_N_COUNT			14
#define	SEC_HFDND_U_COUNT			16
#define	SEC_HFDND_FREQUENCY			55 /*300khz*/
/*---------------------------------------------------------------------*/
struct capa_info {
	u16	vendor_id;
	u16	ic_revision;
	u16	fw_version;
	u16	fw_minor_version;
	u16	reg_data_version;
	u16	threshold;
	u16	key_threshold;
	u16	dummy_threshold;
	u32	ic_fw_size;
	u32	MaxX;
	u32	MaxY;
	u32	MinX;
	u32	MinY;
	u8	gesture_support;
	u16	multi_fingers;
	u16	button_num;
	u16	ic_int_mask;
	u16	x_node_num;
	u16	y_node_num;
	u16	total_node_num;
	u16	hw_id;
	u16	module_id;
	u16	afe_frequency;
	u16	shift_value;
	u16	N_cnt;
	u16	U_cnt;
	u16	i2s_checksum;
};

enum work_state {
	NOTHING = 0,
	NORMAL,
	ESD_TIMER,
	EALRY_SUSPEND,
	SUSPEND,
	RESUME,
	LATE_RESUME,
	UPGRADE,
	REMOVE,
	SET_MODE,
	HW_CALIBRAION,
	RAW_DATA,
	PROBE,
};

enum {
	BUILT_IN = 0,
	UMS,
	REQ_FW,
};

struct bt541_ts_info;

struct bt541_ts_platform_data {
	u32	gpio_int;
	u32	gpio_ldo_en;
	int	(*tsp_power)(struct bt541_ts_info *info, int on);
	u32	x_resolution;
	u32	y_resolution;
	u32	page_size;
	u32	orientation;
	u32	tsp_supply_type;
};

struct bt541_ts_info {
	struct i2c_client			*client;
	struct input_dev			*input_dev;
	struct bt541_ts_platform_data		*pdata;
	char					phys[32];
	struct capa_info			cap_info;
	struct point_info			touch_info;
	struct point_info			reported_touch_info;
	u16					icon_event_reg;
	u16					prev_icon_event;
	int					irq;
	u8					button[MAX_SUPPORTED_BUTTON_NUM];
	u8					work_state;
	struct semaphore			work_lock;
#if ESD_TIMER_INTERVAL
	struct work_struct			tmr_work;
	struct timer_list			esd_timeout_tmr;
	struct timer_list			*p_esd_timeout_tmr;
	spinlock_t				lock;
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend			early_suspend;
#endif
	struct semaphore			raw_data_lock;
	u16					touch_mode;
	s16					cur_data[MAX_TRAW_DATA_SZ];
	u8					update;
#ifdef SEC_FACTORY_TEST
	struct tsp_factory_info			*factory_info;
	struct tsp_raw_data			*raw_data;

	s16 Gap_max_x;
	s16 Gap_max_y;
	s16 Gap_max_val;
	s16 Gap_min_x;
	s16 Gap_min_y;
	s16 Gap_min_val;
	s16 Gap_Gap_val;
	s16 Gap_node_num;
#endif
#if TOUCH_BOOSTER || defined(CONFIG_INPUT_BOOSTER)
	u8 finger_cnt;
#endif
};
/* Dummy touchkey code */
#define KEY_DUMMY_HOME1				249
#define KEY_DUMMY_HOME2				250
#define KEY_DUMMY_MENU				251
#define KEY_DUMMY_HOME				252
#define KEY_DUMMY_BACK				253
/*<= you must set key button mapping*/
#ifdef NOT_SUPPORTED_TOUCH_DUMMY_KEY
u32 BUTTON_MAPPING_KEY[MAX_SUPPORTED_BUTTON_NUM] = {
	KEY_RECENT, KEY_BACK};
#else
u32 BUTTON_MAPPING_KEY[MAX_SUPPORTED_BUTTON_NUM] = {
	KEY_DUMMY_MENU, KEY_RECENT,// KEY_DUMMY_HOME1,
	/*KEY_DUMMY_HOME2,*/ KEY_BACK, KEY_DUMMY_BACK};
#endif


static int cal_mode;
static int get_boot_mode(char *str)
{
	get_option(&str, &cal_mode);
	printk("get_boot_mode, uart_mode : %d\n", cal_mode);
	return 1;
}
__setup("calmode=",get_boot_mode);
/* define i2c sub functions*/
static inline s32 read_data(struct i2c_client *client,
		u16 reg, u8 *values, u16 length)
{
	int ret = 0;
	int count = 0;
retry:
	/* select register*/
	ret = i2c_master_send(client , (u8 *)&reg , 2);
	if (ret < 0) {
		mdelay(1);

		if (++count < 8)
			goto retry;

		return ret;
	}
	/* for setup tx transaction. */
	udelay(DELAY_FOR_TRANSCATION);
	ret = i2c_master_recv(client , values , length);
	if (ret < 0)
		return ret;

	udelay(DELAY_FOR_POST_TRANSCATION);
	return length;
}

static inline s32 write_data(struct i2c_client *client,
		u16 reg, u8 *values, u16 length)
{
	int ret = 0;
	int count = 0;
	u8 pkt[10]; /* max packet */
	pkt[0] = (reg) & 0xff; /* reg addr */
	pkt[1] = (reg >> 8)&0xff;
	memcpy((u8 *)&pkt[2], values, length);

retry:
	ret = i2c_master_send(client , pkt , length + 2);
	if (ret < 0) {
		mdelay(1);

		if (++count < 8)
			goto retry;
		printk(KERN_INFO "%s, count = %d\n", __func__, count);

		return ret;
	}

	udelay(DELAY_FOR_POST_TRANSCATION);
	return length;
}

static inline s32 write_reg(struct i2c_client *client, u16 reg, u16 value)
{
	if (write_data(client, reg, (u8 *)&value, 2) < 0)
		return I2C_FAIL;

	return I2C_SUCCESS;
}

static inline s32 write_cmd(struct i2c_client *client, u16 reg)
{
	int ret = 0;
	int count = 0;

retry:
	ret = i2c_master_send(client , (u8 *)&reg , 2);
	if (ret < 0) {
		mdelay(1);

		if (++count < 8)
			goto retry;

		return ret;
	}

	udelay(DELAY_FOR_POST_TRANSCATION);
	return I2C_SUCCESS;
}

static inline s32 read_raw_data(struct i2c_client *client,
		u16 reg, u8 *values, u16 length)
{
	int ret = 0;
	int count = 0;

retry:
	/* select register */
	ret = i2c_master_send(client , (u8 *)&reg , 2);
	if (ret < 0) {
		mdelay(1);

		if (++count < 8)
			goto retry;

		return ret;
	}

	/* for setup tx transaction. */
	udelay(200);

	ret = i2c_master_recv(client , values , length);
	if (ret < 0)
		return ret;

	udelay(DELAY_FOR_POST_TRANSCATION);
	return length;
}

static inline s32 read_firmware_data(struct i2c_client *client,
		u16 addr, u8 *values, u16 length)
{
	int ret = 0;
	/* select register*/

	ret = i2c_master_send(client , (u8 *)&addr , 2);
	if (ret < 0)
		return ret;

	/* for setup tx transaction. */
	mdelay(1);

	ret = i2c_master_recv(client , values , length);
	if (ret < 0)
		return ret;

	udelay(DELAY_FOR_POST_TRANSCATION);
	return length;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bt541_ts_early_suspend(struct early_suspend *h);
static void bt541_ts_late_resume(struct early_suspend *h);
#endif

static bool bt541_power_control(struct bt541_ts_info *info, u8 ctl);
static int bt541_power(struct bt541_ts_info *info, int on);

static bool init_touch(struct bt541_ts_info *info, bool fw_oneshot_upgrade);
static bool mini_init_touch(struct bt541_ts_info *info);
static void clear_report_data(struct bt541_ts_info *info);
#if ESD_TIMER_INTERVAL
static void esd_timer_start(u16 sec, struct bt541_ts_info *info);
static void esd_timer_stop(struct bt541_ts_info *info);
static void esd_timer_init(struct bt541_ts_info *info);
static void esd_timeout_handler(unsigned long data);
#endif

static long ts_misc_fops_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg);
static int ts_misc_fops_open(struct inode *inode, struct file *filp);
static int ts_misc_fops_close(struct inode *inode, struct file *filp);

static const struct file_operations ts_misc_fops = {
	.owner = THIS_MODULE,
	.open = ts_misc_fops_open,
	.release = ts_misc_fops_close,
	.unlocked_ioctl = ts_misc_fops_ioctl,
	.compat_ioctl = ts_misc_fops_ioctl,
};

static struct miscdevice touch_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zinitix_touch_misc",
	.fops = &ts_misc_fops,
};

#define TOUCH_IOCTL_BASE			0xbc
#define TOUCH_IOCTL_GET_DEBUGMSG_STATE		_IOW(TOUCH_IOCTL_BASE, 0, int)
#define TOUCH_IOCTL_SET_DEBUGMSG_STATE		_IOW(TOUCH_IOCTL_BASE, 1, int)
#define TOUCH_IOCTL_GET_CHIP_REVISION		_IOW(TOUCH_IOCTL_BASE, 2, int)
#define TOUCH_IOCTL_GET_FW_VERSION		_IOW(TOUCH_IOCTL_BASE, 3, int)
#define TOUCH_IOCTL_GET_REG_DATA_VERSION	_IOW(TOUCH_IOCTL_BASE, 4, int)
#define TOUCH_IOCTL_VARIFY_UPGRADE_SIZE		_IOW(TOUCH_IOCTL_BASE, 5, int)
#define TOUCH_IOCTL_VARIFY_UPGRADE_DATA		_IOW(TOUCH_IOCTL_BASE, 6, int)
#define TOUCH_IOCTL_START_UPGRADE		_IOW(TOUCH_IOCTL_BASE, 7, int)
#define TOUCH_IOCTL_GET_X_NODE_NUM		_IOW(TOUCH_IOCTL_BASE, 8, int)
#define TOUCH_IOCTL_GET_Y_NODE_NUM		_IOW(TOUCH_IOCTL_BASE, 9, int)
#define TOUCH_IOCTL_GET_TOTAL_NODE_NUM		_IOW(TOUCH_IOCTL_BASE, 10, int)
#define TOUCH_IOCTL_SET_RAW_DATA_MODE		_IOW(TOUCH_IOCTL_BASE, 11, int)
#define TOUCH_IOCTL_GET_RAW_DATA		_IOW(TOUCH_IOCTL_BASE, 12, int)
#define TOUCH_IOCTL_GET_X_RESOLUTION		_IOW(TOUCH_IOCTL_BASE, 13, int)
#define TOUCH_IOCTL_GET_Y_RESOLUTION		_IOW(TOUCH_IOCTL_BASE, 14, int)
#define TOUCH_IOCTL_HW_CALIBRAION		_IOW(TOUCH_IOCTL_BASE, 15, int)
#define TOUCH_IOCTL_GET_REG			_IOW(TOUCH_IOCTL_BASE, 16, int)
#define TOUCH_IOCTL_SET_REG			_IOW(TOUCH_IOCTL_BASE, 17, int)
#define TOUCH_IOCTL_SEND_SAVE_STATUS		_IOW(TOUCH_IOCTL_BASE, 18, int)
#define TOUCH_IOCTL_DONOT_TOUCH_EVENT		_IOW(TOUCH_IOCTL_BASE, 19, int)

struct bt541_ts_info *misc_info;

#ifdef SUPPORTED_USE_DUAL_FW
static void bt541_firmware_check(void)
{
	int i;
	u16 newHWID;

	for (i = 0; i < TSP_TYPE_COUNT; i++) {
		newHWID = (u16) (m_pFirmware[i][0x57d2] | (m_pFirmware[i][0x57d3]<<8));
		if (misc_info->cap_info.hw_id == newHWID) {
			m_FirmwareIdx = i;
			break;
		}
	}
	if (i == TSP_TYPE_COUNT)
		m_FirmwareIdx = 0;

	m_firmware_data = m_pFirmware[m_FirmwareIdx];
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&misc_info->client->dev, "touch tsp type = %d,  hw_id = %d\n", m_FirmwareIdx, misc_info->cap_info.hw_id);
#endif
}
#endif

static u16 m_optional_mode = 0;
static u16 m_prev_optional_mode = 0;
static void bt541_set_optional_mode(struct bt541_ts_info *info, bool force)
{
	if (m_prev_optional_mode == m_optional_mode && !force)
		return;

	if (write_reg(info->client, BT541_OPTIONAL_SETTING, m_optional_mode) == I2C_SUCCESS) {
		m_prev_optional_mode = m_optional_mode;
		dev_info(&misc_info->client->dev, "TA setting changed to %d\n",
				m_optional_mode&0x1);
	}
}
static void bt541_set_ta_status(struct bt541_ts_info *info)
{
	if (ta_connected)
		zinitix_bit_set(m_optional_mode, 0);
	else
		zinitix_bit_clr(m_optional_mode, 0);
}

void tsp_charger_enable(int status)
{
#if !defined(CONFIG_EXTCON_SM5504)
	if (status == POWER_SUPPLY_TYPE_BATTERY ||
			status == POWER_SUPPLY_TYPE_UNKNOWN)
		status = 0;
	else
		status = 1;
#endif
	if (status)
		ta_connected = true;
	else
		ta_connected = false;

	bt541_set_ta_status(misc_info);

	dev_info(&misc_info->client->dev, "TA %s\n",
			status ? "connected" : "disconnected");
}
EXPORT_SYMBOL(tsp_charger_enable);

static bool get_raw_data(struct bt541_ts_info *info, u8 *buff, int skip_cnt)
{
	struct i2c_client *client = info->client;
	struct bt541_ts_platform_data *pdata = info->pdata;
	u32 total_node = info->cap_info.total_node_num;
	u32 sz;
	int i;

	disable_irq(info->irq);

	down(&info->work_lock);
	if (info->work_state != NOTHING) {
		printk(KERN_INFO "other process occupied.. (%d)\n", info->work_state);
		enable_irq(info->irq);
		up(&info->work_lock);
		return false;
	}

	info->work_state = RAW_DATA;

	for (i = 0; i < skip_cnt; i++) {
		while (gpio_get_value(pdata->gpio_int))
			msleep(1);

		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
		msleep(1);
	}

	zinitix_debug_msg("read raw data\r\n");
	sz = total_node * 2;

	while (gpio_get_value(pdata->gpio_int))
		msleep(1);

	if (read_raw_data(client, BT541_RAWDATA_REG, (char *)buff, sz) < 0) {
		zinitix_printk("error : read zinitix tc raw data\n");
		info->work_state = NOTHING;
		enable_irq(info->irq);
		up(&info->work_lock);
		return false;
	}

	write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
	info->work_state = NOTHING;
	enable_irq(info->irq);
	up(&info->work_lock);

	return true;
}

static bool ts_get_raw_data(struct bt541_ts_info *info)
{
	struct i2c_client *client = info->client;
	u32 total_node = info->cap_info.total_node_num;
	u32 sz;

	if (down_trylock(&info->raw_data_lock)) {
		dev_err(&client->dev, "Failed to occupy sema\n");
		info->touch_info.status = 0;
		return true;
	}

	sz = total_node * 2 + sizeof(struct point_info);

	if (read_raw_data(info->client, BT541_RAWDATA_REG,
				(char *)info->cur_data, sz) < 0) {
		dev_err(&client->dev, "Failed to read raw data\n");
		up(&info->raw_data_lock);
		return false;
	}

	info->update = 1;
	memcpy((u8 *)(&info->touch_info),
			(u8 *)&info->cur_data[total_node],
			sizeof(struct point_info));
	up(&info->raw_data_lock);

	return true;
}

#if ZINITIX_I2C_CHECKSUM
#define ZINITIX_I2C_CHECKSUM_WCNT		0x016a
#define ZINITIX_I2C_CHECKSUM_RESULT		0x016c
static bool i2c_checksum(struct bt541_ts_info *info, s16 *pChecksum, u16 wlength)
{
	s16 checksum_result;
	s16 checksum_cur;
	int i;

	checksum_cur = 0;
	for (i = 0; i < wlength; i++) {
		checksum_cur += (s16)pChecksum[i];
	}
	if (read_data(info->client,
				ZINITIX_I2C_CHECKSUM_RESULT,
				(u8 *)(&checksum_result), 2) < 0) {
		printk(KERN_INFO "error read i2c checksum rsult.-\n");
		return false;
	}
	if (checksum_cur != checksum_result) {
		printk(KERN_INFO "checksum error : %d, %d\n", checksum_cur, checksum_result);
		return false;
	}
	return true;
}

#endif

static bool ts_read_coord(struct bt541_ts_info *info)
{
	struct i2c_client *client = info->client;
#if (TOUCH_POINT_MODE == 1)
	int i;
#endif

	/* for  Debugging Tool */

	if (info->touch_mode != TOUCH_POINT_MODE) {
		if (info->update == 0) {
			if (ts_get_raw_data(info) == false)
				return false;
		} else {
			info->touch_info.status = 0;
		}

		dev_err(&client->dev, "status = 0x%04X\n", info->touch_info.status);

		goto out;
	}

#if (TOUCH_POINT_MODE == 1)
	memset(&info->touch_info,
			0x0, sizeof(struct point_info));

#if ZINITIX_I2C_CHECKSUM
	if (info->cap_info.i2s_checksum)
		if ((write_reg(info->client, ZINITIX_I2C_CHECKSUM_WCNT, 2)) != I2C_SUCCESS)
			return false;

#endif
	if (read_data(info->client, BT541_POINT_STATUS_REG, (u8 *)(&info->touch_info), 4) < 0) {
		dev_err(&client->dev, "%s: Failed to read point info\n", __func__);

		return false;
	}

#if ZINITIX_I2C_CHECKSUM
	if (info->cap_info.i2s_checksum)
		if (i2c_checksum(info, (s16 *)(&info->touch_info), 2) == false)
		return false;
#endif
	dev_info(&client->dev, "status reg = 0x%x , event_flag = 0x%04x\n",
			info->touch_info.status, info->touch_info.event_flag);

	bt541_set_optional_mode(info, false);
	if (info->touch_info.event_flag == 0)
		goto out;

#if ZINITIX_I2C_CHECKSUM
	if (info->cap_info.i2s_checksum)
		if ((write_reg(info->client, ZINITIX_I2C_CHECKSUM_WCNT, sizeof(struct point_info)/2)) != I2C_SUCCESS)
			return false;
#endif
	for (i = 0; i < info->cap_info.multi_fingers; i++) {
		if (zinitix_bit_test(info->touch_info.event_flag, i)) {
			udelay(20);

			if (read_data(info->client, BT541_POINT_STATUS_REG + 2 + ( i * 4),
						(u8 *)(&info->touch_info.coord[i]),
						sizeof(struct coord)) < 0) {
				dev_err(&client->dev, "Failed to read point info\n");

				return false;
			}
#if ZINITIX_I2C_CHECKSUM
			if (info->cap_info.i2s_checksum)
				if (i2c_checksum(info, (s16 *)(&info->touch_info.coord[i]), sizeof(struct point_info)/2) == false)
					return false;
#endif
		}
	}

#else
#if ZINITIX_I2C_CHECKSUM
	if (info->cap_info.i2s_checksum)
		if (write_reg(info->client,
					ZINITIX_I2C_CHECKSUM_WCNT,
					(sizeof(struct point_info)/2)) != I2C_SUCCESS) {
			dev_err(&client->dev, "error write checksum wcnt.-\n");
			return false;
		}
#endif
	if (read_data(info->client, BT541_POINT_STATUS_REG,
				(u8 *)(&info->touch_info), sizeof(struct point_info)) < 0) {
		dev_err(&client->dev, "Failed to read point info\n");

		return false;
	}
#if ZINITIX_I2C_CHECKSUM
	if (info->cap_info.i2s_checksum)
		if (i2c_checksum(info, (s16 *)(&info->touch_info), sizeof(struct point_info)/2) == false)
			return false;
#endif

	bt541_set_optional_mode(info, false);

#endif

out:
	/* error */
	if (zinitix_bit_test(info->touch_info.status, BIT_MUST_ZERO)) {
		dev_err(&client->dev, "Invalid must zero bit(%04x)\n",
				info->touch_info.status);

		return false;
	}

	write_cmd(info->client, BT541_CLEAR_INT_STATUS_CMD);

	return true;
}

#if ESD_TIMER_INTERVAL
static void esd_timeout_handler(unsigned long data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)data;

	info->p_esd_timeout_tmr = NULL;
	queue_work(esd_tmr_workqueue, &info->tmr_work);
}

static void esd_timer_start(u16 sec, struct bt541_ts_info *info)
{
	unsigned long flags;
	spin_lock_irqsave(&info->lock,flags);
	if (info->p_esd_timeout_tmr != NULL)
#ifdef CONFIG_SMP
		del_singleshot_timer_sync(info->p_esd_timeout_tmr);
#else
	del_timer(info->p_esd_timeout_tmr);
#endif
	info->p_esd_timeout_tmr = NULL;
	init_timer(&(info->esd_timeout_tmr));
	info->esd_timeout_tmr.data = (unsigned long)(info);
	info->esd_timeout_tmr.function = esd_timeout_handler;
	info->esd_timeout_tmr.expires = jiffies + (HZ * sec);
	info->p_esd_timeout_tmr = &info->esd_timeout_tmr;
	add_timer(&info->esd_timeout_tmr);
	spin_unlock_irqrestore(&info->lock, flags);
}

static void esd_timer_stop(struct bt541_ts_info *info)
{
	unsigned long flags;
	spin_lock_irqsave(&info->lock,flags);
	if (info->p_esd_timeout_tmr)
#ifdef CONFIG_SMP
		del_singleshot_timer_sync(info->p_esd_timeout_tmr);
#else
	del_timer(info->p_esd_timeout_tmr);
#endif

	info->p_esd_timeout_tmr = NULL;
	spin_unlock_irqrestore(&info->lock, flags);
}

static void esd_timer_init(struct bt541_ts_info *info)
{
	unsigned long flags;
	spin_lock_irqsave(&info->lock,flags);
	init_timer(&(info->esd_timeout_tmr));
	info->esd_timeout_tmr.data = (unsigned long)(info);
	info->esd_timeout_tmr.function = esd_timeout_handler;
	info->p_esd_timeout_tmr = NULL;
	spin_unlock_irqrestore(&info->lock, flags);
}

static void ts_tmr_work(struct work_struct *work)
{
	struct bt541_ts_info *info =
		container_of(work, struct bt541_ts_info, tmr_work);
	struct i2c_client *client = info->client;

#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "tmr queue work ++\n");
#endif

	if (down_trylock(&info->work_lock)) {
		dev_err(&client->dev, "%s: Failed to occupy work lock\n", __func__);
		esd_timer_start(CHECK_ESD_TIMER, info);

		return;
	}

	if (info->work_state != NOTHING) {
		dev_info(&client->dev, "%s: Other process occupied (%d)\n",
				__func__, info->work_state);
		up(&info->work_lock);

		return;
	}
	info->work_state = ESD_TIMER;

	disable_irq(info->irq);
	bt541_power_control(info, POWER_OFF);
	bt541_power_control(info, POWER_ON_SEQUENCE);

	clear_report_data(info);
	if (mini_init_touch(info) == false)
		goto fail_time_out_init;

	info->work_state = NOTHING;
	enable_irq(info->irq);
	up(&info->work_lock);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "tmr queue work--\n");
#endif

	return;
fail_time_out_init:
	dev_err(&client->dev, "%s: Failed to restart\n", __func__);
	esd_timer_start(CHECK_ESD_TIMER, info);
	info->work_state = NOTHING;
	enable_irq(info->irq);
	up(&info->work_lock);

	return;
}
#endif

static bool bt541_power_sequence(struct bt541_ts_info *info)
{
	struct i2c_client *client = info->client;
	int retry = 0;
	u16 chip_code;

retry_power_sequence:
	if (write_reg(client, 0xc000, 0x0001) != I2C_SUCCESS) {
		dev_err(&client->dev, "Failed to send power sequence(vendor cmd enable)\n");
		goto fail_power_sequence;
	}
	udelay(10);

	if (read_data(client, 0xcc00, (u8 *)&chip_code, 2) < 0) {
		dev_err(&client->dev, "Failed to read chip code\n");
		goto fail_power_sequence;
	}

	dev_info(&client->dev, "%s: chip code = 0x%x\n", __func__, chip_code);
	udelay(10);

	if (write_cmd(client, 0xc004) != I2C_SUCCESS) {
		dev_err(&client->dev, "Failed to send power sequence(intn clear)\n");
		goto fail_power_sequence;
	}
	udelay(10);

	if (write_reg(client, 0xc002, 0x0001) != I2C_SUCCESS) {
		dev_err(&client->dev, "Failed to send power sequence(nvm init)\n");
		goto fail_power_sequence;
	}
	mdelay(2);

	if (write_reg(client, 0xc001, 0x0001) != I2C_SUCCESS) {
		dev_err(&client->dev, "Failed to send power sequence(program start)\n");
		goto fail_power_sequence;
	}
	msleep(FIRMWARE_ON_DELAY);	/* wait for checksum cal */

	return true;

fail_power_sequence:
	if (retry++ < 3) {
		msleep(CHIP_ON_DELAY);
		dev_info(&client->dev, "retry = %d\n", retry);
		goto retry_power_sequence;
	}

	dev_err(&client->dev, "Failed to send power sequence\n");
	return false;
}

static struct regulator *touch_regulator;
static int bt541_power(struct bt541_ts_info *info, int on)
{
	struct i2c_client *client = info->client;
	static u8 is_power_on;
	int ret = 0;

	if (touch_regulator == NULL) {
		touch_regulator = regulator_get(&info->client->dev, "avdd");
		if (IS_ERR(touch_regulator)) {
			touch_regulator = NULL;
			dev_info(&client->dev, "%s: get touch_regulator error\n",
					__func__);
			return -EIO;
		}

		ret = regulator_set_voltage(touch_regulator, 3100000, 3100000);
		if (ret < 0)
			dev_info(&client->dev, "%s: set voltage error(%d)\n", __func__, ret);
	}

	if (on == 0) {
		if (is_power_on) {
			is_power_on = 0;

			ret = regulator_disable(touch_regulator);
			if (ret) {
				is_power_on = 1;
				dev_err(&client->dev, "%s: touch_regulator disable " \
						"failed  (%d)\n", __func__, ret);
				return -EIO;
			}

			mdelay(CHIP_OFF_DELAY);
		}
	} else {
		if (!is_power_on) {
			is_power_on = 1;

			ret = regulator_enable(touch_regulator);
			if (ret) {
				is_power_on = 0;
				dev_err(&client->dev, "%s: touch_regulator enable "\
						"failed (%d)\n", __func__, ret);
				return -EIO;
			}
		}
	}

	dev_info(&client->dev, "%s, expected power[%d], actural power[%d]\n",
			__func__, on, is_power_on);
	return 0;
}
static bool bt541_power_control(struct bt541_ts_info *info, u8 ctl)
{
	int ret = 0;

	dev_info(&info->client->dev, "%s, %d\n", __func__, ctl);

	if (info->pdata->tsp_supply_type == TSP_REGULATOR_SUPPLY) {
		ret = info->pdata->tsp_power(info, ctl);
		if (ret)
			return false;

		if (ctl == POWER_ON_SEQUENCE) {
			msleep(CHIP_ON_DELAY);
			return bt541_power_sequence(info);
		}
	}
	else
	{
		if (ctl == POWER_OFF) {
			gpio_direction_output(info->pdata->gpio_ldo_en, 0);
			msleep(CHIP_OFF_DELAY);
		} else {
			gpio_direction_output(info->pdata->gpio_ldo_en, 1);
			msleep(CHIP_ON_DELAY);
			/* zxt power on sequence */
			if (ctl == POWER_ON_SEQUENCE)
				return bt541_power_sequence(info);
		}
	}

	return true;
}

#if TOUCH_ONESHOT_UPGRADE
static bool ts_check_need_upgrade(struct bt541_ts_info *info,
		u16 cur_version, u16 cur_minor_version, u16 cur_reg_version, u16 cur_hw_id)
{
	u16 new_version;
	u16 new_minor_version;
	u16 new_reg_version;
	/*u16 new_chip_code;*/
#if CHECK_HWID
	u16 new_hw_id;
#endif
	new_version = (u16) (m_firmware_data[52] | (m_firmware_data[53]<<8));
	new_minor_version = (u16) (m_firmware_data[56] | (m_firmware_data[57]<<8));
	new_reg_version = (u16) (m_firmware_data[60] | (m_firmware_data[61]<<8));
	/*new_chip_code = (u16) (m_firmware_data[64] | (m_firmware_data[65]<<8));*/

#if CHECK_HWID
	new_hw_id =  (u16)(m_firmware_data[0x7528] | (m_firmware_data[0x7529]<<8));
	dev_info(&info->client->dev, "cur HW_ID = 0x%x, new HW_ID = 0x%x\n",
			cur_hw_id, new_hw_id);
#endif

	dev_info(&info->client->dev, "cur version = 0x%x, new version = 0x%x\n",
			cur_version, new_version);

	dev_info(&info->client->dev, "cur minor version = 0x%x, new minor version = 0x%x\n",
			cur_minor_version, new_minor_version);
	dev_info(&info->client->dev, "cur reg data version = 0x%x, new reg data version = 0x%x\n",
			cur_reg_version, new_reg_version);

	if (cal_mode) {
		dev_info(&info->client->dev, "didn't update TSP F/W!! in CAL MODE\n");
		return false;
	}

	if (cur_version > 0xFF)
		return true;
	if (cur_version < new_version)
		return true;
	else if (cur_version > new_version)
		return false;
#if CHECK_HWID
	if (cur_hw_id != new_hw_id)
		return true;
#endif
	if (cur_minor_version < new_minor_version)
		return true;
	else if (cur_minor_version > new_minor_version)
		return false;
	if (cur_reg_version < new_reg_version)
		return true;

	return false;
}
#endif

#define TC_SECTOR_SZ		8

#ifdef SUPPORTED_USE_DUAL_FW
static void ts_check_hwid_in_fatal_state(struct bt541_ts_info *info)
{
	/*u16 flash_addr;*/
	u8 check_data[80];
	int i;
	u16 hw_id0, hw_id1_1, hw_id1_2;
	int retry = 0;
retry_fatal:
	bt541_power_control(info, POWER_OFF);
	bt541_power_control(info, POWER_ON);
	mdelay(10);
	if (write_reg(info->client, 0xc000, 0x0001) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "power sequence error (vendor cmd enable)\n");
		goto fail_check_hwid;
	}
	udelay(10);
	if (write_cmd(info->client, 0xc004) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "power sequence error (intn clear)\n");
		goto fail_check_hwid;
	}
	udelay(10);
	if (write_reg(info->client, 0xc002, 0x0001) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "power sequence error (nvm init)\n");
		goto fail_check_hwid;
	}
	/*dev_err(&info->client->dev, "init flash\n");*/
	mdelay(5);
	if (write_reg(info->client, 0xc003, 0x0000) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "fail to write nvm vpp on\n");
		goto fail_check_hwid;
	}
	if (write_reg(info->client, 0xc104, 0x0000) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "fail to write nvm wp disable\n");
		goto fail_check_hwid;
	}
	/*dev_err(&info->client->dev, "init flash\n");*/
	if (write_cmd(info->client, BT541_INIT_FLASH) != I2C_SUCCESS) {
		dev_err(&info->client->dev, "failed to init flash\n");
		goto fail_check_hwid;
	}
	/*dev_err(&info->client->dev, "read firmware data\n");*/
	for (i = 0; i < 80; i += TC_SECTOR_SZ) {
		if (read_firmware_data(info->client,
					BT541_READ_FLASH,
					(u8 *)&check_data[i], TC_SECTOR_SZ) < 0) {
			dev_err(&info->client->dev, "error : read zinitix tc firmare\n");
			goto fail_check_hwid;
		}
	}
	hw_id0 = check_data[48] + check_data[49]*256;
	hw_id1_1 = check_data[70];
	hw_id1_2 = check_data[71];

	/*dev_err(&info->client->dev, "eeprom hw id = %d, %d, %d\n", hw_id0, hw_id1_1, hw_id1_2);*/

	if (hw_id1_1 == hw_id1_2 && hw_id0 != hw_id1_1)
		info->cap_info.hw_id = hw_id1_1;
	else
		info->cap_info.hw_id = hw_id0;

	dev_err(&info->client->dev, "hw id = %d\n", info->cap_info.hw_id);
	mdelay(5);
	return;

fail_check_hwid:
	if (retry++ < 3)
		goto retry_fatal;
	dev_err(&info->client->dev, "fail to check hw id\n");
	mdelay(5);
}
#endif

static u8 ts_upgrade_firmware(struct bt541_ts_info *info,
		const u8 *firmware_data, u32 size)
{
	struct i2c_client *client = info->client;
	u16 flash_addr;
	u16 reg_val;
	u8 *verify_data;
	int retry_cnt = 0;
	int i;
	int page_sz = info->pdata->page_size;
	u16 chip_code;

	verify_data = kzalloc(size, GFP_KERNEL);
	if (verify_data == NULL) {
		zinitix_printk(KERN_ERR "cannot alloc verify buffer\n");
		return false;
	}
#ifdef SUPPORTED_USE_DUAL_FW
	if (read_data(client, BT541_MODULE_ID, (u8 *)&cap->module_id, 2) < 0)
		dev_err(&client->dev, "Failed to read Modul ID\n");
#endif
retry_upgrade:
	bt541_power_control(info, POWER_OFF);
	bt541_power_control(info, POWER_ON);
	mdelay(10);

	if (write_reg(client, 0xc000, 0x0001) != I2C_SUCCESS) {
		zinitix_printk("power sequence error (vendor cmd enable)\n");
		goto fail_upgrade;
	}

	udelay(10);

	if (read_data(client, 0xcc00, (u8 *)&chip_code, 2) < 0) {
		zinitix_printk("failed to read chip code\n");
		goto fail_upgrade;
	}

	zinitix_printk("chip code = 0x%x\n", chip_code);
	udelay(10);

	if (write_cmd(client, 0xc004) != I2C_SUCCESS) {
		zinitix_printk("power sequence error (intn clear)\n");
		goto fail_upgrade;
	}

	udelay(10);

	if (write_reg(client, 0xc002, 0x0001) != I2C_SUCCESS) {
		zinitix_printk("power sequence error (nvm init)\n");
		goto fail_upgrade;
	}

	mdelay(5);

	zinitix_printk(KERN_INFO "init flash\n");

	if (write_reg(client, 0xc003, 0x0001) != I2C_SUCCESS) {
		zinitix_printk("failed to write nvm vpp on\n");
		goto fail_upgrade;
	}

	if (write_reg(client, 0xc104, 0x0001) != I2C_SUCCESS) {
		zinitix_printk("failed to write nvm wp disable\n");
		goto fail_upgrade;
	}

	if (write_cmd(client, BT541_INIT_FLASH) != I2C_SUCCESS) {
		zinitix_printk(KERN_INFO "failed to init flash\n");
		goto fail_upgrade;
	}

	zinitix_printk(KERN_INFO "writing firmware data\n");
	for (flash_addr = 0; flash_addr < size; ) {
		for (i = 0; i < page_sz / TC_SECTOR_SZ; i++) {
			/*zinitix_debug_msg("write :addr=%04x, len=%d\n",	flash_addr, TC_SECTOR_SZ);*/
			/*zinitix_printk(KERN_INFO "writing :addr = %04x, len=%d \n", flash_addr, TC_SECTOR_SZ);*/
			if (write_data(client, BT541_WRITE_FLASH,
					(u8 *)&firmware_data[flash_addr], TC_SECTOR_SZ) < 0) {
				zinitix_printk(KERN_INFO"error : write zinitix tc firmare\n");
				goto fail_upgrade;
			}
			flash_addr += TC_SECTOR_SZ;
			udelay(100);

		}

		mdelay(30); /*for fuzing delay*/
	}

	if (write_reg(client, 0xc003, 0x0000) != I2C_SUCCESS) {
		zinitix_printk("nvm write vpp off\n");
		goto fail_upgrade;
	}

	if (write_reg(client, 0xc104, 0x0000) != I2C_SUCCESS) {
		zinitix_printk("nvm wp enable\n");
		goto fail_upgrade;
	}

	zinitix_printk(KERN_INFO "init flash\n");

	if (write_cmd(client, BT541_INIT_FLASH) != I2C_SUCCESS) {
		zinitix_printk(KERN_INFO "failed to init flash\n");
		goto fail_upgrade;
	}

	zinitix_printk(KERN_INFO "read firmware data\n");

	for (flash_addr = 0; flash_addr < size; ) {
		for (i = 0; i < page_sz / TC_SECTOR_SZ; i++) {
			/*zinitix_debug_msg("read :addr=%04x, len=%d\n",flash_addr, TC_SECTOR_SZ);*/
			if (read_firmware_data(client,
						BT541_READ_FLASH,
						(u8 *)&verify_data[flash_addr], TC_SECTOR_SZ) < 0) {
				dev_err(&client->dev, "Failed to read firmare\n");

				goto fail_upgrade;
			}

			flash_addr += TC_SECTOR_SZ;
		}
	}
	/* verify */
	dev_info(&client->dev, "verify firmware data\n");
	if (memcmp((u8 *)&firmware_data[0], (u8 *)&verify_data[0], size) == 0) {
		dev_info(&client->dev, "upgrade finished\n");
		kfree(verify_data);
		bt541_power_control(info, POWER_OFF);
		bt541_power_control(info, POWER_ON_SEQUENCE);

		for (i = 0; i < 5; i++) {
			if (read_data(client, BT541_CHECKSUM_RESULT,
					(u8 *)&reg_val, 2) < 0) {
				mdelay(10);
				continue;
			}
		}

		if (reg_val != 0x55aa) {
			dev_err(&client->dev, "upgrade done, but firmware checksum fail. reg_val = %x\n", reg_val);
			goto fail_upgrade;
		}
#ifdef SUPPORTED_USE_DUAL_FW
		if (read_data(client, BT541_MODULE_ID, (u8 *)&reg_val, 2) < 0)
			dev_err(&client->dev, "Failed to read Modul ID\n");
		else {
			if (cap->module_id != reg_val) {
				write_reg(client, BT541_MODULE_ID, cap->module_id);
				write_reg(client, 0xc003, 0x0001);
				write_reg(client, 0xc104, 0x0001);
				udelay(100);
				if (write_cmd(client, BT541_SAVE_STATUS_CMD) != I2C_SUCCESS)
					goto fail_upgrade;

				mdelay(1000);
				write_reg(client, 0xc003, 0x0000);
				write_reg(client, 0xc104, 0x0000);
			}
		}
#endif
		return true;
	}

fail_upgrade:
	bt541_power_control(info, POWER_OFF);

	if (retry_cnt++ < INIT_RETRY_CNT) {
		dev_err(&client->dev, "upgrade failed : so retry... (%d)\n", retry_cnt);
		goto retry_upgrade;
	}

	if (verify_data != NULL)
		kfree(verify_data);

	dev_info(&client->dev, "Failed to upgrade\n");

	return false;
}

static bool ts_hw_calibration(struct bt541_ts_info *info)
{
	struct i2c_client *client = info->client;
	u16	chip_eeprom_info;
	int time_out = 0;

	if (write_reg(client, BT541_TOUCH_MODE, 0x07) != I2C_SUCCESS)
		return false;
	mdelay(10);
	write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
	mdelay(10);
	write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
	mdelay(50);
	write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
	mdelay(10);

	if (write_cmd(client, BT541_CALIBRATE_CMD) != I2C_SUCCESS)
		return false;

	if (write_cmd(client, BT541_CLEAR_INT_STATUS_CMD) != I2C_SUCCESS)
		return false;

	mdelay(10);
	write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);

	/* wait for h/w calibration*/
	do {
		mdelay(500);
		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);

		if (read_data(client, BT541_EEPROM_INFO_REG,
				(u8 *)&chip_eeprom_info, 2) < 0)
			return false;

		zinitix_debug_msg("touch eeprom info = 0x%04X\r\n",
				chip_eeprom_info);
		if (!zinitix_bit_test(chip_eeprom_info, 0))
			break;

		if (time_out++ == 4) {
			write_cmd(client, BT541_CALIBRATE_CMD);
			mdelay(10);
			write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
			dev_err(&client->dev, "h/w calibration retry timeout.\n");
		}

		if (time_out++ > 10) {
			dev_err(&client->dev, "h/w calibration timeout.\n");
			break;
		}

	} while (1);

	if (write_reg(client,
				BT541_TOUCH_MODE, TOUCH_POINT_MODE) != I2C_SUCCESS)
		return false;

	if (info->cap_info.ic_int_mask != 0) {
		if (write_reg(client,
					BT541_INT_ENABLE_FLAG,
					info->cap_info.ic_int_mask)
				!= I2C_SUCCESS)
			return false;
	}

	write_reg(client, 0xc003, 0x0001);
	write_reg(client, 0xc104, 0x0001);
	udelay(100);
	if (write_cmd(client, BT541_SAVE_CALIBRATION_CMD) != I2C_SUCCESS)
		return false;

	mdelay(1000);
	write_reg(client, 0xc003, 0x0000);
	write_reg(client, 0xc104, 0x0000);
	return true;
}

static bool init_touch(struct bt541_ts_info *info, bool fw_oneshot_upgrade)
{
	struct bt541_ts_platform_data *pdata = info->pdata;
	struct i2c_client *client = info->client;
	struct capa_info *cap = &(info->cap_info);
	u16 reg_val;
	int i;
	u16 chip_eeprom_info;
#if USE_CHECKSUM
	u16 chip_check_sum;
	u8 checksum_err;
#endif
	int retry_cnt = 0;

retry_init:
	for (i = 0; i < INIT_RETRY_CNT; i++) {
		if (read_data(client, BT541_EEPROM_INFO_REG,
					(u8 *)&chip_eeprom_info, 2) < 0) {
			dev_err(&client->dev, "Failed to read eeprom info(%d)\n", i);
			mdelay(10);
			continue;
		} else
			break;
	}

	if (i == INIT_RETRY_CNT)
		goto fail_init;

#if USE_CHECKSUM
	dev_info(&client->dev, "%s: Check checksum\n", __func__);

	checksum_err = 0;

	for (i = 0; i < INIT_RETRY_CNT; i++) {
		if (read_data(client, BT541_CHECKSUM_RESULT,
					(u8 *)&chip_check_sum, 2) < 0) {
			mdelay(10);
			continue;
		}

#if defined(TSP_VERBOSE_DEBUG)
		dev_info(&client->dev, "0x%04X\n", chip_check_sum);
#endif

		if (chip_check_sum == 0x55aa)
			break;
		else {
			checksum_err = 1;
			break;
		}
	}

	if (i == INIT_RETRY_CNT || checksum_err) {
		dev_err(&client->dev, "Failed to check firmware data\n");
		if (checksum_err == 1 && retry_cnt < INIT_RETRY_CNT)
			retry_cnt = INIT_RETRY_CNT;

		goto fail_init;
	}
#endif

	if (write_cmd(client, BT541_SWRESET_CMD) != I2C_SUCCESS) {
		dev_err(&client->dev, "Failed to write reset command\n");
		goto fail_init;
	}

	cap->button_num = SUPPORTED_BUTTON_NUM;

	reg_val = 0;
	zinitix_bit_set(reg_val, BIT_PT_CNT_CHANGE);
	zinitix_bit_set(reg_val, BIT_DOWN);
	zinitix_bit_set(reg_val, BIT_MOVE);
	zinitix_bit_set(reg_val, BIT_UP);
#if (TOUCH_POINT_MODE == 2)
	zinitix_bit_set(reg_val, BIT_PALM);
	zinitix_bit_set(reg_val, BIT_PALM_REJECT);
#endif

	if (cap->button_num > 0)
		zinitix_bit_set(reg_val, BIT_ICON_EVENT);

	cap->ic_int_mask = reg_val;

	if (write_reg(client, BT541_INT_ENABLE_FLAG, 0x0) != I2C_SUCCESS)
		goto fail_init;

	dev_info(&client->dev, "%s: Send reset command\n", __func__);
	if (write_cmd(client, BT541_SWRESET_CMD) != I2C_SUCCESS)
		goto fail_init;

	/* get chip information */
	if (read_data(client, BT541_VENDOR_ID,
				(u8 *)&cap->vendor_id, 2) < 0) {
		zinitix_printk("failed to read chip revision\n");
		goto fail_init;
	}


	if (read_data(client, BT541_CHIP_REVISION,
				(u8 *)&cap->ic_revision, 2) < 0) {
		zinitix_printk("failed to read chip revision\n");
		goto fail_init;
	}

	cap->ic_fw_size = 32*1024;

	if (read_data(client, BT541_HW_ID, (u8 *)&cap->hw_id, 2) < 0) {
		dev_err(&client->dev, "Failed to read hw id\n");
		goto fail_init;
	}
#ifdef SUPPORTED_USE_DUAL_FW
	bt541_firmware_check();
#endif
	if (read_data(client, BT541_THRESHOLD, (u8 *)&cap->threshold, 2) < 0)
		goto fail_init;

	if (read_data(client, BT541_BUTTON_SENSITIVITY,
				(u8 *)&cap->key_threshold, 2) < 0)
		goto fail_init;

	/*if (read_data(client, BT541_DUMMY_BUTTON_SENSITIVITY,
				(u8 *)&cap->dummy_threshold, 2) < 0)
			goto fail_init;*/

	if (read_data(client, BT541_TOTAL_NUMBER_OF_X,
				(u8 *)&cap->x_node_num, 2) < 0)
		goto fail_init;

	if (read_data(client, BT541_TOTAL_NUMBER_OF_Y,
				(u8 *)&cap->y_node_num, 2) < 0)
		goto fail_init;

	cap->total_node_num = cap->x_node_num * cap->y_node_num;


	if (read_data(client, BT541_SHIFT_VALUE,
				(u8 *)&cap->shift_value, 2) < 0)
		goto fail_init;
	zinitix_debug_msg("Shift value = %d\n", cap->shift_value);


	if (read_data(client, BT541_DND_N_COUNT,
				(u8 *)&cap->N_cnt, 2) < 0)
		goto fail_init;
	zinitix_debug_msg("N count = %d\n", cap->N_cnt);

	if (read_data(client, BT541_DND_U_COUNT,
				(u8 *)&cap->U_cnt, 2) < 0)
		goto fail_init;
	zinitix_debug_msg("u count = %d\n", cap->U_cnt);

	if (read_data(client, BT541_AFE_FREQUENCY,
				(u8 *)&cap->afe_frequency, 2) < 0)
		goto fail_init;
	zinitix_debug_msg("afe frequency = %d\n", cap->afe_frequency);

	/* get chip firmware version */
	if (read_data(client, BT541_FIRMWARE_VERSION,
				(u8 *)&cap->fw_version, 2) < 0)
		goto fail_init;

	if (read_data(client, BT541_MINOR_FW_VERSION,
				(u8 *)&cap->fw_minor_version, 2) < 0)
		goto fail_init;

	if (read_data(client, BT541_DATA_VERSION_REG,
				(u8 *)&cap->reg_data_version, 2) < 0)
		goto fail_init;

#if TOUCH_ONESHOT_UPGRADE
	if (fw_oneshot_upgrade && (ts_check_need_upgrade(info, cap->fw_version,
					cap->fw_minor_version, cap->reg_data_version,
					cap->hw_id) == true)) {
		zinitix_printk("start upgrade firmware\n");

		if (ts_upgrade_firmware(info, m_firmware_data,
					cap->ic_fw_size) == false)
			goto fail_init;

		if (ts_hw_calibration(info) == false)
			goto fail_init;

		/* disable chip interrupt */
		if (write_reg(client, BT541_INT_ENABLE_FLAG, 0) != I2C_SUCCESS)
			goto fail_init;

		/* get chip firmware version */
		if (read_data(client, BT541_FIRMWARE_VERSION,
					(u8 *)&cap->fw_version, 2) < 0)
			goto fail_init;

		if (read_data(client, BT541_MINOR_FW_VERSION,
					(u8 *)&cap->fw_minor_version, 2) < 0)
			goto fail_init;

		if (read_data(client, BT541_DATA_VERSION_REG,
					(u8 *)&cap->reg_data_version, 2) < 0)
			goto fail_init;
#ifdef SUPPORTED_USE_DUAL_FW
		if (read_data(client, BT541_HW_ID,
					(u8 *)&cap->hw_id, 2) < 0)
			goto fail_init;
		bt541_firmware_check();
		if (read_data(client, BT541_VENDOR_ID,
					(u8 *)&cap->vendor_id, 2) < 0)
			goto fail_init;
#endif
	}
#endif

	if (read_data(client, BT541_EEPROM_INFO_REG,
				(u8 *)&chip_eeprom_info, 2) < 0)
		goto fail_init;

	if (zinitix_bit_test(chip_eeprom_info, 0)) { /* hw calibration bit*/
		if (ts_hw_calibration(info) == false)
			goto fail_init;

		/* disable chip interrupt */
		if (write_reg(client, BT541_INT_ENABLE_FLAG, 0) != I2C_SUCCESS)
			goto fail_init;
	}

	/* initialize */
	if (write_reg(client, BT541_X_RESOLUTION,
				(u16)pdata->x_resolution) != I2C_SUCCESS)
		goto fail_init;

	if (write_reg(client, BT541_Y_RESOLUTION,
				(u16)pdata->y_resolution) != I2C_SUCCESS)
		goto fail_init;

	cap->MinX = (u32)0;
	cap->MinY = (u32)0;
	cap->MaxX = (u32)pdata->x_resolution;
	cap->MaxY = (u32)pdata->y_resolution;

	if (write_reg(client, BT541_BUTTON_SUPPORTED_NUM,
				(u16)cap->button_num) != I2C_SUCCESS)
		goto fail_init;

	if (write_reg(client, BT541_SUPPORTED_FINGER_NUM,
				(u16)MAX_SUPPORTED_FINGER_NUM) != I2C_SUCCESS)
		goto fail_init;

	cap->multi_fingers = MAX_SUPPORTED_FINGER_NUM;
	zinitix_debug_msg("max supported finger num = %d\r\n",
			cap->multi_fingers);

	cap->gesture_support = 0;
	zinitix_debug_msg("set other configuration\r\n");

	if (write_reg(client, BT541_INITIAL_TOUCH_MODE,
				TOUCH_POINT_MODE) != I2C_SUCCESS)
		goto fail_init;

	if (write_reg(client, BT541_TOUCH_MODE, info->touch_mode) != I2C_SUCCESS)
		goto fail_init;

#if ZINITIX_I2C_CHECKSUM
	if (read_data(client, ZINITIX_INTERNAL_FLAG_02,
				(u8 *)&reg_val, 2) < 0)
		goto fail_init;
	cap->i2s_checksum = !(!zinitix_bit_test(reg_val, 15));
	printk("use i2s checksum = %d\r\n",
			cap->i2s_checksum);
#endif

	bt541_set_ta_status(info);
	bt541_set_optional_mode(info, true);
	/* soft calibration */
	/*	if (write_cmd(client, BT541_CALIBRATE_CMD) != I2C_SUCCESS)
		goto fail_init;*/

	if (write_reg(client, BT541_INT_ENABLE_FLAG,
				cap->ic_int_mask) != I2C_SUCCESS)
		goto fail_init;

	/* read garbage data */
	for (i = 0; i < 10; i++) {
		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
		udelay(10);
	}

	if (info->touch_mode != TOUCH_POINT_MODE) { /* Test Mode */
		if (write_reg(client, BT541_DELAY_RAW_FOR_HOST,
					RAWDATA_DELAY_FOR_HOST) != I2C_SUCCESS) {
			dev_err(&client->dev, "%s: Failed to set DELAY_RAW_FOR_HOST\n",
					__func__);

			goto fail_init;
		}
	}
#if ESD_TIMER_INTERVAL
	if (write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL,
				SCAN_RATE_HZ * ESD_TIMER_INTERVAL) != I2C_SUCCESS)
		goto fail_init;

	read_data(client, BT541_PERIODICAL_INTERRUPT_INTERVAL, (u8 *)&reg_val, 2);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Esd timer register = %d\n", reg_val);
#endif
#endif
	tsp_charger_status_cb = tsp_charger_enable;
	zinitix_debug_msg("successfully initialized\r\n");
	return true;

fail_init:
	if (cal_mode) {
		dev_err(&client->dev,"didn't update TSP F/W!! in CAL MODE\n");
		return false;
	}
	if (++retry_cnt <= INIT_RETRY_CNT) {
		bt541_power_control(info, POWER_OFF);
		bt541_power_control(info, POWER_ON_SEQUENCE);

		zinitix_debug_msg("retry to initiallize(retry cnt = %d)\r\n",
				retry_cnt);
		goto retry_init;

	} else if (retry_cnt == INIT_RETRY_CNT+1) {
		cap->ic_fw_size = 32*1024;

		zinitix_debug_msg("retry to initiallize(retry cnt = %d)\r\n", retry_cnt);
#if TOUCH_FORCE_UPGRADE
#ifdef SUPPORTED_USE_DUAL_FW
		ts_check_hwid_in_fatal_state(info);
		bt541_firmware_check();
#endif
		if (ts_upgrade_firmware(info, m_firmware_data,
					cap->ic_fw_size) == false) {
			zinitix_printk("upgrade failed\n");
			return false;
		}

		mdelay(100);

		/* hw calibration and make checksum */
		if (ts_hw_calibration(info) == false) {
			zinitix_printk("failed to initiallize\r\n");
			return false;
		}
		goto retry_init;
#endif
	}

	dev_err(&client->dev, "Failed to initiallize\n");

	return false;
}

static bool mini_init_touch(struct bt541_ts_info *info)
{
	struct bt541_ts_platform_data *pdata = info->pdata;
	struct i2c_client *client = info->client;
	int i;
#if USE_CHECKSUM
	u16 chip_check_sum;

	/*dev_info(&client->dev, "check checksum\n");*/

	if (read_data(client, BT541_CHECKSUM_RESULT,
				(u8 *)&chip_check_sum, 2) < 0)
		goto fail_mini_init;

	if (chip_check_sum != 0x55aa) {
		dev_err(&client->dev, "Failed to check firmware"
				" checksum(0x%04x)\n", chip_check_sum);

		goto fail_mini_init;
	}
#endif

	if (write_cmd(client, BT541_SWRESET_CMD) != I2C_SUCCESS) {
		dev_info(&client->dev, "Failed to write reset command\n");

		goto fail_mini_init;
	}

	/* initialize */
	if (write_reg(client, BT541_X_RESOLUTION,
				(u16)(pdata->x_resolution)) != I2C_SUCCESS)
		goto fail_mini_init;

	if (write_reg(client,BT541_Y_RESOLUTION,
				(u16)(pdata->y_resolution)) != I2C_SUCCESS)
		goto fail_mini_init;

	/*dev_info(&client->dev, "touch max x = %d\r\n", pdata->x_resolution);
	  dev_info(&client->dev, "touch max y = %d\r\n", pdata->y_resolution);*/

	if (write_reg(client, BT541_BUTTON_SUPPORTED_NUM,
				(u16)info->cap_info.button_num) != I2C_SUCCESS)
		goto fail_mini_init;

	if (write_reg(client, BT541_SUPPORTED_FINGER_NUM,
				(u16)MAX_SUPPORTED_FINGER_NUM) != I2C_SUCCESS)
		goto fail_mini_init;

	if (write_reg(client, BT541_INITIAL_TOUCH_MODE,
				TOUCH_POINT_MODE) != I2C_SUCCESS)
		goto fail_mini_init;

	if (write_reg(client, BT541_TOUCH_MODE,
				info->touch_mode) != I2C_SUCCESS)
		goto fail_mini_init;

	bt541_set_ta_status(info);
	bt541_set_optional_mode(info, true);
	/* soft calibration */
	if (write_cmd(client, BT541_CALIBRATE_CMD) != I2C_SUCCESS)
		goto fail_mini_init;

	if (write_reg(client, BT541_INT_ENABLE_FLAG,
				info->cap_info.ic_int_mask) != I2C_SUCCESS)
		goto fail_mini_init;

	/* read garbage data */
	for (i = 0; i < 10; i++) {
		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
		udelay(10);
	}

	if (info->touch_mode != TOUCH_POINT_MODE) {
		if (write_reg(client, BT541_DELAY_RAW_FOR_HOST,
					RAWDATA_DELAY_FOR_HOST) != I2C_SUCCESS) {
			dev_err(&client->dev, "Failed to set BT541_DELAY_RAW_FOR_HOST\n");

			goto fail_mini_init;
		}
	}

#if ESD_TIMER_INTERVAL
	if (write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL,
				SCAN_RATE_HZ * ESD_TIMER_INTERVAL) != I2C_SUCCESS)
		goto fail_mini_init;

	esd_timer_start(CHECK_ESD_TIMER, info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Started esd timer\n");
#endif
#endif

	dev_info(&client->dev, "Successfully mini initialized\r\n");
	return true;

fail_mini_init:
	dev_err(&client->dev, "Failed to initialize mini init\n");
	bt541_power_control(info, POWER_OFF);
	bt541_power_control(info, POWER_ON_SEQUENCE);

	if (init_touch(info, true) == false) {
		dev_err(&client->dev, "Failed to initialize\n");

		return false;
	}

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Started esd timer\n");
#endif
#endif
	return true;
}

static void clear_report_data(struct bt541_ts_info *info)
{
	int i;
	u8 reported = 0;
	u8 sub_status;

	for (i = 0; i < info->cap_info.button_num; i++) {
		if (info->button[i] == ICON_BUTTON_DOWN) {
			info->button[i] = ICON_BUTTON_UP;
			input_report_key(info->input_dev, BUTTON_MAPPING_KEY[i], 0);
			reported = true;
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
			printk(KERN_INFO "Button up = %d\n", i);
#else
			printk(KERN_INFO "Button up\n");
#endif
		}
	}

	for (i = 0; i < info->cap_info.multi_fingers; i++) {
		sub_status = info->reported_touch_info.coord[i].sub_status;
		if (zinitix_bit_test(sub_status, SUB_BIT_EXIST)) {
			input_mt_slot(info->input_dev, i);
			input_mt_report_slot_state(info->input_dev,	MT_TOOL_FINGER, 0);
			reported = true;
			if (!m_ts_debug_mode && TSP_NORMAL_EVENT_MSG)
				printk(KERN_INFO "[TSP] R %02d\r\n", i);
		}
		info->reported_touch_info.coord[i].sub_status = 0;
	}

	if (reported)
		input_sync(info->input_dev);

#ifdef CONFIG_INPUT_BOOSTER
	info->finger_cnt = 0;
	input_booster_send_event(BOOSTER_DEVICE_TOUCH, BOOSTER_MODE_FORCE_OFF);
#endif
}

#define	PALM_REPORT_WIDTH	200
#define	PALM_REJECT_WIDTH	255

static irqreturn_t bt541_touch_work(int irq, void *data)
{
	struct bt541_ts_info* info = (struct bt541_ts_info*)data;
	struct bt541_ts_platform_data *pdata = info->pdata;
	struct i2c_client *client = info->client;
	int i;
	u8 sub_status;
	u8 prev_sub_status;
	u32 x, y, maxX, maxY;
	u32 w;
	u32 tmp;
	u8 palm = 0;
#ifdef CONFIG_SEC_FACTORY
	u16 val = 0;
	int ret = 0;
#endif

	if (gpio_get_value(info->pdata->gpio_int)) {
		dev_err(&client->dev, "Invalid interrupt\n");

		return IRQ_HANDLED;
	}

	if (down_trylock(&info->work_lock)) {
		dev_err(&client->dev, "%s: Failed to occupy work lock\n", __func__);
		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);

		return IRQ_HANDLED;
	}
#if ESD_TIMER_INTERVAL
	esd_timer_stop(info);
#endif

	if (info->work_state != NOTHING) {
		dev_err(&client->dev, "%s: Other process occupied\n", __func__);
		udelay(DELAY_FOR_SIGNAL_DELAY);

		if (!gpio_get_value(info->pdata->gpio_int)) {
			write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
			udelay(DELAY_FOR_SIGNAL_DELAY);
		}

		goto out;
	}

	info->work_state = NORMAL;

#if ZINITIX_I2C_CHECKSUM
	i = 0;
	if (ts_read_coord(info) == false || info->touch_info.status == 0xffff
			|| info->touch_info.status == 0x1) {
		/* more retry*/
		for (i = 1; i < 50; i++) {	/* about 10ms*/
			if (!(ts_read_coord(info) == false || info->touch_info.status == 0xffff
						|| info->touch_info.status == 0x1))
				break;
		}

	}

	if (i == 50) {
		dev_err(&client->dev, "Failed to read info coord\n");
		bt541_power_control(info, POWER_OFF);
		bt541_power_control(info, POWER_ON_SEQUENCE);

		clear_report_data(info);
		mini_init_touch(info);

		goto out;
	}
#else
	if (ts_read_coord(info) == false || info->touch_info.status == 0xffff
			|| info->touch_info.status == 0x1) { /* maybe desirable reset */
		dev_err(&client->dev, "Failed to read info coord\n");
		bt541_power_control(info, POWER_OFF);
		bt541_power_control(info, POWER_ON_SEQUENCE);

		clear_report_data(info);
		mini_init_touch(info);

		goto out;
	}
#endif
	/* invalid : maybe periodical repeated int. */
	if (info->touch_info.status == 0x0)
		goto out;

	if (zinitix_bit_test(info->touch_info.status, BIT_ICON_EVENT)) {
		if (read_data(info->client, BT541_ICON_STATUS_REG,
					(u8 *)(&info->icon_event_reg), 2) < 0) {
			dev_err(&client->dev, "Failed to read button info\n");
			write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);

			goto out;
		}

		for (i = 0; i < info->cap_info.button_num; i++) {
			if (zinitix_bit_test(info->icon_event_reg,
						(BIT_O_ICON0_DOWN + i))) {
				info->button[i] = ICON_BUTTON_DOWN;
				input_report_key(info->input_dev, BUTTON_MAPPING_KEY[i], 1);
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
				dev_info(&client->dev, "Button down = %d\n", i);
#else
				dev_info(&client->dev, "Button down\n");
#endif
			}
		}

		for (i = 0; i < info->cap_info.button_num; i++) {
			if (zinitix_bit_test(info->icon_event_reg,
						(BIT_O_ICON0_UP + i))) {
				info->button[i] = ICON_BUTTON_UP;
				input_report_key(info->input_dev, BUTTON_MAPPING_KEY[i], 0);
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
				dev_info(&client->dev, "Button up = %d\n", i);
#else
				dev_info(&client->dev, "Button up\n");
#endif
			}
		}
	}

#ifdef SUPPORTED_PALM_TOUCH
	if (zinitix_bit_test(info->touch_info.status, BIT_PALM)) {
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
		dev_info(&client->dev, "Palm report\n");
#endif
		palm = 1;
	}

	if (zinitix_bit_test(info->touch_info.status, BIT_PALM_REJECT)) {
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
		dev_info(&client->dev, "Palm reject\n");
#endif
		palm = 2;
	}
#endif
	for (i = 0; i < info->cap_info.multi_fingers; i++) {
		sub_status = info->touch_info.coord[i].sub_status;
		prev_sub_status = info->reported_touch_info.coord[i].sub_status;

		if (zinitix_bit_test(sub_status, SUB_BIT_EXIST)) {
			x = info->touch_info.coord[i].x;
			y = info->touch_info.coord[i].y;
			w = info->touch_info.coord[i].width;

			/* transformation from touch to screen orientation */
			if (pdata->orientation & TOUCH_V_FLIP)
				y = info->cap_info.MaxY
					+ info->cap_info.MinY - y;

			if (pdata->orientation & TOUCH_H_FLIP)
				x = info->cap_info.MaxX
					+ info->cap_info.MinX - x;

			maxX = info->cap_info.MaxX;
			maxY = info->cap_info.MaxY;

			if (pdata->orientation & TOUCH_XY_SWAP) {
				zinitix_swap_v(x, y, tmp);
				zinitix_swap_v(maxX, maxY, tmp);
			}

			if (x > maxX || y > maxY) {
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
				dev_err(&client->dev,
						"Invalid coord %d : x=%d, y=%d\n", i, x, y);
#endif
				continue;
			}

			info->touch_info.coord[i].x = x;
			info->touch_info.coord[i].y = y;
			if (zinitix_bit_test(sub_status, SUB_BIT_DOWN))
			{
#if TOUCH_BOOSTER
				if (!min_handle)
				{
					min_handle = cpufreq_limit_min_freq(touch_cpufreq_lock, "TSP");
					if (IS_ERR(min_handle)) {
						printk(KERN_ERR "[TSP] cannot get cpufreq_min lock %lu(%ld)\n",
								touch_cpufreq_lock, PTR_ERR(min_handle));
						min_handle = NULL;
					}
					_store_cpu_num_min_limit(2);
					dev_info(&client->dev, "cpu freq on\n");
				}
				info->finger_cnt++;
#endif
#ifdef CONFIG_INPUT_BOOSTER
				info->finger_cnt++;
				input_booster_send_event(BOOSTER_DEVICE_TOUCH, BOOSTER_MODE_ON);
#endif
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
				dev_info(&client->dev, "Finger [%02d] x = %d, y = %d,"
						" w = %d\n", i, x, y, w);
#else
				dev_info(&client->dev, "Finger down\n");
#endif
			}
			if (w == 0)
				w = 1;

			input_mt_slot(info->input_dev, i);
			input_mt_report_slot_state(info->input_dev, MT_TOOL_FINGER, 1);

#if (TOUCH_POINT_MODE == 2)
			if (palm == 0) {
				if (w >= PALM_REPORT_WIDTH)
					w = PALM_REPORT_WIDTH - 10;
			} else if (palm == 1) {	/*palm report*/
				w = PALM_REPORT_WIDTH;
				/*				info->touch_info.coord[i].minor_width = PALM_REPORT_WIDTH;*/
			} else if (palm == 2) {	/* palm reject*/
				/*				x = y = 0;*/
				w = PALM_REJECT_WIDTH;
				/*				info->touch_info.coord[i].minor_width = PALM_REJECT_WIDTH;*/
			}
#endif
#ifdef CONFIG_SEC_FACTORY
			ret = read_data(client, BT541_REAL_WIDTH + i, (u8*)&val, 2);
			if (ret < 0)
					dev_info(&client->dev, ": Failed to read %d's Real width %s\n", i, __func__);
			input_report_abs(info->input_dev, ABS_MT_PRESSURE, (u32)val);
#else
			input_report_abs(info->input_dev, ABS_MT_PRESSURE, (u32)w);
#endif
			input_report_abs(info->input_dev, ABS_MT_TOUCH_MAJOR, (u32)w);
			input_report_abs(info->input_dev, ABS_MT_WIDTH_MAJOR,
					(u32)((palm == 1) ? w-40 : w));
#if (TOUCH_POINT_MODE == 2)
			input_report_abs(info->input_dev,
					ABS_MT_TOUCH_MINOR, (u32)info->touch_info.coord[i].minor_width);
			/*			input_report_abs(info->input_dev,
							ABS_MT_WIDTH_MINOR, (u32)info->touch_info.coord[i].minor_width);*/
#ifdef SUPPORTED_PALM_TOUCH
			/*input_report_abs(info->input_dev, ABS_MT_ANGLE,
			  (palm > 1)?70:info->touch_info.coord[i].angle - 90);*/
			/*dev_info(&client->dev, "finger [%02d] angle = %03d\n", i,
			  info->touch_info.coord[i].angle);*/
			input_report_abs(info->input_dev, ABS_MT_PALM, (palm > 0)?1:0);
#endif
			/*			input_report_abs(info->input_dev, ABS_MT_PALM, 1);*/
#endif

			input_report_abs(info->input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(info->input_dev, ABS_MT_POSITION_Y, y);
		} else if (zinitix_bit_test(sub_status, SUB_BIT_UP)||
				zinitix_bit_test(prev_sub_status, SUB_BIT_EXIST)) {
#if TOUCH_BOOSTER
			info->finger_cnt--;
			if (!info->finger_cnt)
			{
				cpufreq_limit_put(min_handle);
				min_handle = NULL;
				_store_cpu_num_min_limit(1);
				dev_info(&client->dev, "cpu freq off\n");
			}
#endif
#ifdef CONFIG_INPUT_BOOSTER
			info->finger_cnt--;
			if (!info->finger_cnt)
				input_booster_send_event(BOOSTER_DEVICE_TOUCH, BOOSTER_MODE_OFF);
#endif
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
			dev_info(&client->dev, "Finger [%02d] up\n", i);
#else
			dev_info(&client->dev, "Finger up\n");
#endif
			memset(&info->touch_info.coord[i], 0x0, sizeof(struct coord));
			input_mt_slot(info->input_dev, i);
			input_mt_report_slot_state(info->input_dev, MT_TOOL_FINGER, 0);

		} else {
			memset(&info->touch_info.coord[i], 0x0, sizeof(struct coord));
		}
	}
	memcpy((char *)&info->reported_touch_info, (char *)&info->touch_info,
			sizeof(struct point_info));
	input_sync(info->input_dev);

out:
	if (info->work_state == NORMAL) {
#if ESD_TIMER_INTERVAL
		esd_timer_start(CHECK_ESD_TIMER, info);
#endif
		info->work_state = NOTHING;
	}

	up(&info->work_lock);

	return IRQ_HANDLED;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bt541_ts_late_resume(struct early_suspend *h)
{
	struct bt541_ts_info *info = misc_info;
	//info = container_of(h, struct bt541_ts_info, early_suspend);
	struct capa_info *cap = &(info->cap_info);

	if (info == NULL)
		return;
	zinitix_printk("late resume++\r\n");

	down(&info->work_lock);
	if (info->work_state != RESUME
			&& info->work_state != EALRY_SUSPEND) {
		zinitix_printk("invalid work proceedure (%d)\r\n",
				info->work_state);
		up(&info->work_lock);
		return;
	}
#if 0
	write_cmd(info->client, BT541_WAKEUP_CMD);
	mdelay(1);
#else
	bt541_power_control(info, POWER_ON_SEQUENCE);
#endif
	info->work_state = RESUME;
	if (mini_init_touch(info) == false)
		goto fail_late_resume;

	if (read_data(info->client, BT541_FIRMWARE_VERSION, (u8 *)&cap->fw_version, 2))
		;
	if (read_data(info->client, BT541_MINOR_FW_VERSION, (u8 *)&cap->fw_minor_version, 2))
		;
	if (read_data(info->client, BT541_DATA_VERSION_REG, (u8 *)&cap->reg_data_version, 2))
		;
	if (read_data(info->client, BT541_HW_ID, (u8 *)&cap->hw_id, 2))
		;
	if (read_data(info->client, BT541_VENDOR_ID, (u8 *)&cap->vendor_id, 2))
		;
#ifdef SUPPORTED_USE_DUAL_FW
	bt541_firmware_check();
#endif
	enable_irq(info->irq);
	info->work_state = NOTHING;
	up(&info->work_lock);
	zinitix_printk("late resume--\n");
	return;
fail_late_resume:
	zinitix_printk("failed to late resume\n");
	enable_irq(info->irq);
	info->work_state = NOTHING;
	up(&info->work_lock);
	return;
}

static void bt541_ts_early_suspend(struct early_suspend *h)
{
	struct bt541_ts_info *info = misc_info;
	/*info = container_of(h, struct bt541_ts_info, early_suspend);*/

	if (info == NULL)
		return;

	zinitix_printk("early suspend++\n");

	disable_irq(info->irq);
#if ESD_TIMER_INTERVAL
	flush_work(&info->tmr_work);
#endif

	down(&info->work_lock);
	if (info->work_state != NOTHING) {
		zinitix_printk("invalid work proceedure (%d)\r\n",
				info->work_state);
		up(&info->work_lock);
		enable_irq(info->irq);
		return;
	}
	info->work_state = EALRY_SUSPEND;

	zinitix_debug_msg("clear all reported points\r\n");
	clear_report_data(info);

#if ESD_TIMER_INTERVAL
	/*write_reg(info->client, BT541_PERIODICAL_INTERRUPT_INTERVAL, 0);*/
	esd_timer_stop(info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&info->client->dev, "Stopped esd timer\n");
#endif
#endif

#if 0
	write_reg(info->client, BT541_INT_ENABLE_FLAG, 0x0);

	udelay(100);
	if (write_cmd(info->client, BT541_SLEEP_CMD) != I2C_SUCCESS) {
		zinitix_printk("failed to enter into sleep mode\n");
		up(&info->work_lock);
		return;
	}
#else
	bt541_power_control(info, POWER_OFF);
#endif
#if TOUCH_BOOSTER
	if (min_handle)
	{
		dev_err(&info->client->dev,"[TSP] %s %d:: OOPs, Cpu was not in Normal Freq..\n", __func__, __LINE__);

		if (cpufreq_limit_put(min_handle) < 0) {
			dev_err(&info->client->dev, "[TSP] Error in scaling down cpu frequency\n");
		}

		min_handle = NULL;
		info->finger_cnt = 0;
		dev_info(&info->client->dev, "cpu freq off\n");
	}
#endif
	zinitix_printk("early suspend--\n");
	up(&info->work_lock);
	return;
}
#endif	/* CONFIG_HAS_EARLYSUSPEND */

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static int bt541_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_info *info = i2c_get_clientdata(client);
	struct capa_info *cap = &(info->cap_info);

#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "resume++\n");
#endif
	down(&info->work_lock);
	if (info->work_state != SUSPEND) {
		dev_err(&client->dev, "%s: Invalid work proceedure (%d)\n",
				__func__, info->work_state);
		up(&info->work_lock);

		return 0;
	}
	bt541_power_control(info, POWER_ON_SEQUENCE);

#ifdef CONFIG_HAS_EARLYSUSPEND
	info->work_state = RESUME;
#else
	info->work_state = NOTHING;
	if (mini_init_touch(info) == false)
		dev_err(&client->dev, "Failed to resume\n");

	if (read_data(client, BT541_FIRMWARE_VERSION, (u8 *)&cap->fw_version, 2))
		;
	if (read_data(client, BT541_MINOR_FW_VERSION, (u8 *)&cap->fw_minor_version, 2))
		;
	if (read_data(client, BT541_DATA_VERSION_REG, (u8 *)&cap->reg_data_version, 2))
		;
	if (read_data(client, BT541_HW_ID, (u8 *)&cap->hw_id, 2))
		;
	if (read_data(client, BT541_VENDOR_ID, (u8 *)&cap->vendor_id, 2))
		;
#ifdef SUPPORTED_USE_DUAL_FW
	bt541_firmware_check();
#endif
	if (!gpio_get_value(info->pdata->gpio_int))
	{
		write_cmd(info->client, BT541_CLEAR_INT_STATUS_CMD);
		udelay(50);
		write_cmd(info->client, BT541_CLEAR_INT_STATUS_CMD);
		udelay(50);
		write_cmd(info->client, BT541_CLEAR_INT_STATUS_CMD);
	}
	info->work_state = NOTHING;
#endif

#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "resume--\n");
#endif
	up(&info->work_lock);
	enable_irq(info->irq);
	return 0;
}

static int bt541_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_info *info = i2c_get_clientdata(client);

#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "suspend++\n");
#endif

#ifndef CONFIG_HAS_EARLYSUSPEND
	disable_irq(info->irq);
#endif
#if ESD_TIMER_INTERVAL
	flush_work(&info->tmr_work);
#endif

	down(&info->work_lock);
	if (info->work_state != NOTHING
			&& info->work_state != SUSPEND) {
		dev_err(&client->dev,"%s: Invalid work proceedure (%d)\n",
				__func__, info->work_state);
		up(&info->work_lock);
#ifndef CONFIG_HAS_EARLYSUSPEND
		enable_irq(info->irq);
#endif
		return 0;
	}

#ifndef CONFIG_HAS_EARLYSUSPEND
	clear_report_data(info);

#if ESD_TIMER_INTERVAL
	esd_timer_stop(info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Stopped esd timer\n");
#endif
#endif
#endif
	write_cmd(info->client, BT541_SLEEP_CMD);
	bt541_power_control(info, POWER_OFF);
	info->work_state = SUSPEND;

#if defined(TSP_VERBOSE_DEBUG)
	zinitix_printk("suspend--\n");
#endif
	up(&info->work_lock);

	return 0;
}
#endif

static bool ts_set_touchmode(u16 value)
{
	int i;

	disable_irq(misc_info->irq);

	down(&misc_info->work_lock);
	if (misc_info->work_state != NOTHING) {
		printk(KERN_INFO "other process occupied.. (%d)\n",
				misc_info->work_state);
		enable_irq(misc_info->irq);
		up(&misc_info->work_lock);
		return -1;
	}

	misc_info->work_state = SET_MODE;

	if (value == TOUCH_DND_MODE) {
		if (write_reg(misc_info->client, BT541_DND_N_COUNT,
					SEC_DND_N_COUNT) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set DND_N_COUNT\n");

		if (write_reg(misc_info->client, BT541_DND_U_COUNT,
					SEC_DND_U_COUNT) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev,  "Failed to set DND_U_COUNT\n");

		if (write_reg(misc_info->client, BT541_AFE_FREQUENCY,
					SEC_DND_FREQUENCY) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev,  "Failed to set AFE_FREQUENCY\n");

	} else if (misc_info->touch_mode == TOUCH_DND_MODE) {
		if (write_reg(misc_info->client, BT541_AFE_FREQUENCY,
					misc_info->cap_info.afe_frequency) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set AFE_FREQUENCY\n");

		if (write_reg(misc_info->client, BT541_DND_U_COUNT,
					misc_info->cap_info.U_cnt) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set U_COUNT\n");

		if (write_reg(misc_info->client, BT541_SHIFT_VALUE,
					misc_info->cap_info.shift_value) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set Shift\n");
	}

	if (value == TOUCH_SEC_MODE)
		misc_info->touch_mode = TOUCH_POINT_MODE;
	else
		misc_info->touch_mode = value;

	printk(KERN_INFO "[zinitix_touch] tsp_set_testmode, "
			"touchkey_testmode = %d\r\n", misc_info->touch_mode);

	if (misc_info->touch_mode != TOUCH_POINT_MODE) {
		if (write_reg(misc_info->client, BT541_DELAY_RAW_FOR_HOST,
					RAWDATA_DELAY_FOR_HOST) != I2C_SUCCESS)
			zinitix_printk("Fail to set BT541_DELAY_RAW_FOR_HOST.\r\n");
	}

	if (write_reg(misc_info->client, BT541_TOUCH_MODE,
				misc_info->touch_mode) != I2C_SUCCESS)
		printk(KERN_INFO "[zinitix_touch] TEST Mode : "
				"Fail to set ZINITX_TOUCH_MODE %d.\r\n", misc_info->touch_mode);

	/* clear garbage data */
	for (i = 0; i < 10; i++) {
		mdelay(20);
		write_cmd(misc_info->client, BT541_CLEAR_INT_STATUS_CMD);
	}

	misc_info->work_state = NOTHING;
	enable_irq(misc_info->irq);
	up(&misc_info->work_lock);
	return 1;
}

static bool ts_set_touchmode2(u16 value)
{
	int i;

	disable_irq(misc_info->irq);

	down(&misc_info->work_lock);
	if (misc_info->work_state != NOTHING) {
		printk(KERN_INFO "other process occupied.. (%d)\n",
				misc_info->work_state);
		enable_irq(misc_info->irq);
		up(&misc_info->work_lock);
		return -1;
	}

	misc_info->work_state = SET_MODE;

	if (value == TOUCH_DND_MODE) {
		if (write_reg(misc_info->client, BT541_DND_N_COUNT,
					SEC_HFDND_N_COUNT) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set DND_N_COUNT\n");

		if (write_reg(misc_info->client, BT541_DND_U_COUNT,
					SEC_HFDND_U_COUNT) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev,  "Failed to set DND_U_COUNT\n");

		if (write_reg(misc_info->client, BT541_AFE_FREQUENCY,
					SEC_HFDND_FREQUENCY) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev,  "Failed to set AFE_FREQUENCY\n");


	} else {
		if (write_reg(misc_info->client, BT541_AFE_FREQUENCY,
					misc_info->cap_info.afe_frequency) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set AFE_FREQUENCY\n");

		if (write_reg(misc_info->client, BT541_DND_U_COUNT,
					misc_info->cap_info.U_cnt) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set U_COUNT\n");

		if (write_reg(misc_info->client, BT541_SHIFT_VALUE,
					misc_info->cap_info.shift_value) != I2C_SUCCESS)
			dev_err(&misc_info->client->dev, "Failed to set Shift\n");
	}

	if (value == TOUCH_SEC_MODE)
		misc_info->touch_mode = TOUCH_POINT_MODE;
	else
		misc_info->touch_mode = value;

	printk(KERN_INFO "[zinitix_touch] tsp_set_testmode, "
			"touchkey_testmode = %d\r\n", misc_info->touch_mode);

	if (misc_info->touch_mode != TOUCH_POINT_MODE) {
		if (write_reg(misc_info->client, BT541_DELAY_RAW_FOR_HOST,
					RAWDATA_DELAY_FOR_HOST) != I2C_SUCCESS)
			zinitix_printk("Fail to set BT541_DELAY_RAW_FOR_HOST.\r\n");
	}

	if (write_reg(misc_info->client, BT541_TOUCH_MODE,
				misc_info->touch_mode) != I2C_SUCCESS)
		printk(KERN_INFO "[zinitix_touch] TEST Mode : "
				"Fail to set ZINITX_TOUCH_MODE %d.\r\n", misc_info->touch_mode);

	/* clear garbage data */
	for (i = 0; i < 10; i++) {
		mdelay(20);
		write_cmd(misc_info->client, BT541_CLEAR_INT_STATUS_CMD);
	}

	misc_info->work_state = NOTHING;
	enable_irq(misc_info->irq);
	up(&misc_info->work_lock);
	return 1;
}

static int ts_upgrade_sequence(const u8 *firmware_data)
{
	disable_irq(misc_info->irq);
	down(&misc_info->work_lock);
	misc_info->work_state = UPGRADE;

#if ESD_TIMER_INTERVAL
	esd_timer_stop(misc_info);
#endif
	zinitix_debug_msg("clear all reported points\r\n");
	clear_report_data(misc_info);

	printk(KERN_INFO "start upgrade firmware\n");
	if (ts_upgrade_firmware(misc_info,
				firmware_data,
				misc_info->cap_info.ic_fw_size) == false) {
		enable_irq(misc_info->irq);
		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return -1;
	}

	if (init_touch(misc_info, false) == false) {
		enable_irq(misc_info->irq);
		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return -1;
	}

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, misc_info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&misc_info->client->dev, "Started esd timer\n");
#endif
#endif

	enable_irq(misc_info->irq);
	misc_info->work_state = NOTHING;
	up(&misc_info->work_lock);
	return 0;
}

#ifdef SEC_FACTORY_TEST
static inline void set_cmd_result(struct bt541_ts_info *info, char *buff, int len)
{
	strncat(info->factory_info->cmd_result, buff, len);
}

static inline void set_default_result(struct bt541_ts_info *info)
{
	char delim = ':';
	memset(info->factory_info->cmd_result, 0x00, ARRAY_SIZE(info->factory_info->cmd_result));
	memcpy(info->factory_info->cmd_result, info->factory_info->cmd, strlen(info->factory_info->cmd));
	strncat(info->factory_info->cmd_result, &delim, 1);
}

#define MAX_FW_PATH 255
#define TSP_FW_FILENAME "zinitix_fw.bin"

static void fw_update(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	int ret = 0;
	const u8 *buff = 0;
	mm_segment_t old_fs = {0};
	struct file *fp = NULL;
	long fsize = 0, nread = 0;
	char fw_path[MAX_FW_PATH+1];
	char result[16] = {0};

	set_default_result(info);

	switch (info->factory_info->cmd_param[0]) {
	case BUILT_IN:
		ret = ts_upgrade_sequence((u8 *)m_firmware_data);
		if (ret < 0) {
			info->factory_info->cmd_state = 3;
			return;
		}
		break;

	case UMS:
		old_fs = get_fs();
		set_fs(get_ds());

		snprintf(fw_path, MAX_FW_PATH, "/sdcard/%s", TSP_FW_FILENAME);
		fp = filp_open(fw_path, O_RDONLY, 0);
		if (IS_ERR(fp)) {
			dev_err(&client->dev,
					"file %s open error:%lu\n", fw_path, (unsigned long)fp);
			info->factory_info->cmd_state = 3;
			goto err_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;

		if (fsize != info->cap_info.ic_fw_size) {
			dev_err(&client->dev, "invalid fw size!!\n");
			info->factory_info->cmd_state = 3;
			goto err_open;
		}

		buff = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!buff) {
			dev_err(&client->dev, "failed to alloc buffer for fw\n");
			info->factory_info->cmd_state = 3;
			goto err_alloc;
		}

		nread = vfs_read(fp, (char __user *)buff, fsize, &fp->f_pos);
		if (nread != fsize) {
			info->factory_info->cmd_state = 3;
			goto err_fw_size;
		}

		filp_close(fp, current->files);
		set_fs(old_fs);
		dev_info(&client->dev, "ums fw is loaded!!\n");

		ret = ts_upgrade_sequence((u8 *)buff);
		if (ret < 0) {
			kfree(buff);
			info->factory_info->cmd_state = 3;
			goto update_fail;
		}
		break;

	default:
		dev_err(&client->dev, "invalid fw file type!!\n");
		goto update_fail;
	}

	info->factory_info->cmd_state = 2;
	snprintf(result, sizeof(result) , "%s", "OK");
	set_cmd_result(info, result,
			strnlen(result, sizeof(result)));
	kfree(buff);

	return;


	if (fp != NULL) {
err_fw_size:
		kfree(buff);
err_alloc:
		filp_close(fp, NULL);
err_open:
		set_fs(old_fs);
	}
update_fail:
	snprintf(result, sizeof(result) , "%s", "NG");
	set_cmd_result(info, result, strnlen(result, sizeof(result)));
}

static void get_fw_ver_bin(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	u16 fw_version, fw_minor_version, reg_version, hw_id;
	u32 version;

	set_default_result(info);

	/* To Do */
	/* modify m_firmware_data */
	fw_version = (u16)(m_firmware_data[52] | (m_firmware_data[53] << 8));
	fw_minor_version = (u16)(m_firmware_data[56] | (m_firmware_data[57] << 8));
	reg_version = (u16)(m_firmware_data[60] | (m_firmware_data[61] << 8));
	hw_id =  (u16)(m_firmware_data[0x7528] | (m_firmware_data[0x7529]<<8));
	/*vendor_id = ntohs(*(u16 *)&m_firmware_data[0x57e2]);*/
	version = (u32)((u32)(hw_id & 0xff) << 16) | ((fw_version & 0xf ) << 12)
		| ((fw_minor_version & 0xf) << 8) | (reg_version & 0xff);

	/*length = sizeof(vendor_id);
	snprintf(finfo->cmd_buff, length + 1, "%s", (u8 *)&vendor_id);
	snprintf(finfo->cmd_buff + length, sizeof(finfo->cmd_buff) - length,
				"%06X", version);*/

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "ZI%06X", version);

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void get_fw_ver_ic(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	u16 fw_version, fw_minor_version, reg_version, hw_id;
	u32 version;

	set_default_result(info);

	fw_version = info->cap_info.fw_version;
	fw_minor_version = info->cap_info.fw_minor_version;
	reg_version = info->cap_info.reg_data_version;
	hw_id = info->cap_info.hw_id;
	/*vendor_id = ntohs(info->cap_info.vendor_id);*/
	version = (u32)((u32)(hw_id & 0xff) << 16) | ((fw_version & 0xf) << 12)
		| ((fw_minor_version & 0xf) << 8) | (reg_version & 0xff);

	/*length = sizeof(vendor_id);
	snprintf(finfo->cmd_buff, length + 1, "%s", (u8 *)&vendor_id);
	snprintf(finfo->cmd_buff + length, sizeof(finfo->cmd_buff) - length,
				"%06X", version);*/

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "ZI%06X", version);

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void get_threshold(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
			"%d", info->cap_info.threshold);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void module_off_master(void *device_data)
{
	return;
}

static void module_on_master(void *device_data)
{
	return;
}

static void module_off_slave(void *device_data)
{
	return;
}

static void module_on_slave(void *device_data)
{
	return;
}

#define BT541_VENDOR_NAME "ZINITIX"

static void get_chip_vendor(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
			"%s", BT541_VENDOR_NAME);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

#define BT541_CHIP_NAME "BT541C"

static void get_chip_name(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", BT541_CHIP_NAME);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

#ifdef SUPPORTED_USE_DUAL_FW
static void get_module_vendor(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	int module_vendor =0;

	set_default_result(info);

	switch (info->cap_info.hw_id) {
	case 0:
		module_vendor = 0;
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s,%d", "OK", module_vendor);
		break;
	case 1:
		module_vendor = 1;
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s,%d", "OK", module_vendor);
		break;
	default:
		break;
	}
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}
#endif

static void get_x_num(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
			"%u", info->cap_info.x_node_num);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void get_y_num(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
			"%u", info->cap_info.y_node_num);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void not_support_cmd(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

	sprintf(finfo->cmd_buff, "%s", "NA");
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	info->factory_info->cmd_state = NOT_APPLICABLE;

	dev_info(&client->dev, "%s: \"%s(%d)\"\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void run_reference_read(void * device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	u16 min, max;
	s32 i, j, offset;

#if ESD_TIMER_INTERVAL
	esd_timer_stop(misc_info);
#endif
	set_default_result(info);

	ts_set_touchmode(TOUCH_DND_MODE);
	get_raw_data(info, (u8 *)raw_data->dnd_data, 5);
	ts_set_touchmode(TOUCH_POINT_MODE);

	min = 0xFFFF;
	max = 0x0000;

	for (i = 0; i < info->cap_info.x_node_num; i++) {
		for (j = 0; j < info->cap_info.y_node_num; j++) {
			offset = i * info->cap_info.y_node_num + j;

			if (raw_data->dnd_data[offset] &&
				  raw_data->dnd_data[offset] < min)
				min = raw_data->dnd_data[offset];

			if (raw_data->dnd_data[offset] > max)
				max = raw_data->dnd_data[offset];
		}
	}

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%d,%d\n", min, max);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s\n", __func__, finfo->cmd_buff);

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, misc_info);
#endif
	return;
}

static void run_dnd_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset, val, x, y, node_num;
	bool result = true;

#if ESD_TIMER_INTERVAL
	esd_timer_stop(misc_info);
#endif
	set_default_result(info);

	ts_set_touchmode(TOUCH_DND_MODE);
	get_raw_data(info, (u8 *)raw_data->dnd_data, 5);
	ts_set_touchmode(TOUCH_POINT_MODE);

	printk("DND start\n");

	for (i = 0; i < x_num; i++) {
		for (j = 0; j < y_num; j++) {
			offset = (i * y_num) + j;
			printk("%d ", raw_data->dnd_data[offset]);

			if ((raw_data->dnd_data[offset] > dnd_max[i][j]) ||
			  (raw_data->dnd_data[offset] &&
			  (raw_data->dnd_data[offset] < dnd_min[i][j]))) {
				val =  raw_data->dnd_data[offset];
				x = i;
				y = j;
				node_num = offset;
				result = false;
			}
		}
		printk("\n");
	}

	if (result) {
	dev_info(&client->dev, "DND Pass\n");
	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	} else {
		dev_err(&client->dev, "DND Fail\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
				"Fail %d,%d,%d,%d\n",
				node_num, dnd_min[x][y], dnd_max[x][y], val);
	}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, misc_info);
#endif
	return;
}

static void run_dnd_v_gap_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset, val, cur_val, next_val, x, y, node_num, fail_val;
	bool result = true;

	set_default_result(info);

	memset(raw_data->vgap_data, 0x00, TSP_CMD_NODE_NUM);

	printk("DND V Gap start\n");

	for (i = 0; i < x_num - 1; i++) {
		for (j = 0; j < y_num; j++) {
			offset = (i * y_num) + j;

			cur_val = raw_data->dnd_data[offset];
			next_val = raw_data->dnd_data[offset + y_num];
			if (!next_val) {
				raw_data->vgap_data[offset] = next_val;
				continue;
	}

			if (next_val > cur_val)
				val = 100 - ((cur_val * 100) / next_val);
	else
				val = 100 - ((next_val * 100) / cur_val);

			printk("%d ", val);
			cur_val = (s16)(dnd_v_gap[i][j]);

			if (val > cur_val) {
				fail_val = val;
				x = i;
				y = j;
				node_num = offset;
				result = false;
				}
			raw_data->vgap_data[offset] = val;
			}
		printk("\n");
			}

	if (result) {
		dev_info(&client->dev, "DND V Gap Pass\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	} else {
		dev_err(&client->dev, "DND V Gap Fail\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
				"Fail %d,%d,%d,%d\n",
				node_num, 0, dnd_v_gap[x][y], fail_val);
	}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	return;
}

static void run_dnd_h_gap_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset, val, cur_val, next_val, x, y, node_num, fail_val;
	bool result = true;

	set_default_result(info);

	memset(raw_data->hgap_data, 0x00, TSP_CMD_NODE_NUM);

	printk("DND H Gap start\n");

	for (i = 0; i < x_num ; i++) {
		for (j = 0; j < y_num-1; j++) {
			offset = (i * y_num) + j;

			cur_val = raw_data->dnd_data[offset];
			if (!cur_val) {
				raw_data->hgap_data[offset] = cur_val;
				continue;
			}

			next_val = raw_data->dnd_data[offset + 1];
			if (!next_val) {
				raw_data->hgap_data[offset] = next_val;
				for (++j; j < y_num - 1; j++) {
					offset = (i * y_num) + j;

					next_val = raw_data->dnd_data[offset];
					if (!next_val) {
						raw_data->hgap_data[offset]
							= next_val;
						continue;
			}

					break;
		}
	}

			if (next_val > cur_val)
				val = 100 - ((cur_val * 100) / next_val);
	else
				val = 100 - ((next_val * 100) / cur_val);

			printk("%d ", val);
			cur_val = (s16)(dnd_h_gap[i][j]);

			if (val > cur_val) {
				fail_val = val;
				x = i;
				y = j;
				node_num = offset;
			result = false;
			}
			raw_data->hgap_data[offset] = val;
		}
		printk("\n");
		}

	if (result) {
		dev_info(&client->dev, "DND H Gap Pass\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	} else {
		dev_err(&client->dev, "DND H Gap Fail\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
				"Fail %d,%d,%d,%d\n",
				node_num, 0, dnd_h_gap[x][y], fail_val);
	}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	return;
}

static void get_dnd_h_gap(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_node, y_node;
	int node_num;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= x_num || y_node < 0 || y_node >= y_num - 1) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "NG");
		set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = (x_node * y_num) + y_node;

	sprintf(finfo->cmd_buff, "%d", raw_data->hgap_data[node_num]);
	set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
}

static void get_dnd_v_gap(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_node, y_node;
	int node_num;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= x_num - 1 || y_node < 0 || y_node >= y_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "NG");
		set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = (x_node * y_num) + y_node;

	sprintf(finfo->cmd_buff, "%d", raw_data->vgap_data[node_num]);
	set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
}

#define DEF_RAW_DATA_CNT	10
static void run_hfdnd_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset;
	bool result = true;

#if ESD_TIMER_INTERVAL
	esd_timer_stop(misc_info);
#endif
	set_default_result(info);

	for (i = 0; i < TSP_CMD_NODE_NUM; i++)
		raw_data->hfdnd_data_sum[i] = 0;

	ts_set_touchmode2(TOUCH_DND_MODE);
	for (i = 0; i < DEF_RAW_DATA_CNT; i++) {
		get_raw_data(info, (u8 *)raw_data->hfdnd_data, 2);
		write_cmd(misc_info->client, BT541_CLEAR_INT_STATUS_CMD);

		for (j = 0; j < TSP_CMD_NODE_NUM; j++)
			raw_data->hfdnd_data_sum[j] += raw_data->hfdnd_data[j];
	}
	ts_set_touchmode2(TOUCH_POINT_MODE);

	for (i = 0; i < TSP_CMD_NODE_NUM; i++)
		raw_data->hfdnd_data[i] = raw_data->hfdnd_data_sum[i] / DEF_RAW_DATA_CNT;

	printk("HF DND start\n");

	for (i = 0; i < x_num; i++) {
		for (j = 0; j < y_num; j++) {
			offset = (i * y_num) + j;
			printk("%d ", raw_data->hfdnd_data[offset]);
		}
		printk("\n");
	}

	if (result) {
		dev_info(&client->dev, "HF DND Pass\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, misc_info);
#endif
	return;
}


static void run_hfdnd_v_gap_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset, val, cur_val, next_val, x, y, node_num, fail_val;
	bool result = true;

	set_default_result(info);

	memset(raw_data->vgap_data, 0x00, TSP_CMD_NODE_NUM);

	printk("HF DND V Gap start\n");

	for (i = 0; i < x_num - 1; i++) {
		for (j = 0; j < y_num; j++) {
			offset = (i * y_num) + j;

			cur_val = raw_data->hfdnd_data[offset];
			next_val = raw_data->hfdnd_data[offset + y_num];
			if (!next_val) {
				raw_data->vgap_data[offset] = next_val;
				continue;
			}

			if (next_val > cur_val)
				val = 100 - ((cur_val * 100) / next_val);
	else
				val = 100 - ((next_val * 100) / cur_val);

			printk("%d ", val);
			cur_val = (s16)(hfdnd_v_gap[i][j]);

			if (val > cur_val) {
				fail_val = val;
				x = i;
				y = j;
				node_num = offset;
				result = false;
				}
			raw_data->vgap_data[offset] = val;
			}
		printk("\n");
			}

	if (result) {
		dev_info(&client->dev, "HF DND V Gap Pass\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	} else {
		dev_err(&client->dev, "HF DND V Gap Fail\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
				"Fail %d,%d,%d,%d\n",
				node_num, 0, hfdnd_v_gap[x][y], fail_val);
			}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	return;
}

static void run_hfdnd_h_gap_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;
	int i, j, offset, val, cur_val, next_val, x, y, node_num, fail_val;
	bool result = true;

	set_default_result(info);

	memset(raw_data->hgap_data, 0x00, TSP_CMD_NODE_NUM);

	printk("HF DND H Gap start\n");

	for (i = 0; i < x_num ; i++) {
		for (j = 0; j < y_num-1; j++) {
			offset = (i * y_num) + j;

			cur_val = raw_data->hfdnd_data[offset];
			if (!cur_val) {
				raw_data->hgap_data[offset] = cur_val;
				continue;
			}

			next_val = raw_data->hfdnd_data[offset + 1];
			if (!next_val) {
				raw_data->hgap_data[offset] = next_val;
				for (++j; j < y_num - 1; j++) {
					offset = (i * y_num) + j;

					next_val = raw_data->hfdnd_data[offset];
					if (!next_val) {
						raw_data->hgap_data[offset]
							= next_val;
						continue;
			}

					break;
		}
	}

			if (next_val > cur_val)
				val = 100 - ((cur_val * 100) / next_val);
	   else
				val = 100 - ((next_val * 100) / cur_val);

			printk("%d ", val);
			cur_val = (s16)(hfdnd_h_gap[i][j]);

			if (val > cur_val) {
				fail_val = val;
				x = i;
				y = j;
				node_num = offset;
			result = false;
			}
			raw_data->hgap_data[offset] = val;
		}
		printk("\n");
		}

	if (result) {
		dev_info(&client->dev, "HF DND H Gap Pass\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK\n");
	} else {
		dev_err(&client->dev, "HF DND H Gap Fail\n");
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
				"Fail %d,%d,%d,%d\n",
				node_num, 0, hfdnd_h_gap[x][y], fail_val);
	}

	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	return;
}

static void get_hfdnd_h_gap(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_node, y_node;
	int node_num;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= x_num || y_node < 0 || y_node >= y_num - 1) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "NG");
		set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = (x_node * y_num) + y_node;

	sprintf(finfo->cmd_buff, "%d", raw_data->hgap_data[node_num]);
	set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
}

static void get_hfdnd_v_gap(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	int x_node, y_node;
	int node_num;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= x_num - 1 || y_node < 0 || y_node >= y_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "NG");
		set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = (x_node * y_num) + y_node;

	sprintf(finfo->cmd_buff, "%d", raw_data->vgap_data[node_num]);
	set_cmd_result(info, finfo->cmd_buff, strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
}

static void get_reference(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
/*	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	unsigned int val;
	int x_node, y_node;
	int node_num;*/

	set_default_result(info);
/*
	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= info->cap_info.x_node_num ||
		y_node < 0 || y_node >= info->cap_info.y_node_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "abnormal");
		set_cmd_result(info, finfo->cmd_buff,
		strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = x_node * info->cap_info.y_node_num + y_node;

	val = raw_data->ref_data[node_num];
	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%u", val);
	set_cmd_result(info, finfo->cmd_buff,
	strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
*/
	return;
}

static void get_dnd(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	unsigned int val;
	int x_node, y_node;
	int node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= info->cap_info.x_node_num ||
		y_node < 0 || y_node >= info->cap_info.y_node_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "abnormal");
		set_cmd_result(info, finfo->cmd_buff,
		strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = x_node * info->cap_info.y_node_num + y_node;

	val = raw_data->dnd_data[node_num];
	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%u", val);
	set_cmd_result(info, finfo->cmd_buff,
	strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
		(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void get_hfdnd(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	unsigned int val;
	int x_node, y_node;
	int node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= info->cap_info.x_node_num ||
		y_node < 0 || y_node >= info->cap_info.y_node_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "abnormal");
		set_cmd_result(info, finfo->cmd_buff,
		strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;
		return;
	}

	node_num = x_node * info->cap_info.y_node_num + y_node;

	val = raw_data->hfdnd_data[node_num];
	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%u", val);
	set_cmd_result(info, finfo->cmd_buff,
	strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
		(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void run_delta_read(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	s16 min, max;
	s32 i,j, offset;
	int x_num = info->cap_info.x_node_num, y_num = info->cap_info.y_node_num;

#if ESD_TIMER_INTERVAL
	esd_timer_stop(misc_info);
#endif

	set_default_result(info);

	ts_set_touchmode(TOUCH_DELTA_MODE);
	get_raw_data(info, (u8 *)raw_data->delta_data, 10);
	ts_set_touchmode(TOUCH_POINT_MODE);

	min = (s16)0x7FFF;
	max = (s16)0x8000;

	for (i = 0; i < x_num; i++) {
		for (j = 0; j < y_num; j++) {
//			printk("delta_data : %d\n", raw_data->delta_data[j+i]);
			offset = (i * y_num) + j;
			printk("%d ", raw_data->delta_data[offset]);

			if (raw_data->delta_data[offset] < min &&
					raw_data->delta_data[offset] != 0)
				min = raw_data->delta_data[offset];

			if (raw_data->delta_data[offset] > max)
				max = raw_data->delta_data[offset];

		}
		printk("\n");
	}

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%d,%d\n", min, max);
	set_cmd_result(info, finfo->cmd_buff,
			strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: \"%s\"(%d)\n", __func__, finfo->cmd_buff,
			(int)strlen(finfo->cmd_buff));

#if ESD_TIMER_INTERVAL
	esd_timer_start(CHECK_ESD_TIMER, misc_info);
#endif

	return;
}

static void get_delta(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	struct tsp_raw_data *raw_data = info->raw_data;
	unsigned int val;
	int x_node, y_node;
	int node_num;

	set_default_result(info);

	x_node = finfo->cmd_param[0];
	y_node = finfo->cmd_param[1];

	if (x_node < 0 || x_node >= info->cap_info.x_node_num ||
		y_node < 0 || y_node >= info->cap_info.y_node_num) {
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "abnormal");
		set_cmd_result(info, finfo->cmd_buff,
		strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
		finfo->cmd_state = FAIL;

		return;
	}

	node_num = x_node * info->cap_info.y_node_num + y_node;

	val = raw_data->delta_data[node_num];
	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%u", val);
	set_cmd_result(info, finfo->cmd_buff,
	strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__, finfo->cmd_buff,
		(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));

	return;
}

static void hfdnd_spec_adjust(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	int test;

	set_default_result(info);

	test = finfo->cmd_param[0];

	if (test) {
		dnd_h_gap = assy_dnd_h_gap;
		dnd_v_gap = assy_dnd_v_gap;
		hfdnd_h_gap = assy_hfdnd_h_gap;
		hfdnd_v_gap = assy_hfdnd_v_gap;
		dnd_max = assy_dnd_max;
		dnd_min = assy_dnd_min;
	} else {
		dnd_h_gap = tsp_dnd_h_gap;
		dnd_v_gap = tsp_dnd_v_gap;
		hfdnd_h_gap = tsp_hfdnd_h_gap;
		hfdnd_v_gap = tsp_hfdnd_v_gap;
		dnd_max = tsp_dnd_max;
		dnd_min = tsp_dnd_min;
	}

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "OK");
	set_cmd_result(info, finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	return;
}

static void clear_reference_data(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	set_default_result(info);

#if ESD_TIMER_INTERVAL
	esd_timer_stop(info);
	write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL, 0);
#endif

	write_reg(client, BT541_EEPROM_INFO_REG, 0xffff);

	write_reg(client, 0xc003, 0x0001);
	write_reg(client, 0xc104, 0x0001);
	udelay(100);
	if (write_cmd(client, BT541_SAVE_STATUS_CMD) != I2C_SUCCESS)
		return;

	mdelay(500);
	write_reg(client, 0xc003, 0x0000);
	write_reg(client, 0xc104, 0x0000);
	udelay(100);

#if ESD_TIMER_INTERVAL
	write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL,
			SCAN_RATE_HZ * ESD_TIMER_INTERVAL);
	esd_timer_start(CHECK_ESD_TIMER, info);
#endif
	dev_info(&client->dev, "%s: TSP clear calibration bit\n", __func__);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "OK");
	set_cmd_result(info, finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	return;
}

static void run_ref_calibration(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	int i;

	set_default_result(info);

#if ESD_TIMER_INTERVAL
	esd_timer_stop(info);
	write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL, 0);
#endif

	write_reg(client, 0x01CA, 1300);
	if (ts_hw_calibration(info) == true)
		dev_info(&client->dev, "%s: TSP calibration Pass\n", __func__);
	else
		dev_info(&client->dev, "%s: TSP calibration Fail\n", __func__);

	write_reg(client, 0x01CA, 0);
	write_reg(client, 0x0149, 0);

	for (i = 0; i < 5; i++) {
		write_cmd(client, BT541_CLEAR_INT_STATUS_CMD);
		udelay(10);
	}

#if ESD_TIMER_INTERVAL
	write_reg(client, BT541_PERIODICAL_INTERRUPT_INTERVAL,
			SCAN_RATE_HZ * ESD_TIMER_INTERVAL);
	esd_timer_start(CHECK_ESD_TIMER, info);
#endif

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "%s", "OK");
	set_cmd_result(info, finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s: %s(%d)\n", __func__,
			finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	return;
}

#ifdef CONFIG_INPUT_BOOSTER
static void boost_level(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct i2c_client *client = info->client;
	char buf[16] = { 0 };
	u32 level = 0;

	set_default_result(info);

	level = finfo->cmd_param[0];

	change_booster_level_for_tsp(level);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "Boost level %d", level);
	set_cmd_result(info, finfo->cmd_buff, (int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s(), %s\n", __func__, finfo->cmd_buff);

	return;
}
#endif

static void dead_zone_enable(void *device_data)
{
	struct bt541_ts_info *info = (struct bt541_ts_info *)device_data;
	struct tsp_factory_info *finfo = info->factory_info;
	struct i2c_client *client = info->client;
	int val = finfo->cmd_param[0];

	set_default_result(info);

	if(val) //disable
		zinitix_bit_clr(m_optional_mode, 3);
	else //enable
		zinitix_bit_set(m_optional_mode, 3);

	snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff),
			"dead_zone %s", val ? "disable" : "enable");
	set_cmd_result(info, finfo->cmd_buff,
			(int)strnlen(finfo->cmd_buff, sizeof(finfo->cmd_buff)));
	finfo->cmd_state = OK;

	dev_info(&client->dev, "%s(), %s\n", __func__, finfo->cmd_buff);

	return;
}

static ssize_t store_cmd(struct device *dev, struct device_attribute
		*devattr, const char *buf, size_t count)
{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;
	char *cur, *start, *end;
	char buff[TSP_CMD_STR_LEN] = {0};
	int len, i;
	struct tsp_cmd *tsp_cmd_ptr = NULL;
	char delim = ',';
	bool cmd_found = false;
	int param_cnt = 0;

	if (finfo->cmd_is_running == true) {
		dev_err(&client->dev, "%s: other cmd is running\n", __func__);
		goto err_out;
	}

	/* check lock  */
	mutex_lock(&finfo->cmd_lock);
	finfo->cmd_is_running = true;
	mutex_unlock(&finfo->cmd_lock);

	finfo->cmd_state = RUNNING;

	for (i = 0; i < ARRAY_SIZE(finfo->cmd_param); i++)
		finfo->cmd_param[i] = 0;

	len = (int)count;
	if (*(buf + len - 1) == '\n')
		len--;

	memset(finfo->cmd, 0x00, ARRAY_SIZE(finfo->cmd));
	memcpy(finfo->cmd, buf, len);

	cur = strchr(buf, (int)delim);
	if (cur)
		memcpy(buff, buf, cur - buf);
	else
		memcpy(buff, buf, len);

	/* find command */
	list_for_each_entry(tsp_cmd_ptr, &finfo->cmd_list_head, list) {
		if (!strcmp(buff, tsp_cmd_ptr->cmd_name)) {
			cmd_found = true;
			break;
		}
	}

	/* set not_support_cmd */
	if (!cmd_found) {
		list_for_each_entry(tsp_cmd_ptr, &finfo->cmd_list_head, list) {
			if (!strcmp("not_support_cmd", tsp_cmd_ptr->cmd_name))
				break;
		}
	}

	/* parsing parameters */
	if (cur && cmd_found) {
		cur++;
		start = cur;
		memset(buff, 0x00, ARRAY_SIZE(buff));
		do {
			if (*cur == delim || cur - buf == len) {
				end = cur;
				memcpy(buff, start, end - start);
				*(buff + strlen(buff)) = '\0';
				finfo->cmd_param[param_cnt] =
					(int)simple_strtol(buff, NULL, 10);
				start = cur + 1;
				memset(buff, 0x00, ARRAY_SIZE(buff));
				param_cnt++;
			}
			cur++;
		} while (cur - buf <= len);
	}

	dev_info(&client->dev, "cmd = %s\n", tsp_cmd_ptr->cmd_name);
	/*	for (i = 0; i < param_cnt; i++)
		dev_info(&client->dev, "cmd param %d= %d\n", i, finfo->cmd_param[i]);*/

	tsp_cmd_ptr->cmd_func(info);

	mutex_lock(&finfo->cmd_lock);
	finfo->cmd_is_running = false;
	mutex_unlock(&finfo->cmd_lock);

err_out:
	return count;
}

static ssize_t show_cmd_status(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	dev_info(&client->dev, "tsp cmd: status:%d\n", finfo->cmd_state);

	if (finfo->cmd_state == WAITING)
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "WAITING");

	else if (finfo->cmd_state == RUNNING)
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "RUNNING");

	else if (finfo->cmd_state == OK)
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "OK");

	else if (finfo->cmd_state == FAIL)
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "FAIL");

	else if (finfo->cmd_state == NOT_APPLICABLE)
		snprintf(finfo->cmd_buff, sizeof(finfo->cmd_buff), "NOT_APPLICABLE");

	return snprintf(buf, sizeof(finfo->cmd_buff),
			"%s\n", finfo->cmd_buff);
}

static ssize_t show_cmd_result(struct device *dev, struct device_attribute
		*devattr, char *buf)
{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	struct tsp_factory_info *finfo = info->factory_info;

	dev_info(&client->dev, "tsp cmd: result: %s\n", finfo->cmd_result);

	finfo->cmd_state = WAITING;

	return snprintf(buf, sizeof(finfo->cmd_result),
			"%s\n", finfo->cmd_result);
}

/* sysfs: /sys/class/sec/tsp/input/enabled */
static ssize_t show_enabled(struct device *dev, struct device_attribute
		*devattr, char *buf)

{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;

	int val =0;

	if(info->work_state == EALRY_SUSPEND || info->work_state == SUSPEND)
		val =0;
	else
		val =1;

	dev_info(&client->dev, " enabled is %d\n", info->work_state);

	return snprintf(buf, sizeof(val),"%d\n", val);
}

static DEVICE_ATTR(cmd, S_IWUSR | S_IWGRP, NULL, store_cmd);
static DEVICE_ATTR(cmd_status, S_IRUGO, show_cmd_status, NULL);
static DEVICE_ATTR(cmd_result, S_IRUGO, show_cmd_result, NULL);
static DEVICE_ATTR(enabled, S_IRUGO, show_enabled, NULL);


static struct attribute *touchscreen_attributes[] = {
	&dev_attr_cmd.attr,
	&dev_attr_cmd_status.attr,
	&dev_attr_cmd_result.attr,
	NULL,
};

static struct attribute *sec_touch_pretest_attributes[] = {
	&dev_attr_enabled.attr,
	NULL,
};

static struct attribute_group touchscreen_attr_group = {
	.attrs = touchscreen_attributes,
};

static struct attribute_group sec_touch_pertest_attr_group = {
	.attrs	= sec_touch_pretest_attributes,
};


#ifdef SUPPORTED_TOUCH_KEY
static ssize_t show_touchkey_threshold(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	struct capa_info *cap = &(info->cap_info);

#ifdef NOT_SUPPORTED_TOUCH_DUMMY_KEY
	dev_info(&client->dev, "%s: key threshold = %d\n", __func__,
			cap->key_threshold);

	return snprintf(buf, 41, "%d", cap->key_threshold);
#else
	dev_info(&client->dev, "%s: key threshold = %d %d %d %d\n", __func__,
			cap->dummy_threshold, cap->key_threshold, cap->key_threshold, cap->dummy_threshold);

	return snprintf(buf, 41, "%d %d %d %d", cap->dummy_threshold,
			cap->key_threshold,  cap->key_threshold,
			cap->dummy_threshold);
#endif
}

static ssize_t show_touchkey_sensitivity(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bt541_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u16 val = 0;
	int ret = 0;
	int i;

#ifdef NOT_SUPPORTED_TOUCH_DUMMY_KEY
	if (!strcmp(attr->attr.name, "touchkey_recent"))
		i = 0;
	else if (!strcmp(attr->attr.name, "touchkey_back"))
		i = 1;
	else {
		dev_err(&client->dev, "%s: Invalid attribute\n", __func__);

		goto err_out;
	}

#else
	if (!strcmp(attr->attr.name, "touchkey_dummy_btn1"))
		i = 0;
	else if (!strcmp(attr->attr.name, "touchkey_recent"))
		i = 1;
	else if (!strcmp(attr->attr.name, "touchkey_back"))
		i = 2;
	else if (!strcmp(attr->attr.name, "touchkey_dummy_btn4"))
		i = 3;
	else if (!strcmp(attr->attr.name, "touchkey_dummy_btn5"))
		i = 4;
	else if (!strcmp(attr->attr.name, "touchkey_dummy_btn6"))
		i = 5;
	else {
		dev_err(&client->dev, "%s: Invalid attribute\n", __func__);

		goto err_out;
	}
#endif
	down(&info->work_lock);
	ret = read_data(client, BT541_BTN_WIDTH + i, (u8 *)&val, 2);
	up(&info->work_lock);
	if (ret < 0) {
		dev_err(&client->dev, "%s: Failed to read %d's key sensitivity\n",
				__func__, i);

		goto err_out;
	}

	dev_info(&client->dev, "%s: %d's key sensitivity = %d\n",
			__func__, i, val);

	return snprintf(buf, 6, "%d", val);

err_out:
	return sprintf(buf, "NG");
}

static ssize_t show_back_key_raw_data(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t show_menu_key_raw_data(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

static DEVICE_ATTR(touchkey_threshold, S_IRUGO, show_touchkey_threshold, NULL);
static DEVICE_ATTR(touchkey_recent, S_IRUGO, show_touchkey_sensitivity, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO, show_touchkey_sensitivity, NULL);
#ifndef NOT_SUPPORTED_TOUCH_DUMMY_KEY
static DEVICE_ATTR(touchkey_dummy_btn1, S_IRUGO,
		show_touchkey_sensitivity, NULL);
static DEVICE_ATTR(touchkey_dummy_btn3, S_IRUGO,
		show_touchkey_sensitivity, NULL);
static DEVICE_ATTR(touchkey_dummy_btn4, S_IRUGO,
		show_touchkey_sensitivity, NULL);
static DEVICE_ATTR(touchkey_dummy_btn6, S_IRUGO,
		show_touchkey_sensitivity, NULL);
#endif
static DEVICE_ATTR(touchkey_raw_back, S_IRUGO, show_back_key_raw_data, NULL);
static DEVICE_ATTR(touchkey_raw_menu, S_IRUGO, show_menu_key_raw_data, NULL);

static struct attribute *touchkey_attributes[] = {
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_raw_menu.attr,
	&dev_attr_touchkey_raw_back.attr,
#ifndef NOT_SUPPORTED_TOUCH_DUMMY_KEY
	&dev_attr_touchkey_dummy_btn1.attr,
	&dev_attr_touchkey_dummy_btn3.attr,
	&dev_attr_touchkey_dummy_btn4.attr,
	&dev_attr_touchkey_dummy_btn6.attr,
#endif
	NULL,
};
static struct attribute_group touchkey_attr_group = {
	.attrs = touchkey_attributes,
};
#endif

static int init_sec_factory(struct bt541_ts_info *info)
{
	struct device *factory_ts_dev;
#ifdef SUPPORTED_TOUCH_KEY
	struct device *factory_tk_dev;
#endif
	struct device *sec_pretest_dev;
	struct tsp_factory_info *factory_info;
	struct tsp_raw_data *raw_data;
	int ret = 0;
	int i;

	factory_info = kzalloc(sizeof(struct tsp_factory_info), GFP_KERNEL);
	if (unlikely(!factory_info)) {
		dev_err(&info->client->dev, "%s: Failed to allocate memory\n",
				__func__);
		ret = -ENOMEM;

		goto err_alloc1;
	}
	raw_data = kzalloc(sizeof(struct tsp_raw_data), GFP_KERNEL);
	if (unlikely(!raw_data)) {
		dev_err(&info->client->dev, "%s: Failed to allocate memory\n",
				__func__);
		ret = -ENOMEM;

		goto err_alloc2;
	}

	INIT_LIST_HEAD(&factory_info->cmd_list_head);
	for (i = 0; i < ARRAY_SIZE(tsp_cmds); i++)
		list_add_tail(&tsp_cmds[i].list, &factory_info->cmd_list_head);

	factory_ts_dev = device_create(sec_class, NULL, 0, info, "tsp");
	if (unlikely(!factory_ts_dev)) {
		dev_err(&info->client->dev, "Failed to create factory dev\n");
		ret = -ENODEV;
		goto err_create_device;
	}

#ifdef SUPPORTED_TOUCH_KEY
	factory_tk_dev = device_create(sec_class, NULL, 0, info, "sec_touchkey");
	if (IS_ERR(factory_tk_dev)) {
		dev_err(&info->client->dev, "Failed to create factory dev\n");
		ret = -ENODEV;
		goto err_create_device;
	}
#endif

	/* /sys/class/sec/tsp/input/ */
	sec_pretest_dev = device_create(sec_class, factory_ts_dev, 4, info, "input");
	if (IS_ERR(sec_pretest_dev)) {
		dev_err(&info->client->dev, "Failed to create device (%s)!\n", "tsp");
		goto err_create_device;
	}

	ret = sysfs_create_group(&factory_ts_dev->kobj, &touchscreen_attr_group);
	if (unlikely(ret)) {
		dev_err(&info->client->dev, "Failed to create touchscreen sysfs group\n");
		goto err_create_sysfs;
	}

#ifdef SUPPORTED_TOUCH_KEY
	ret = sysfs_create_group(&factory_tk_dev->kobj, &touchkey_attr_group);
	if (unlikely(ret)) {
		dev_err(&info->client->dev, "Failed to create touchkey sysfs group\n");
		goto err_create_sysfs;
	}
#endif

	/* /sys/class/sec/tsp/... */
	if (sysfs_create_group(&sec_pretest_dev->kobj, &sec_touch_pertest_attr_group)) {
		dev_err(&info->client->dev, "Failed to create sysfs group(%s)!\n", "input");
		goto err_create_sysfs;
	}

	mutex_init(&factory_info->cmd_lock);
	factory_info->cmd_is_running = false;

	info->factory_info = factory_info;
	info->raw_data = raw_data;

	dnd_h_gap = assy_dnd_h_gap;
	dnd_v_gap = assy_dnd_v_gap;
	hfdnd_h_gap = assy_hfdnd_h_gap;
	hfdnd_v_gap = assy_hfdnd_v_gap;
	dnd_max = assy_dnd_max;
	dnd_min = assy_dnd_min;

	return ret;

err_create_sysfs:
err_create_device:
	kfree(raw_data);
err_alloc2:
	kfree(factory_info);
err_alloc1:

	return ret;
}
#endif

static int ts_misc_fops_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int ts_misc_fops_close(struct inode *inode, struct file *filp)
{
	return 0;
}

static long ts_misc_fops_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct raw_ioctl raw_ioctl;
	u8 *u8Data;
	int ret = 0;
	size_t sz = 0;
	u16 version;
	u16 mode;

	struct reg_ioctl reg_ioctl;
	u16 val;
	int nval = 0;

	if (misc_info == NULL)
	{
		zinitix_debug_msg("misc device NULL?\n");
		return -1;
	}

	switch (cmd) {

	case TOUCH_IOCTL_GET_DEBUGMSG_STATE:
		ret = m_ts_debug_mode;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_SET_DEBUGMSG_STATE:
		if (copy_from_user(&nval, argp, 4)) {
			pr_info("[zinitix_touch] error : copy_from_user\n");
			return -1;
		}
		if (nval)
			pr_info("[zinitix_touch] on debug mode (%d)\n",
					nval);
		else
			pr_info("[zinitix_touch] off debug mode (%d)\n",
					nval);
		m_ts_debug_mode = nval;
		break;

	case TOUCH_IOCTL_GET_CHIP_REVISION:
		ret = misc_info->cap_info.ic_revision;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_FW_VERSION:
		ret = misc_info->cap_info.fw_version;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_REG_DATA_VERSION:
		ret = misc_info->cap_info.reg_data_version;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_VARIFY_UPGRADE_SIZE:
		if (copy_from_user(&sz, argp, sizeof(size_t)))
			return -1;

		printk(KERN_INFO "[zinitix_touch]: firmware size = %d, %d\r\n", (int)sz, misc_info->cap_info.ic_fw_size);
		if (misc_info->cap_info.ic_fw_size != sz) {
			pr_info("[zinitix_touch]: firmware size error\r\n");
			return -1;
		}
		break;

	case TOUCH_IOCTL_VARIFY_UPGRADE_DATA:
		if (copy_from_user(m_firmware_data,
					argp, misc_info->cap_info.ic_fw_size))
			return -1;

		version = (u16) (m_firmware_data[52] | (m_firmware_data[53]<<8));

		pr_info("[zinitix_touch]: firmware version = %x\r\n", version);

		if (copy_to_user(argp, &version, sizeof(version)))
			return -1;
		break;

	case TOUCH_IOCTL_START_UPGRADE:
		return ts_upgrade_sequence((u8 *)m_firmware_data);

	case TOUCH_IOCTL_GET_X_RESOLUTION:
		ret = misc_info->pdata->x_resolution;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_Y_RESOLUTION:
		ret = misc_info->pdata->y_resolution;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_X_NODE_NUM:
		ret = misc_info->cap_info.x_node_num;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_Y_NODE_NUM:
		ret = misc_info->cap_info.y_node_num;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_GET_TOTAL_NODE_NUM:
		ret = misc_info->cap_info.total_node_num;
		if (copy_to_user(argp, &ret, sizeof(ret)))
			return -1;
		break;

	case TOUCH_IOCTL_HW_CALIBRAION:
		ret = -1;
		disable_irq(misc_info->irq);
		down(&misc_info->work_lock);
		if (misc_info->work_state != NOTHING) {
			pr_info("[zinitix_touch]: other process occupied.. (%d)\r\n",
					misc_info->work_state);
			up(&misc_info->work_lock);
			return -1;
		}
		misc_info->work_state = HW_CALIBRAION;
		mdelay(100);

		/* h/w calibration */
		if (ts_hw_calibration(misc_info) == true)
			ret = 0;

		mode = misc_info->touch_mode;
		if (write_reg(misc_info->client,
					BT541_TOUCH_MODE, mode) != I2C_SUCCESS) {
			pr_err("[zinitix_touch]: failed to set touch mode %d.\n",
					mode);
			goto fail_hw_cal;
		}

		if (write_cmd(misc_info->client,
					BT541_SWRESET_CMD) != I2C_SUCCESS)
			goto fail_hw_cal;

		enable_irq(misc_info->irq);
		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return ret;
fail_hw_cal:
		enable_irq(misc_info->irq);
		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return -1;

	case TOUCH_IOCTL_SET_RAW_DATA_MODE:
		if (misc_info == NULL) {
			zinitix_debug_msg("misc device NULL?\n");
			return -1;
		}
		if (copy_from_user(&nval, argp, 4)) {
			pr_info("[zinitix_touch] error : copy_from_user\r\n");
			misc_info->work_state = NOTHING;
			return -1;
		}
		ts_set_touchmode((u16)nval);

		return 0;

	case TOUCH_IOCTL_GET_REG:
		if (misc_info == NULL) {
			zinitix_debug_msg("misc device NULL?\n");
			return -1;
		}
		down(&misc_info->work_lock);
		if (misc_info->work_state != NOTHING) {
			pr_info("[zinitix_touch]:other process occupied.. (%d)\n",
					misc_info->work_state);
			up(&misc_info->work_lock);
			return -1;
		}

		misc_info->work_state = SET_MODE;

		if (copy_from_user(&reg_ioctl,
					argp, sizeof(struct reg_ioctl))) {
			misc_info->work_state = NOTHING;
			up(&misc_info->work_lock);
			pr_info("[zinitix_touch] error : copy_from_user\n");
			return -1;
		}

		if (read_data(misc_info->client,
					(u16)reg_ioctl.addr, (u8 *)&val, 2) < 0)
			ret = -1;

		nval = (int)val;

		if (copy_to_user((void *)(unsigned long)reg_ioctl.val, (u8 *)&nval, 4)) {
			misc_info->work_state = NOTHING;
			up(&misc_info->work_lock);
			pr_info("[zinitix_touch] error : copy_to_user\n");
			return -1;
		}

		zinitix_debug_msg("read : reg addr = 0x%x, val = 0x%x\n",
				reg_ioctl.addr, nval);

		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return ret;

	case TOUCH_IOCTL_SET_REG:

		down(&misc_info->work_lock);
		if (misc_info->work_state != NOTHING) {
			pr_info("[zinitix_touch]: other process occupied.. (%d)\n",
					misc_info->work_state);
			up(&misc_info->work_lock);
			return -1;
		}

		misc_info->work_state = SET_MODE;
		if (copy_from_user(&reg_ioctl,
					argp, sizeof(struct reg_ioctl))) {
			misc_info->work_state = NOTHING;
			up(&misc_info->work_lock);
			pr_info("[zinitix_touch] error : copy_from_user(1)\n");
			return -1;
		}

		if (copy_from_user(&val, (void *)(unsigned long)reg_ioctl.val, 4)) {
			misc_info->work_state = NOTHING;
			up(&misc_info->work_lock);
			pr_info("[zinitix_touch] error : copy_from_user(2)\n");
			return -1;
		}

		if (write_reg(misc_info->client,
					(u16)reg_ioctl.addr, val) != I2C_SUCCESS)
			ret = -1;

		zinitix_debug_msg("write : reg addr = 0x%x, val = 0x%x\r\n",
				reg_ioctl.addr, val);
		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return ret;

	case TOUCH_IOCTL_DONOT_TOUCH_EVENT:

		if (misc_info == NULL) {
			zinitix_debug_msg("misc device NULL?\n");
			return -1;
		}
		down(&misc_info->work_lock);
		if (misc_info->work_state != NOTHING) {
			pr_info("[zinitix_touch]: other process occupied.. (%d)\r\n",
					misc_info->work_state);
			up(&misc_info->work_lock);
			return -1;
		}

		misc_info->work_state = SET_MODE;
		if (write_reg(misc_info->client,
					BT541_INT_ENABLE_FLAG, 0) != I2C_SUCCESS)
			ret = -1;
		zinitix_debug_msg("write : reg addr = 0x%x, val = 0x0\r\n",
				BT541_INT_ENABLE_FLAG);

		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return ret;

	case TOUCH_IOCTL_SEND_SAVE_STATUS:
		if (misc_info == NULL) {
			zinitix_debug_msg("misc device NULL?\n");
			return -1;
		}
		down(&misc_info->work_lock);
		if (misc_info->work_state != NOTHING) {
			pr_info("[zinitix_touch]: other process occupied.." \
					"(%d)\r\n", misc_info->work_state);
			up(&misc_info->work_lock);
			return -1;
		}
		misc_info->work_state = SET_MODE;
		ret = 0;
		write_reg(misc_info->client, 0xc003, 0x0001);
		write_reg(misc_info->client, 0xc104, 0x0001);
		if (write_cmd(misc_info->client,
					BT541_SAVE_STATUS_CMD) != I2C_SUCCESS)
			ret =  -1;

		mdelay(1000);	/* for fusing eeprom */
		write_reg(misc_info->client, 0xc003, 0x0000);
		write_reg(misc_info->client, 0xc104, 0x0000);

		misc_info->work_state = NOTHING;
		up(&misc_info->work_lock);
		return ret;

	case TOUCH_IOCTL_GET_RAW_DATA:
		if (misc_info == NULL) {
			zinitix_debug_msg("misc device NULL?\n");
			return -1;
		}

		if (misc_info->touch_mode == TOUCH_POINT_MODE)
			return -1;

		down(&misc_info->raw_data_lock);
		if (misc_info->update == 0) {
			up(&misc_info->raw_data_lock);
			return -2;
		}

		if (copy_from_user(&raw_ioctl,
					argp, sizeof(struct raw_ioctl))) {
			up(&misc_info->raw_data_lock);
			pr_info("[zinitix_touch] error : copy_from_user\r\n");
			return -1;
		}

		misc_info->update = 0;

		u8Data = (u8 *)&misc_info->cur_data[0];
		if (raw_ioctl.sz > MAX_TRAW_DATA_SZ*2)
			raw_ioctl.sz = MAX_TRAW_DATA_SZ*2;
		if (copy_to_user((void *)(unsigned long)raw_ioctl.buf, (u8 *)u8Data,
					raw_ioctl.sz)) {
			up(&misc_info->raw_data_lock);
			return -1;
		}

		up(&misc_info->raw_data_lock);
		return 0;

	default:
		break;
	}
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id tsp_dt_ids[] = {
	{ .compatible = "Zinitix,bt541_ts", },
	{},
};
MODULE_DEVICE_TABLE(of, tsp_dt_ids);
#endif
static int bt541_ts_probe_dt(struct device_node *np,
		struct device *dev,
		struct bt541_ts_platform_data *pdata)
{
	int ret;

	if (!np)
		return -EINVAL;

	pdata->gpio_int = of_get_named_gpio(np,"gpios", 0);
	if (pdata->gpio_int < 0) {
		dev_err(dev, "%s: of_get_named_gpio failed: %d\n", __func__,
				pdata->gpio_int);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "bt541,x_resolution",
			&pdata->x_resolution);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(np, "bt541,y_resolution",
			&pdata->y_resolution);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(np, "bt541,orientation",
			&pdata->orientation);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(np, "bt541,page_size", &pdata->page_size);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(np, "tsp_vdd_supply_type",
			&pdata->tsp_supply_type);
	if (ret < 0) {
		dev_err(dev, "%s: failed to read property tsp_vdd_supply_type\n",
				__func__);
		return ret;
	}

	if (pdata->tsp_supply_type == TSP_LDO_SUPPLY) {
		pdata->gpio_ldo_en = of_get_named_gpio(np, "gpio_ldo_en", 0);
		if (pdata->gpio_ldo_en < 0) {
			dev_err(dev, "%s: of_get_named_gpio failed: %d\n", __func__,
					pdata->gpio_ldo_en);
			return -EINVAL;
		}
	}

	pdata->tsp_power = bt541_power;

	return 0;

}

static int bt541_ts_probe(struct i2c_client *client,
		const struct i2c_device_id *i2c_id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct bt541_ts_platform_data *pdata = client->dev.platform_data;
	struct bt541_ts_info *info;
	struct input_dev *input_dev;
	int ret = 0;
	int i;

	struct device_node *np = client->dev.of_node;

	if (boot_panel_id == 0) {
		dev_err(&client->dev, "[%s]: There is no TSP device!\n", __FUNCTION__);
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_OF)) {
		if (!pdata) {
			pdata = devm_kzalloc(&client->dev,
					sizeof(*pdata), GFP_KERNEL);
			if (!pdata)
				return -ENOMEM;
		}
		ret = bt541_ts_probe_dt(np, &client->dev, pdata);
		if (ret)
			goto err_no_platform_data;

	} else if (!pdata) {
		dev_err(&client->dev, "Not exist platform data\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "Not compatible i2c function\n");
		ret = -EIO;
		goto err_no_platform_data;
	}

	info = kzalloc(sizeof(struct bt541_ts_info), GFP_KERNEL);
	if (!info) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_no_platform_data;
	}

	ret = gpio_request(pdata->gpio_int, "bt541_int");
	if (ret < 0) {
		pr_err("%s: Request GPIO failed, gpio %d (%d)\n", BT541_TS_DEVICE,
				pdata->gpio_int, ret);
		goto err_gpio_alloc;
	}

	gpio_direction_input(pdata->gpio_int);
	i2c_set_clientdata(client, info);
	info->client = client;
	info->pdata = pdata;

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&client->dev, "Failed to allocate input device\n");
		ret = -EPERM;
		goto err_alloc;
	}

	info->input_dev = input_dev;
	info->work_state = PROBE;

	/* power on */
	if (bt541_power_control(info, POWER_ON_SEQUENCE) == false) {
		ret = -EPERM;
		goto err_power_sequence;
	}

	/* To Do */
	/* FW version read from tsp */

	memset(&info->reported_touch_info,
			0x0, sizeof(struct point_info));

	/* init touch mode */
	info->touch_mode = TOUCH_POINT_MODE;
	misc_info = info;

	if (init_touch(info, true) == false) {
		ret = -EPERM;
		goto err_input_unregister_device;
	}

	for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++)
		info->button[i] = ICON_BUTTON_UNCHANGE;

	snprintf(info->phys, sizeof(info->phys),
			"%s/input0", dev_name(&client->dev));
	input_dev->name = "sec_touchscreen";
	input_dev->id.bustype = BUS_I2C;
	/*	input_dev->id.vendor = 0x0001; */
	input_dev->phys = info->phys;
	/*	input_dev->id.product = 0x0002; */
	/*	input_dev->id.version = 0x0100; */
	input_dev->dev.parent = &client->dev;

	set_bit(EV_SYN, info->input_dev->evbit);
	set_bit(EV_KEY, info->input_dev->evbit);
	set_bit(EV_ABS, info->input_dev->evbit);
	set_bit(INPUT_PROP_DIRECT, info->input_dev->propbit);

	for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++)
		set_bit(BUTTON_MAPPING_KEY[i], info->input_dev->keybit);

	if (pdata->orientation & TOUCH_XY_SWAP) {
		input_set_abs_params(info->input_dev, ABS_MT_POSITION_Y,
				info->cap_info.MinX,
				info->cap_info.MaxX + ABS_PT_OFFSET,
				0, 0);
		input_set_abs_params(info->input_dev, ABS_MT_POSITION_X,
				info->cap_info.MinY,
				info->cap_info.MaxY + ABS_PT_OFFSET,
				0, 0);
	} else {
		input_set_abs_params(info->input_dev, ABS_MT_POSITION_X,
				info->cap_info.MinX,
				info->cap_info.MaxX + ABS_PT_OFFSET,
				0, 0);
		input_set_abs_params(info->input_dev, ABS_MT_POSITION_Y,
				info->cap_info.MinY,
				info->cap_info.MaxY + ABS_PT_OFFSET,
				0, 0);
	}

	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MAJOR,
			0, 255, 0, 0);
#ifdef CONFIG_SEC_FACTORY
	input_set_abs_params(info->input_dev, ABS_MT_PRESSURE,
			0, 3000, 0, 0);
#endif
	input_set_abs_params(info->input_dev, ABS_MT_WIDTH_MAJOR,
			0, 255, 0, 0);

#if (TOUCH_POINT_MODE == 2)
	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MINOR,
			0, 255, 0, 0);
	/*	input_set_abs_params(info->input_dev, ABS_MT_WIDTH_MINOR,
		0, 255, 0, 0); */
	/*	input_set_abs_params(info->input_dev, ABS_MT_ORIENTATION,
		-128, 127, 0, 0);  */
	/*	input_set_abs_params(info->input_dev, ABS_MT_ANGLE,
		-90, 90, 0, 0);*/
	input_set_abs_params(info->input_dev, ABS_MT_PALM,
			0, 1, 0, 0);
#endif

	set_bit(MT_TOOL_FINGER, info->input_dev->keybit);
	input_mt_init_slots(info->input_dev, info->cap_info.multi_fingers,0);

	zinitix_debug_msg("register %s input device \r\n",
			info->input_dev->name);
	input_set_drvdata(info->input_dev, info);
	ret = input_register_device(info->input_dev);
	if (ret) {
		printk(KERN_ERR "unable to register %s input device\r\n",
				info->input_dev->name);
		goto err_input_register_device;
	}

	/* configure irq */
	info->irq = gpio_to_irq(pdata->gpio_int);
	if (info->irq < 0)
		printk(KERN_INFO "error. gpio_to_irq(..) function is not \
				supported? you should define GPIO_TOUCH_IRQ.\r\n");

	zinitix_debug_msg("request irq (irq = %d, pin = %d) \r\n",
			info->irq, pdata->gpio_int);

	info->work_state = NOTHING;
#if TOUCH_BOOSTER || defined(CONFIG_INPUT_BOOSTER)
	info->finger_cnt = 0;
#endif
	sema_init(&info->work_lock, 1);

#if ESD_TIMER_INTERVAL
	spin_lock_init(&info->lock);
	INIT_WORK(&info->tmr_work, ts_tmr_work);
	esd_tmr_workqueue =
		create_singlethread_workqueue("esd_tmr_workqueue");

	if (!esd_tmr_workqueue) {
		dev_err(&client->dev, "Failed to create esd tmr work queue\n");
		ret = -EPERM;

		goto err_esd_input_unregister_device;
	}

	esd_timer_init(info);
	esd_timer_start(CHECK_ESD_TIMER, info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Started esd timer\n");
#endif
#endif
	ret = request_threaded_irq(info->irq, NULL, bt541_touch_work,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT , BT541_TS_DEVICE, info);

	if (ret) {
		printk(KERN_ERR "unable to register irq.(%s)\r\n",
				info->input_dev->name);
		goto err_request_irq;
	}
	dev_info(&client->dev, "zinitix touch probe.\r\n");
#ifdef CONFIG_HAS_EARLYSUSPEND
	info->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	info->early_suspend.suspend = bt541_ts_early_suspend;
	info->early_suspend.resume = bt541_ts_late_resume;
	register_early_suspend(&info->early_suspend);
#endif

#if defined(CONFIG_PM_RUNTIME) && !defined(CONFIG_HAS_EARLYSUSPEND)
	pm_runtime_enable(&client->dev);
#endif

	sema_init(&info->raw_data_lock, 1);

	ret = misc_register(&touch_misc_device);
	if (ret) {
		dev_err(&client->dev, "Failed to register touch misc device\n");
		goto err_misc_register;
	}

#ifdef SEC_FACTORY_TEST
	ret = init_sec_factory(info);
	if (ret) {
		dev_err(&client->dev, "Failed to init sec factory device\n");

		goto err_kthread_create_failed;
	}
#endif

	return 0;

#ifdef SEC_FACTORY_TEST
err_kthread_create_failed:
	kfree(info->factory_info);
	kfree(info->raw_data);
#endif
err_misc_register:
	free_irq(info->irq, info);
err_request_irq:
#if ESD_TIMER_INTERVAL
err_esd_input_unregister_device:
#endif
err_input_unregister_device:
	input_unregister_device(info->input_dev);
err_input_register_device:
	tsp_charger_status_cb = NULL;
err_power_sequence:
	input_free_device(info->input_dev);
err_alloc:
	kfree(info);
err_gpio_alloc:
	gpio_free(pdata->gpio_int);
err_no_platform_data:
	if (IS_ENABLED(CONFIG_OF))
		devm_kfree(&client->dev, (void *)pdata);

	dev_info(&client->dev, "Failed to probe\n");
	return ret;
}

static int bt541_ts_remove(struct i2c_client *client)
{
	struct bt541_ts_info *info = i2c_get_clientdata(client);
	struct bt541_ts_platform_data *pdata = info->pdata;

	disable_irq(info->irq);
	down(&info->work_lock);

	info->work_state = REMOVE;

#ifdef SEC_FACTORY_TEST
	kfree(info->factory_info);
	kfree(info->raw_data);
#endif
#if ESD_TIMER_INTERVAL
	flush_work(&info->tmr_work);
	write_reg(info->client, BT541_PERIODICAL_INTERRUPT_INTERVAL, 0);
	esd_timer_stop(info);
#if defined(TSP_VERBOSE_DEBUG)
	dev_info(&client->dev, "Stopped esd timer\n");
#endif
	destroy_workqueue(esd_tmr_workqueue);
#endif

	if (info->irq)
		free_irq(info->irq, info);

	misc_deregister(&touch_misc_device);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&info->early_suspend);
#endif

	if (gpio_is_valid(pdata->gpio_int) != 0)
		gpio_free(pdata->gpio_int);

	tsp_charger_status_cb = NULL;

	input_unregister_device(info->input_dev);
	input_free_device(info->input_dev);
	up(&info->work_lock);
	kfree(info);

	return 0;
}

void bt541_ts_shutdown(struct i2c_client *client)
{
	struct bt541_ts_info *info = i2c_get_clientdata(client);

	dev_info(&client->dev, "%s++\n",__func__);
	disable_irq(info->irq);
	down(&info->work_lock);
#if ESD_TIMER_INTERVAL
	flush_work(&info->tmr_work);
	esd_timer_stop(info);
#endif
	up(&info->work_lock);
	bt541_power_control(info, POWER_OFF);
	dev_info(&client->dev, "%s--\n",__func__);
}


static struct i2c_device_id bt541_idtable[] = {
	{BT541_TS_DEVICE, 0},
	{ }
};

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static const struct dev_pm_ops bt541_ts_pm_ops ={
#if defined(CONFIG_PM_RUNTIME)
	SET_RUNTIME_PM_OPS(bt541_ts_suspend, bt541_ts_resume,NULL)
#else
		SET_SYSTEM_SLEEP_PM_OPS(bt541_ts_suspend, bt541_ts_resume)
#endif
};
#endif

static struct i2c_driver bt541_ts_driver = {
	.probe		= bt541_ts_probe,
	.remove		= bt541_ts_remove,
	.shutdown	= bt541_ts_shutdown,
	.id_table	= bt541_idtable,
	.driver		= {
		.owner		= THIS_MODULE,
		.name		= BT541_TS_DEVICE,
		.of_match_table	= tsp_dt_ids,
#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
		.pm		= &bt541_ts_pm_ops,
#endif
	},
};

extern unsigned int lpcharge;

static void __init bt541_ts_init_async(void *dummy, async_cookie_t cookie)
{
	return i2c_add_driver(&bt541_ts_driver);
}

static int __init bt541_ts_init(void)
{
	if (!lpcharge)
		async_schedule(bt541_ts_init_async, NULL);
	else
		return 0;
}

static void __exit bt541_ts_exit(void)
{
	i2c_del_driver(&bt541_ts_driver);
}

module_init(bt541_ts_init);
module_exit(bt541_ts_exit);

#ifdef CONFIG_EXTCON_SM5504
static int extcon_notifier(struct notifier_block *nb,
	unsigned long state, void *edev)
{
	tsp_charger_enable(extcon_get_state(edev));

	return NOTIFY_OK;
}

static int ta_init_detect(void)
{
	struct extcon_dev *edev = NULL;
	static struct notifier_block nb;

	if (!misc_info)
		return -EPERM;

	edev = extcon_get_extcon_dev("sm5504");

	if (!edev) {
		pr_err("Failed to get extcon dev\n");
		return -ENXIO;
	}

	nb.notifier_call = extcon_notifier;

	extcon_register_notifier(edev, &nb);

	return 0;
}
late_initcall(ta_init_detect);
#endif

MODULE_DESCRIPTION("touch-screen device driver using i2c interface");
MODULE_LICENSE("GPL");
