/* drivers/gpu/t6xx/kbase/src/platform/mali_kbase_dvfs.c
 *
 * Copyright 2011 by S.LSI. Samsung Electronics Inc.
 * San#24, Nongseo-Dong, Giheung-Gu, Yongin, Korea
 *
 * Samsung SoC Mali-T604 DVFS driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software FoundatIon.
 */

/**
 * @file mali_kbase_dvfs.c
 * DVFS
 */

#include <osk/mali_osk.h>
#include <kbase/src/common/mali_kbase.h>
#include <kbase/src/common/mali_kbase_uku.h>
#include <kbase/src/common/mali_kbase_mem.h>
#include <kbase/src/common/mali_midg_regmap.h>
#include <kbase/src/linux/mali_kbase_mem_linux.h>
#include <kbase/mali_ukk.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm_qos.h>

#include <mach/map.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <mach/regs-clock.h>
#include <asm/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <kbase/src/platform/mali_kbase_platform.h>
#include <kbase/src/platform/mali_kbase_dvfs.h>
#include <kbase/src/common/mali_kbase_gator.h>
#ifdef CONFIG_EXYNOS5_CPUFREQ
#include <mach/cpufreq.h>
#endif

#ifdef MALI_DVFS_ASV_ENABLE
#include <mach/asv-exynos.h>
#define ASV_STATUS_INIT 1
#define ASV_STATUS_NOT_INIT 0
#define ASV_STATUS_DISABLE_REQ 2

#define ASV_CMD_DISABLE	-1
#define ASV_CMD_ENABLE 0
#endif

#ifdef CONFIG_REGULATOR
static struct regulator *g3d_regulator=NULL;
static int mali_gpu_vol = 1250000; /* 1.25V @ 533 MHz */
#endif

static struct pm_qos_request	mem_bw_req;

/***********************************************************/
/*  This table and variable are using the check time share of GPU Clock  */
/***********************************************************/

typedef struct _mali_dvfs_info{
	unsigned int voltage;
	unsigned int clock;
	int min_threshold;
	int	max_threshold;
	unsigned long long time;
}mali_dvfs_info;

static mali_dvfs_info mali_dvfs_infotbl[] = {
	{912500, 100, 0, 70, 0},
	{925000, 160, 50, 65, 0},
	{1025000, 266, 60, 78, 0},
	{1075000, 350, 70, 80, 0},
	{1125000, 400, 70, 80, 0},
	{1150000, 450, 76, 99, 0},
	{1250000, 533, 99, 100, 0},
};

#define MALI_DVFS_STEP	ARRAY_SIZE(mali_dvfs_infotbl)

#ifdef CONFIG_MALI_T6XX_DVFS
typedef struct _mali_dvfs_status_type{
	kbase_device *kbdev;
	int step;
	int utilisation;
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	int upper_lock;
	int under_lock;
#endif
#ifdef MALI_DVFS_ASV_ENABLE
	int asv_status;
#endif
}mali_dvfs_status;

static struct workqueue_struct *mali_dvfs_wq = 0;
osk_spinlock mali_dvfs_spinlock;
struct mutex mali_set_clock_lock;
struct mutex mali_enable_clock_lock;
#ifdef CONFIG_MALI_T6XX_DEBUG_SYS
static void update_time_in_state(int level);
#endif

/*dvfs status*/
static mali_dvfs_status mali_dvfs_status_current;
#ifdef MALI_DVFS_ASV_ENABLE
static const unsigned int mali_dvfs_vol_default[]=
	{ 925000, 925000, 1025000, 1075000, 1125000, 1150000, 1200000};

static int mali_dvfs_update_asv(int cmd)
{
	int i;
	int voltage = 0;

	if (cmd == ASV_CMD_DISABLE) {
		for (i=0; i<MALI_DVFS_STEP; i++)
		{
			mali_dvfs_infotbl[i].voltage = mali_dvfs_vol_default[i];
		}
		printk("mali_dvfs_update_asv use default table\n");
		return ASV_STATUS_INIT;
	}
	for (i=0; i<MALI_DVFS_STEP; i++) {
		voltage = asv_get_volt(ID_G3D, mali_dvfs_infotbl[i].clock*1000);
		if (voltage == 0) {
			return ASV_STATUS_NOT_INIT;
		}
		mali_dvfs_infotbl[i].voltage = voltage;
	}

	return ASV_STATUS_INIT;
}
#endif

