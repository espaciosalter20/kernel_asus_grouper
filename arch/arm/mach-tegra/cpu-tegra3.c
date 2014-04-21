/*
 * arch/arm/mach-tegra/cpu-tegra3.c
 *
 * CPU auto-hotplug for Tegra3 CPUs
 *
 * Copyright (c) 2011-2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pm_qos_params.h>

#include "pm.h"
#include "cpu-tegra.h"
#include "clock.h"

#define INITIAL_STATE		TEGRA_HP_DISABLED
#define UP2G0_DELAY_MS		50
#define UP2Gn_DELAY_MS		60
#define DOWN_DELAY_MS		2000

static struct mutex *tegra3_cpu_lock;

static struct workqueue_struct *hotplug_wq;
static struct delayed_work hotplug_work;

static bool no_lp;
module_param(no_lp, bool, 0644);

static unsigned long up2gn_delay;
static unsigned long up2g0_delay;
static unsigned long down_delay;
module_param(up2gn_delay, ulong, 0644);
module_param(up2g0_delay, ulong, 0644);
module_param(down_delay, ulong, 0644);

static unsigned int idle_top_freq;
static unsigned int idle_bottom_freq;
module_param(idle_top_freq, uint, 0644);
module_param(idle_bottom_freq, uint, 0644);

/*
static int mp_overhead = 10;
module_param(mp_overhead, int, 0644);*/

static int balance_level = 70;
module_param(balance_level, int, 0644);

static struct clk *cpu_clk;
static struct clk *cpu_g_clk;
static struct clk *cpu_lp_clk;

enum {
	TEGRA_HP_DISABLED = 0,
	TEGRA_HP_IDLE,
	TEGRA_HP_DOWN,
	TEGRA_HP_UP,
};
static int hp_state;

static int hp_state_set(const char *arg, const struct kernel_param *kp)
{
	int ret = 0;
	int old_state;

	if (!tegra3_cpu_lock)
		return ret;

	mutex_lock(tegra3_cpu_lock);

	old_state = hp_state;
	ret = param_set_bool(arg, kp);	/* set idle or disabled only */

	if (ret == 0) {
		if ((hp_state == TEGRA_HP_DISABLED) &&
		    (old_state != TEGRA_HP_DISABLED))
			pr_info("Tegra auto-hotplug disabled\n");
		else if (hp_state != TEGRA_HP_DISABLED) {
			if (old_state == TEGRA_HP_DISABLED) {
				pr_info("Tegra auto-hotplug enabled\n");
			}
			/* catch-up with governor target speed */
			tegra_cpu_set_speed_cap(NULL);
		}
	} else
		pr_warn("%s: unable to set tegra hotplug state %s\n",
				__func__, arg);

	mutex_unlock(tegra3_cpu_lock);
	return ret;
}

static int hp_state_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_int(buffer, kp);
}

static struct kernel_param_ops tegra_hp_state_ops = {
	.set = hp_state_set,
	.get = hp_state_get,
};
module_param_cb(auto_hotplug, &tegra_hp_state_ops, &hp_state, 0644);


enum {
	TEGRA_CPU_SPEED_BALANCED,
	TEGRA_CPU_SPEED_BIASED,
	TEGRA_CPU_SPEED_SKEWED,
};