static void mali_dvfs_event_proc(struct work_struct *w)
{
	mali_dvfs_status *dvfs_status;
	struct exynos_context *platform;

	mutex_lock(&mali_enable_clock_lock);
	dvfs_status = &mali_dvfs_status_current;

	if (!kbase_platform_dvfs_get_enable_status()) {
		mutex_unlock(&mali_enable_clock_lock);
		return;
	}

	platform = (struct exynos_context *)dvfs_status->kbdev->platform_context;
#ifdef MALI_DVFS_ASV_ENABLE
	if (dvfs_status->asv_status==ASV_STATUS_DISABLE_REQ) {
		dvfs_status->asv_status=mali_dvfs_update_asv(ASV_CMD_DISABLE);
	} else if (dvfs_status->asv_status==ASV_STATUS_NOT_INIT) {
		dvfs_status->asv_status=mali_dvfs_update_asv(ASV_CMD_ENABLE);
	}
#endif
	osk_spinlock_lock(&mali_dvfs_spinlock);
	if (dvfs_status->utilisation > mali_dvfs_infotbl[dvfs_status->step].max_threshold) {
		if (dvfs_status->step==kbase_platform_dvfs_get_level(450)) {
			if (platform->utilisation > mali_dvfs_infotbl[dvfs_status->step].max_threshold)
				dvfs_status->step++;
			OSK_ASSERT(dvfs_status->step < MALI_DVFS_STEP);
		} else {
			dvfs_status->step++;
			OSK_ASSERT(dvfs_status->step < MALI_DVFS_STEP);
		}
	}else if ((dvfs_status->step>0) &&
			(platform->time_tick == MALI_DVFS_TIME_INTERVAL) &&
			(platform->utilisation < mali_dvfs_infotbl[dvfs_status->step].min_threshold)) {
		OSK_ASSERT(dvfs_status->step > 0);
		dvfs_status->step--;
	}
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	if ((dvfs_status->upper_lock >= 0)&&(dvfs_status->step > dvfs_status->upper_lock)) {
		dvfs_status->step = dvfs_status->upper_lock;
	}
	if (dvfs_status->under_lock > 0) {
		if (dvfs_status->step < dvfs_status->under_lock)
			dvfs_status->step = dvfs_status->under_lock;
	}
#endif
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	kbase_platform_dvfs_set_level(dvfs_status->kbdev, dvfs_status->step);

	mutex_unlock(&mali_enable_clock_lock);
}

static DECLARE_WORK(mali_dvfs_work, mali_dvfs_event_proc);

int kbase_platform_dvfs_event(struct kbase_device *kbdev, u32 utilisation)
{
	struct exynos_context *platform;

	OSK_ASSERT(kbdev != NULL);
	platform = (struct exynos_context *) kbdev->platform_context;

	osk_spinlock_lock(&mali_dvfs_spinlock);
	if (platform->time_tick < MALI_DVFS_TIME_INTERVAL) {
		platform->time_tick++;
		platform->time_busy += kbdev->pm.metrics.time_busy;
		platform->time_idle += kbdev->pm.metrics.time_idle;
	} else {
		platform->time_busy = kbdev->pm.metrics.time_busy;
		platform->time_idle = kbdev->pm.metrics.time_idle;
		platform->time_tick = 0;
	}
	if ((platform->time_tick == MALI_DVFS_TIME_INTERVAL) &&
		(platform->time_idle + platform->time_busy > 0))
			platform->utilisation = (100*platform->time_busy) / (platform->time_idle + platform->time_busy);

	mali_dvfs_status_current.utilisation = utilisation;
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	queue_work_on(0, mali_dvfs_wq, &mali_dvfs_work);
	/*add error handle here*/
	return MALI_TRUE;
}

int kbase_platform_dvfs_get_utilisation(void)
{
	int utilisation = 0;

	osk_spinlock_lock(&mali_dvfs_spinlock);
	utilisation = mali_dvfs_status_current.utilisation;
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	return utilisation;
}

int kbase_platform_dvfs_get_enable_status(void)
{
	struct kbase_device *kbdev;
	unsigned long flags;
	int enable;

	kbdev = mali_dvfs_status_current.kbdev;
	spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
	enable = kbdev->pm.metrics.timer_active;
	spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);

	return enable;
}

int kbase_platform_dvfs_enable(bool enable, int freq)
{
	mali_dvfs_status *dvfs_status;
	struct kbase_device *kbdev;
	unsigned long flags;
	struct exynos_context *platform;

	dvfs_status = &mali_dvfs_status_current;
	kbdev = mali_dvfs_status_current.kbdev;

	OSK_ASSERT(kbdev != NULL);
	platform = (struct exynos_context *)kbdev->platform_context;

	mutex_lock(&mali_enable_clock_lock);

	if (freq != MALI_DVFS_CURRENT_FREQ) {
		osk_spinlock_lock(&mali_dvfs_spinlock);
		platform->time_tick = 0;
		platform->time_busy = 0;
		platform->time_idle = 0;
		platform->utilisation = 0;
		dvfs_status->step = kbase_platform_dvfs_get_level(freq);
		osk_spinlock_unlock(&mali_dvfs_spinlock);

		kbase_platform_dvfs_set_level(dvfs_status->kbdev, dvfs_status->step);
	}

	if (enable != kbdev->pm.metrics.timer_active) {
		if (enable) {
			spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
			kbdev->pm.metrics.timer_active = MALI_TRUE;
			spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
			hrtimer_start(&kbdev->pm.metrics.timer,
					HR_TIMER_DELAY_MSEC(KBASE_PM_DVFS_FREQUENCY),
					HRTIMER_MODE_REL);
		} else {
			spin_lock_irqsave(&kbdev->pm.metrics.lock, flags);
			kbdev->pm.metrics.timer_active = MALI_FALSE;
			spin_unlock_irqrestore(&kbdev->pm.metrics.lock, flags);
			hrtimer_cancel(&kbdev->pm.metrics.timer);
			pm_qos_update_request(&mem_bw_req, -1);
		}
	}
	mutex_unlock(&mali_enable_clock_lock);

	return MALI_TRUE;
}

int kbase_platform_dvfs_init(struct kbase_device *kbdev)
{
	/*default status
	  add here with the right function to get initilization value.
	 */
	if (!mali_dvfs_wq)
		mali_dvfs_wq = create_singlethread_workqueue("mali_dvfs");

	osk_spinlock_init(&mali_dvfs_spinlock,OSK_LOCK_ORDER_PM_METRICS);
	mutex_init(&mali_set_clock_lock);
	mutex_init(&mali_enable_clock_lock);

	pm_qos_add_request(&mem_bw_req, PM_QOS_MEMORY_THROUGHPUT, -1);

	/*add a error handling here*/
	osk_spinlock_lock(&mali_dvfs_spinlock);
	mali_dvfs_status_current.kbdev = kbdev;
	mali_dvfs_status_current.utilisation = 100;
	mali_dvfs_status_current.step = MALI_DVFS_STEP-1;
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	mali_dvfs_status_current.upper_lock = -1;
	mali_dvfs_status_current.under_lock = -1;
#endif
#ifdef MALI_DVFS_ASV_ENABLE
	mali_dvfs_status_current.asv_status=ASV_STATUS_NOT_INIT;
#endif
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	return MALI_TRUE;
}

void kbase_platform_dvfs_term(void)
{
	if (mali_dvfs_wq)
		destroy_workqueue(mali_dvfs_wq);

	mali_dvfs_wq = NULL;
}
#endif /*CONFIG_MALI_T6XX_DVFS*/