static noinline int tegra_cpu_speed_balance(void)
{
	unsigned long highest_speed = tegra_cpu_highest_speed();
	unsigned long balanced_speed = highest_speed * balance_level / 100;
	unsigned long skewed_speed = balanced_speed / 2;
	unsigned int nr_cpus = num_online_cpus();
	unsigned int max_cpus = pm_qos_request(PM_QOS_MAX_ONLINE_CPUS) ? : 4;
	unsigned int min_cpus = pm_qos_request(PM_QOS_MIN_ONLINE_CPUS);

	/* balanced: freq targets for all CPUs are above 50% of highest speed
	   biased: freq target for at least one CPU is below 50% threshold
	   skewed: freq targets for at least 2 CPUs are below 25% threshold */
	if (((tegra_count_slow_cpus(skewed_speed) >= 2) /*||
	     tegra_cpu_edp_favor_down(nr_cpus, mp_overhead)*/ ||
	     (highest_speed <= idle_bottom_freq) || (nr_cpus > max_cpus)) &&
	    (nr_cpus > min_cpus))
		return TEGRA_CPU_SPEED_SKEWED;

	if (((tegra_count_slow_cpus(balanced_speed) >= 1) /*||
	     (!tegra_cpu_edp_favor_up(nr_cpus, mp_overhead))*/ ||
	     (highest_speed <= idle_bottom_freq) || (nr_cpus == max_cpus)) &&
	    (nr_cpus >= min_cpus))
		return TEGRA_CPU_SPEED_BIASED;

	return TEGRA_CPU_SPEED_BALANCED;
}
void disable_auto_hotplug(void)
{
	hp_state=TEGRA_HP_DISABLED;
	cancel_delayed_work(&hotplug_work);
}
static void tegra_auto_hotplug_work_func(struct work_struct *work)
{
	bool up = false;
	unsigned int cpu = nr_cpu_ids;
	unsigned long now = jiffies;
	static unsigned long last_change_time;

	mutex_lock(tegra3_cpu_lock);

	switch (hp_state) {
	case TEGRA_HP_DISABLED:
	case TEGRA_HP_IDLE:
		break;
	case TEGRA_HP_DOWN:
		cpu = tegra_get_slowest_cpu_n();
		if (cpu < nr_cpu_ids) {
			up = false;
		} else if (!is_lp_cluster() && !no_lp &&
			   !pm_qos_request(PM_QOS_MIN_ONLINE_CPUS)) {
			if(!clk_set_parent(cpu_clk, cpu_lp_clk)) {
				/* catch-up with governor target speed */
				tegra_cpu_set_speed_cap(NULL);
				break;
			}
		}
		queue_delayed_work(
			hotplug_wq, &hotplug_work, down_delay);
		break;
	case TEGRA_HP_UP:
		if (is_lp_cluster() && !no_lp) {
			if(!clk_set_parent(cpu_clk, cpu_g_clk)) {
				/* catch-up with governor target speed */
				tegra_cpu_set_speed_cap(NULL);
			}
		} else {
			switch (tegra_cpu_speed_balance()) {
			/* cpu speed is up and balanced - one more on-line */
			case TEGRA_CPU_SPEED_BALANCED:
				cpu = cpumask_next_zero(0, cpu_online_mask);
				if (cpu < nr_cpu_ids)
					up = true;
				break;
			/* cpu speed is up, but skewed - remove one core */
			case TEGRA_CPU_SPEED_SKEWED:
				cpu = tegra_get_slowest_cpu_n();
				if (cpu < nr_cpu_ids)
					up = false;
				break;
			/* cpu speed is up, but under-utilized - do nothing */
			case TEGRA_CPU_SPEED_BIASED:
			default:
				break;
			}
		}
		queue_delayed_work(
			hotplug_wq, &hotplug_work, up2gn_delay);
		break;
	default:
		pr_err("%s: invalid tegra hotplug state %d\n",
		       __func__, hp_state);
	}

	if (!up && ((now - last_change_time) < down_delay)){
	  mutex_unlock(tegra3_cpu_lock);
	  return;
	}

	if (cpu < nr_cpu_ids) {
		last_change_time = now;
	}
	mutex_unlock(tegra3_cpu_lock);

	if (cpu < nr_cpu_ids) {
		if (up)
			cpu_up(cpu);
		else
			cpu_down(cpu);
	}
}

static int min_cpus_notify(struct notifier_block *nb, unsigned long n, void *p)
{
	mutex_lock(tegra3_cpu_lock);

	if ((n >= 1) && is_lp_cluster()) {
		/* make sure cpu rate is within g-mode range before switching */
		unsigned int speed = max(
			tegra_getspeed(0), clk_get_min_rate(cpu_g_clk) / 1000);
		tegra_update_cpu_speed(speed);

    clk_set_parent(cpu_clk, cpu_g_clk);
	}
	/* update governor state machine */
	tegra_cpu_set_speed_cap(NULL);
	mutex_unlock(tegra3_cpu_lock);
	return NOTIFY_OK;
}

static struct notifier_block min_cpus_notifier = {
	.notifier_call = min_cpus_notify,
};