int mali_get_dvfs_upper_locked_freq(void)
{
	unsigned int locked_level = -1;

#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	locked_level = mali_dvfs_infotbl[mali_dvfs_status_current.upper_lock].clock;
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif
	return locked_level;
}

int mali_get_dvfs_under_locked_freq(void)
{
	unsigned int locked_level = -1;

#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	locked_level = mali_dvfs_infotbl[mali_dvfs_status_current.under_lock].clock;
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif
	return locked_level;
}

int mali_get_dvfs_current_level(void)
{
	unsigned int current_level = -1;

#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	current_level = mali_dvfs_status_current.step;
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif
	return current_level;
}

int mali_dvfs_freq_lock(int level)
{
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	if (mali_dvfs_status_current.under_lock >= 0) {
		printk( KERN_ERR "[G3D] Upper lock Error : Under lock is already set\n");
		osk_spinlock_unlock(&mali_dvfs_spinlock);
		return -1;
	}
	mali_dvfs_status_current.upper_lock = level;
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	printk( "[G3D] Upper Lock Set : %d\n", level );
#endif
	return 0;
}
void mali_dvfs_freq_unlock(void)
{
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	mali_dvfs_status_current.upper_lock = -1;
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif

	printk("[G3D] Upper Lock Unset\n");
}

int mali_dvfs_freq_under_lock(int level)
{
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	if (mali_dvfs_status_current.upper_lock >= 0) {
		printk( KERN_ERR "[G3D] Under lock Error : Upper lock is already set\n");
		osk_spinlock_unlock(&mali_dvfs_spinlock);
		return -1;
	}
	mali_dvfs_status_current.under_lock = level;
	osk_spinlock_unlock(&mali_dvfs_spinlock);

	printk( "[G3D] Under Lock Set : %d\n", level );
#endif
	return 0;
}
void mali_dvfs_freq_under_unlock(void)
{
#ifdef CONFIG_MALI_T6XX_FREQ_LOCK
	osk_spinlock_lock(&mali_dvfs_spinlock);
	mali_dvfs_status_current.under_lock = -1;
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif
	printk("[G3D] Under Lock Unset\n");
}

int kbase_platform_regulator_init(void)
{

#ifdef CONFIG_REGULATOR
	g3d_regulator = regulator_get(NULL, "vdd_g3d");
	if (IS_ERR(g3d_regulator)) {
		printk("[kbase_platform_regulator_init] failed to get mali t6xx regulator\n");
		return -1;
	}

	if (regulator_enable(g3d_regulator) != 0) {
		printk("[kbase_platform_regulator_init] failed to enable mali t6xx regulator\n");
		return -1;
	}

	if (regulator_set_voltage(g3d_regulator, mali_gpu_vol, mali_gpu_vol) != 0) {
		printk("[kbase_platform_regulator_init] failed to set mali t6xx operating voltage [%d]\n", mali_gpu_vol);
		return -1;
	}
#endif
	return 0;
}

int kbase_platform_regulator_disable(void)
{
#ifdef CONFIG_REGULATOR
	if (!g3d_regulator) {
		printk("[kbase_platform_regulator_disable] g3d_regulator is not initialized\n");
		return -1;
	}

	if (regulator_disable(g3d_regulator) != 0) {
		printk("[kbase_platform_regulator_disable] failed to disable g3d regulator\n");
		return -1;
	}
#endif
	return 0;
}

int kbase_platform_regulator_enable(void)
{
#ifdef CONFIG_REGULATOR
	if (!g3d_regulator) {
		printk("[kbase_platform_regulator_enable] g3d_regulator is not initialized\n");
		return -1;
	}

	if (regulator_enable(g3d_regulator) != 0) {
		printk("[kbase_platform_regulator_enable] failed to enable g3d regulator\n");
		return -1;
	}
#endif
	return 0;
}

int kbase_platform_get_default_voltage(struct device *dev, int *vol)
{
#ifdef CONFIG_REGULATOR
	*vol = mali_gpu_vol;
#else
	*vol = 0;
#endif
	return 0;
}

int kbase_platform_get_voltage(struct device *dev, int *vol)
{
#ifdef CONFIG_REGULATOR
	if (!g3d_regulator) {
		printk("[kbase_platform_get_voltage] g3d_regulator is not initialized\n");
		return -1;
	}

	*vol = regulator_get_voltage(g3d_regulator);
#else
	*vol = 0;
#endif
	return 0;
}

int kbase_platform_set_voltage(struct device *dev, int vol)
{
#ifdef CONFIG_REGULATOR
	if (!g3d_regulator) {
		printk("[kbase_platform_set_voltage] g3d_regulator is not initialized\n");
		return -1;
	}

	if (regulator_set_voltage(g3d_regulator, vol, vol) != 0)
	{
		printk("[kbase_platform_set_voltage] failed to set voltage\n");
		return -1;
	}
#endif
	return 0;
}

void kbase_platform_dvfs_set_clock(kbase_device *kbdev, int freq)
{
	static struct clk * mout_gpll = NULL;
	static struct clk * fin_gpll = NULL;
	static struct clk * fout_gpll = NULL;
	static int _freq = -1;
	static unsigned long gpll_rate_prev = 0;
	unsigned long gpll_rate = 0, aclk_400_rate = 0;
	unsigned long tmp = 0;
	struct exynos_context *platform;

	if (!kbdev)
		panic("oops");

	platform = (struct exynos_context *) kbdev->platform_context;
	if (NULL == platform)
	{
		panic("oops");
	}

	if (mout_gpll==NULL) {
		mout_gpll = clk_get(kbdev->osdev.dev, "mout_gpll");
		fin_gpll = clk_get(kbdev->osdev.dev, "ext_xtal");
		fout_gpll = clk_get(kbdev->osdev.dev, "fout_gpll");
		if (IS_ERR(mout_gpll) || IS_ERR(fin_gpll) || IS_ERR(fout_gpll))
			panic("clk_get ERROR");
	}

	if (platform->sclk_g3d == 0)
		return;

	if (freq == _freq)
		return;

	switch(freq) {
		case 533:
			gpll_rate = 533000000;
			aclk_400_rate = 533000000;
			break;
		case 450:
			gpll_rate = 450000000;
			aclk_400_rate = 450000000;
			break;
		case 400:
			gpll_rate = 800000000;
			aclk_400_rate = 400000000;
			break;
		case 350:
			gpll_rate = 1400000000;
			aclk_400_rate = 350000000;
			break;
		case 266:
			gpll_rate = 800000000;
			aclk_400_rate = 267000000;
			break;
		case 160:
			gpll_rate = 800000000;
			aclk_400_rate = 160000000;
			break;
		case 100:
			gpll_rate = 800000000;
			aclk_400_rate = 100000000;
			break;
		default:
			return;
	}

	/* if changed the GPLL rate, set rate for GPLL and wait for lock time */
	if (gpll_rate != gpll_rate_prev) {
		/*for stable clock input.*/
		clk_set_rate(platform->sclk_g3d, 100000000);
		clk_set_parent(mout_gpll, fin_gpll);

		/*change gpll*/
		clk_set_rate( fout_gpll, gpll_rate );

		/*restore parent*/
		clk_set_parent(mout_gpll, fout_gpll);
		gpll_rate_prev = gpll_rate;
	}

	_freq = freq;
	clk_set_rate(platform->sclk_g3d, aclk_400_rate);

	/* Waiting for clock is stable */
	do {
		tmp = __raw_readl(EXYNOS5_CLKDIV_STAT_TOP0);
	} while (tmp & 0x1000000);

	return;
}

static void kbase_platform_dvfs_set_vol(unsigned int vol)
{
	static int _vol = -1;

	if (_vol == vol)
		return;

	kbase_platform_set_voltage(NULL, vol);
	_vol = vol;

	return;
}

int kbase_platform_dvfs_get_level(int freq)
{
	int i;
	for (i=0; i < MALI_DVFS_STEP; i++) {
		if (mali_dvfs_infotbl[i].clock == freq)
			return i;
	}

	return -1;
}