void tegra_auto_hotplug_governor(unsigned int cpu_freq, bool suspend)
{
	unsigned long up_delay, top_freq, bottom_freq;

	if (!is_g_cluster_present())
		return;

	if (hp_state == TEGRA_HP_DISABLED)
		return;

	if (suspend) {
		hp_state = TEGRA_HP_IDLE;

		/* Switch to G-mode if suspend rate is high enough */
		if (is_lp_cluster() && (cpu_freq >= idle_bottom_freq)) {
		  clk_set_parent(cpu_clk, cpu_g_clk);
		}
		return;
	}

	if (is_lp_cluster()) {
		up_delay = up2g0_delay;
		top_freq = idle_top_freq;
		bottom_freq = 0;
	} else {
		up_delay = up2gn_delay;
		top_freq = idle_bottom_freq;
		bottom_freq = idle_bottom_freq;
	}

	if (pm_qos_request(PM_QOS_MIN_ONLINE_CPUS) >= 2) {
		if (hp_state != TEGRA_HP_UP) {
			hp_state = TEGRA_HP_UP;
			queue_delayed_work(
				hotplug_wq, &hotplug_work, up_delay);
		}
		return;
	}

	switch (hp_state) {
	case TEGRA_HP_IDLE:
		if (cpu_freq > top_freq) {
			hp_state = TEGRA_HP_UP;
			queue_delayed_work(
				hotplug_wq, &hotplug_work, up_delay);
		} else if (cpu_freq <= bottom_freq) {
			hp_state = TEGRA_HP_DOWN;
			queue_delayed_work(
				hotplug_wq, &hotplug_work, down_delay);
		}
		break;
	case TEGRA_HP_DOWN:
		if (cpu_freq > top_freq) {
			hp_state = TEGRA_HP_UP;
			queue_delayed_work(
				hotplug_wq, &hotplug_work, up_delay);
		} else if (cpu_freq > bottom_freq) {
			hp_state = TEGRA_HP_IDLE;
		}
		break;
	case TEGRA_HP_UP:
		if (cpu_freq <= bottom_freq) {
			hp_state = TEGRA_HP_DOWN;
			queue_delayed_work(
				hotplug_wq, &hotplug_work, down_delay);
		} else if (cpu_freq <= top_freq) {
			hp_state = TEGRA_HP_IDLE;
		}
		break;
	default:
		pr_err("%s: invalid tegra hotplug state %d\n",
		       __func__, hp_state);
		BUG();
	}
}

int tegra_auto_hotplug_init(struct mutex *cpu_lock)
{
	/*
	 * Not bound to the issuer CPU (=> high-priority), has rescue worker
	 * task, single-threaded, freezable.
	 */
	hotplug_wq = alloc_workqueue(
		"cpu-tegra3", WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);
	if (!hotplug_wq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&hotplug_work, tegra_auto_hotplug_work_func);

	cpu_clk = clk_get_sys(NULL, "cpu");
	cpu_g_clk = clk_get_sys(NULL, "cpu_g");
	cpu_lp_clk = clk_get_sys(NULL, "cpu_lp");
	if (IS_ERR(cpu_clk) || IS_ERR(cpu_g_clk) || IS_ERR(cpu_lp_clk))
		return -ENOENT;

	idle_top_freq = clk_get_max_rate(cpu_lp_clk) / 1000;
	idle_bottom_freq = clk_get_min_rate(cpu_g_clk) / 1000;

	up2g0_delay = msecs_to_jiffies(UP2G0_DELAY_MS);
	up2gn_delay = msecs_to_jiffies(UP2Gn_DELAY_MS);
	down_delay = msecs_to_jiffies(DOWN_DELAY_MS);

	tegra3_cpu_lock = cpu_lock;
	hp_state = INITIAL_STATE;
	pr_info("Tegra auto-hotplug initialized: %s\n",
		(hp_state == TEGRA_HP_DISABLED) ? "disabled" : "enabled");

	if (pm_qos_add_notifier(PM_QOS_MIN_ONLINE_CPUS, &min_cpus_notifier))
		pr_err("%s: Failed to register min cpus PM QoS notifier\n",
			__func__);

	return 0;
}

#ifdef CONFIG_DEBUG_FS
struct pm_qos_request_list min_cpu_req;
struct pm_qos_request_list max_cpu_req;

static int min_cpus_get(void *data, u64 *val)
{
	*val = pm_qos_request(PM_QOS_MIN_ONLINE_CPUS);
	return 0;
}
static int min_cpus_set(void *data, u64 val)
{
	pm_qos_update_request(&min_cpu_req, (s32)val);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(min_cpus_fops, min_cpus_get, min_cpus_set, "%llu\n");

static int max_cpus_get(void *data, u64 *val)
{
	*val = pm_qos_request(PM_QOS_MAX_ONLINE_CPUS);
	return 0;
}
static int max_cpus_set(void *data, u64 val)
{
	pm_qos_update_request(&max_cpu_req, (s32)val);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(max_cpus_fops, max_cpus_get, max_cpus_set, "%llu\n");
#endif

void tegra_auto_hotplug_exit(void)
{
	destroy_workqueue(hotplug_wq);
#ifdef CONFIG_DEBUG_FS
	pm_qos_remove_request(&min_cpu_req);
	pm_qos_remove_request(&max_cpu_req);
#endif
}