void kbase_platform_dvfs_set_level(kbase_device *kbdev, int level)
{
	static int prev_level = -1;
	int bw;

	if (level == prev_level)
		return;

	if (WARN_ON((level >= MALI_DVFS_STEP)||(level < 0)))
		panic("invalid level");

#ifdef CONFIG_MALI_T6XX_DVFS
	mutex_lock(&mali_set_clock_lock);
#endif
	if (level > 0) {
		bw = mali_dvfs_infotbl[level].clock * 16;
		bw = clamp(bw, 0, 6400);
	} else {
		bw = -1;
	}

	if (level > prev_level) {
		kbase_platform_dvfs_set_vol(mali_dvfs_infotbl[level].voltage);
		kbase_platform_dvfs_set_clock(kbdev, mali_dvfs_infotbl[level].clock);
		pm_qos_update_request(&mem_bw_req, bw);
	} else {
		pm_qos_update_request(&mem_bw_req, bw);
		kbase_platform_dvfs_set_clock(kbdev, mali_dvfs_infotbl[level].clock);
		kbase_platform_dvfs_set_vol(mali_dvfs_infotbl[level].voltage);
	}
#if defined(CONFIG_MALI_T6XX_DEBUG_SYS) && defined(CONFIG_MALI_T6XX_DVFS)
	update_time_in_state(prev_level);
#endif
	prev_level = level;
#ifdef CONFIG_MALI_T6XX_DVFS
	mutex_unlock(&mali_set_clock_lock);
#endif
}

int kbase_platform_dvfs_sprint_avs_table(char *buf)
{
#ifdef MALI_DVFS_ASV_ENABLE
	int i, cnt=0;
	if (buf==NULL)
		return 0;

	for (i=MALI_DVFS_STEP-1; i >= 0; i--) {
		cnt+=sprintf(buf+cnt,"%dMhz:%d\n",
				mali_dvfs_infotbl[i].clock, mali_dvfs_infotbl[i].voltage);
	}
	return cnt;
#else
	return 0;
#endif
}

int kbase_platform_dvfs_set(int enable)
{
#ifdef MALI_DVFS_ASV_ENABLE
	osk_spinlock_lock(&mali_dvfs_spinlock);
	if (enable) {
		mali_dvfs_status_current.asv_status=ASV_STATUS_NOT_INIT;
	} else {
		mali_dvfs_status_current.asv_status=ASV_STATUS_DISABLE_REQ;
	}
	osk_spinlock_unlock(&mali_dvfs_spinlock);
#endif
	return 0;
}

#ifdef CONFIG_MALI_T6XX_DEBUG_SYS
static void update_time_in_state(int level)
{
	u64 current_time;
	static u64 prev_time=0;

	if (!kbase_platform_dvfs_get_enable_status())
		return;

	if (prev_time ==0)
		prev_time=get_jiffies_64();

	current_time = get_jiffies_64();
#ifdef CONFIG_MALI_T6XX_DVFS
	mali_dvfs_infotbl[level].time += current_time-prev_time;
#endif
	prev_time = current_time;
}

ssize_t show_time_in_state(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct kbase_device *kbdev;
	ssize_t ret = 0;
	int i;

	kbdev = dev_get_drvdata(dev);

	update_time_in_state(mali_dvfs_status_current.step);

	if (!kbdev)
		return -ENODEV;

	for (i = 0; i < MALI_DVFS_STEP; i++) {
		ret += snprintf(buf+ret, PAGE_SIZE-ret, "%d %llu\n",
				mali_dvfs_infotbl[i].clock,
				mali_dvfs_infotbl[i].time);
	}

	if (ret < PAGE_SIZE - 1) {
		ret += snprintf(buf+ret, PAGE_SIZE-ret, "\n");
	} else {
		buf[PAGE_SIZE-2] = '\n';
		buf[PAGE_SIZE-1] = '\0';
		ret = PAGE_SIZE-1;
	}

	return ret;
}

ssize_t set_time_in_state(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int i;

	for (i = 0; i < MALI_DVFS_STEP; i++) {
		mali_dvfs_infotbl[i].time = 0;
	}

	return count;
}
#endif