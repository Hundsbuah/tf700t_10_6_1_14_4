/* Copyright (c) 2012, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * Generic auto hotplug driver for ARM SoCs. Targeted at current generation
 * SoCs with dual and quad core applications processors.
 * Automatically hotplugs online and offline CPUs based on system load.
 * It is also capable of immediately onlining a core based on an external
 * event.
 *
 * Not recommended for use with OMAP4460 due to the potential for lockups
 * whilst hotplugging.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mutex.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

/*
 * Enable debug output to dump the average
 * calculations and ring buffer array values
 * WARNING: Enabling this causes a ton of overhead
 *
 * FIXME: Turn it into debugfs stats (somehow)
 * because currently it is a sack of shit.
 */
#define DEBUG 0

#define CPUS_AVAILABLE		num_possible_cpus()
#define MIN_ONLINE_CPUS		1 /* Number of online CPUs whilst screen on */
#define SAMPLING_PERIODS	25 /* Load average for 500ms at MIN_SAMPLING_RATE */
#define MIN_SAMPLING_RATE	msecs_to_jiffies(20)
#define INDEX_MAX_VALUE		(SAMPLING_PERIODS - 1)
#define ENABLE_LOAD_THRESHOLD	(100 * CPUS_AVAILABLE) /* When load spikes high, enable all CPUs */
#define DISABLE_LOAD_THRESHOLD	55 /* When CPU load is 0.55 disable additional CPUs */

/* Control flags */
u8 flags;
#define HOTPLUG_DISABLED	(1 << 0) /* FIXME: Needs implementing */
#define BOOSTPULSE_ACTIVE	(1 << 1)
#define BOOSTPULSE_ONESHOT	(1 << 2)
#define EARLYSUSPEND_ACTIVE	(1 << 3)

struct delayed_work hotplug_decision_work;
struct delayed_work hotplug_online_all_work;
struct delayed_work hotplug_offline_work;
struct delayed_work hotplug_offline_all_work;
struct work_struct hotplug_boost_online_work;

static u8 min_online_cpus __read_mostly = MIN_ONLINE_CPUS;
static u16 history[SAMPLING_PERIODS];
static u8 index;
static u16 avg_running;
static DEFINE_MUTEX(hotplug_lock);
static DEFINE_MUTEX(boostpulse_lock);


static void hotplug_decision_work_fn(struct work_struct *work)
{
	u16 running, disable_load, sampling_rate, enable_load;
	u8 online_cpus, i, j;
#if DEBUG
	u8 k;
#endif

	mutex_lock(&hotplug_lock);

	online_cpus = num_online_cpus();

	disable_load = DISABLE_LOAD_THRESHOLD * online_cpus;
	enable_load = ENABLE_LOAD_THRESHOLD / 2 * online_cpus;
	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use float division to get the average, as it is a hell
	 * of a lot more expensive.
	 */
	running = nr_running() * 100;

	history[index] = running;

#if DEBUG
	pr_info("online_cpus is: %d\n", online_cpus);
	pr_info("enable_load is: %d\n", enable_load);
	pr_info("disable_load is: %d\n", disable_load);
	pr_info("index is: %d\n", index);
	pr_info("running is: %d\n", running);
#endif

	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short duration where
	 * we don't want additional cores to be onlined because
	 * the cpufreq driver should take care of those load spikes.
	 */
	for (i = 0, j = index; i < SAMPLING_PERIODS; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = INDEX_MAX_VALUE;
	}

#if DEBUG
	pr_info("array contents: ");
	for (k = 0; k < SAMPLING_PERIODS; k++) {
		 pr_info("%d\t", history[k]);
	}
	pr_info("\n");
	pr_info("avg_running before division: %d\n", avg_running);
#endif

	avg_running = avg_running / SAMPLING_PERIODS;

	/*
	 * If we are at the end of the buffer, return to the beginning.
	 */
	if (unlikely(index++ == INDEX_MAX_VALUE))
		index = 0;

#if DEBUG
	pr_info("average_running is: %d\n", avg_running);
#endif
	mutex_unlock(&hotplug_lock);

	if(unlikely(((avg_running >= ENABLE_LOAD_THRESHOLD) && (online_cpus < CPUS_AVAILABLE))
			|| (online_cpus < min_online_cpus))) {
		pr_info("auto_hotplug: Onlining CPUs, avg running: %d\n", avg_running);
		/*
		 * Flush any delayed offlining work from the workqueue.
		 * No point in having expensive unnecessary hotplug transitions.
		 * We still online after flushing, because load is high enough to
		 * warrant it.
		 */
		cancel_delayed_work_sync(&hotplug_offline_work);
		schedule_delayed_work_on(0, &hotplug_online_all_work, 0);
		return;
	}
	if(unlikely((avg_running <= disable_load) && (online_cpus > min_online_cpus))
		&& (!(flags & (BOOSTPULSE_ACTIVE & BOOSTPULSE_ONESHOT)))) {
		if (!(delayed_work_pending(&hotplug_offline_work))) {
			pr_info("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
			schedule_delayed_work_on(0, &hotplug_offline_work, HZ);
			mutex_lock(&boostpulse_lock);
			if (unlikely(flags & BOOSTPULSE_ONESHOT)) {
				flags &= ~(BOOSTPULSE_ACTIVE | BOOSTPULSE_ONESHOT);
				pr_info("auto_hotplug: &= ~(BOOSTPULSE_ACTIVE | BOOSTPULSE_ONESHOT)\n");
			}
			mutex_unlock(&boostpulse_lock);
			return;
		}
	}

	/*
	 * If we don't queue a cpu_up or cpu_down then fall through to here
	 * reduce the sampling rate dynamically based on online cpus.
	 */
	sampling_rate = MIN_SAMPLING_RATE * (online_cpus * online_cpus);
#if DEBUG
	pr_info("sampling_rate is: %d\n", jiffies_to_msecs(sampling_rate));
#endif
	schedule_delayed_work_on(0, &hotplug_decision_work, sampling_rate);
}

static int min_online_state_set(const char *val, const struct kernel_param *kp)
{
	param_set_uint(val, kp);
	pr_info("auto_hotplug min_online_cpus: %d\n", min_online_cpus);
	/*
	 * Make sure that a sane value has been passed to
	 * the min_online_cpus parameter.
	 */
	if (unlikely(min_online_cpus > CPUS_AVAILABLE))
		min_online_cpus = CPUS_AVAILABLE;
	else if (unlikely(min_online_cpus < 1))
		min_online_cpus = 1;
	return 0;
}

static int min_online_state_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops hotplug_min_online_ops = {
	.set = min_online_state_set,
	.get = min_online_state_get,
};
module_param_cb(min_online_cpus, &hotplug_min_online_ops, &min_online_cpus, 0644);

static void hotplug_online_all_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu))) {
			cpu_up(cpu);
			pr_info("auto_hotplug: CPU%d up.\n", cpu);
		}
	}
	/*
	 * Pause for 2 seconds before even considering offlining a CPU
	 */
	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 2);
}

static void hotplug_offline_all_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		if (likely(cpu_online(cpu) && (cpu != 0))) {
			cpu_down(cpu);
			pr_info("auto_hotplug: CPU%d down.\n", cpu);
		}
	}
}

static void hotplug_boost_online_work_fn(struct work_struct *work)
{
	int cpu;

	pr_info("%s\n", __func__);

	mutex_lock(&boostpulse_lock);
	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu))) {
			pr_info("%s: for_each_possible_cpu()\n", __func__);
			cpu_up(cpu);
			pr_info("auto_hotplug: CPU%d up.\n", cpu);
			break;
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, 2 * HZ);
	mutex_unlock(&boostpulse_lock);
}

static void hotplug_offline_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_online_cpu(cpu) {
			if (cpu != 0) {
				cpu_down(cpu);
				pr_info("auto_hotplug: CPU%d down.\n", cpu);
				break;
			}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, MIN_SAMPLING_RATE);
}

/*
 * TODO: Use sysfs interface instead of external call to achieve this.
 * Then we can be invoked directly from PowerHAL instead of via CPUfreq.
 */
void hotplug_boostpulse(bool flag, bool oneshot)
{
	if (unlikely(flags & EARLYSUSPEND_ACTIVE))
		return;

	mutex_lock(&boostpulse_lock);
	if (unlikely(oneshot)) {
		if (!(flags & BOOSTPULSE_ONESHOT)) {
			flags |= BOOSTPULSE_ONESHOT;
			pr_info("auto_hotplug: |= BOOSTPULSE_ONESHOT\n");
		}
	}

	if (likely(flag)) {
		if (!(flags & BOOSTPULSE_ACTIVE)) {
			flags |= BOOSTPULSE_ACTIVE;
			pr_info("auto_hotplug: |= BOOSTPULSE_ACTIVE\n");
			/*
			 * If there are less than 2 CPUs online, then online
			 * an additional CPU, otherwise check for any pending
			 * offlines, cancel them and delay the next sample by
			 * 2 seconds. Either way, we don't cause any offlines
			 * whilst the user is interacting with the device.
			 */
			if (likely(num_online_cpus() < 2)) {
				cancel_delayed_work_sync(&hotplug_offline_work);
				cancel_delayed_work_sync(&hotplug_decision_work);
				schedule_work_on(0, &hotplug_boost_online_work);
			} else {
				pr_info("auto_hotplug: %s: %d CPUs online\n", __func__, num_online_cpus());
				if (delayed_work_pending(&hotplug_offline_work)) {
					pr_info("auto_hotplug: %s: Cancelling hotplug_offline_work\n", __func__);
					cancel_delayed_work_sync(&hotplug_offline_work);
					cancel_delayed_work_sync(&hotplug_decision_work);
					schedule_delayed_work_on(0, &hotplug_decision_work, 2 * HZ);
				}
			}
		}
	} else {
		flags &= ~BOOSTPULSE_ACTIVE;
		pr_info("auto_hotplug: &= ~BOOSTPULSE_ACTIVE\n");
	}
	mutex_unlock(&boostpulse_lock);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void auto_hotplug_early_suspend(struct early_suspend *handler)
{
	pr_info("auto_hotplug: early suspend handler\n");
	flags |= EARLYSUSPEND_ACTIVE;

	/* Cancel all scheduled delayed work to avoid races */
	cancel_delayed_work_sync(&hotplug_offline_work);
	cancel_delayed_work_sync(&hotplug_decision_work);
	if (num_online_cpus() > 1) {
		pr_info("auto_hotplug: Offlining CPUs for early suspend\n");
		schedule_delayed_work_on(0, &hotplug_offline_all_work, 0);
	}
}

static void auto_hotplug_late_resume(struct early_suspend *handler)
{
	pr_info("auto_hotplug: late resume handler\n");
	flags &= ~EARLYSUSPEND_ACTIVE;

	schedule_delayed_work_on(0, &hotplug_decision_work, HZ);
}

static struct early_suspend auto_hotplug_suspend = {
	.suspend = auto_hotplug_early_suspend,
	.resume = auto_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __init auto_hotplug_init(void)
{
	pr_info("auto_hotplug: v0.201 by _thalamus init()\n");
	pr_info("auto_hotplug: %d CPUs detected\n", CPUS_AVAILABLE);
	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);
	INIT_DELAYED_WORK(&hotplug_online_all_work, hotplug_online_all_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_offline_all_work, hotplug_offline_all_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_offline_work, hotplug_offline_work_fn);
	INIT_WORK(&hotplug_boost_online_work, hotplug_boost_online_work_fn);

	/*
	 * FIXME: Not ideal, boostpulse can override this plus it would be better
	 * to start sampling earlier then flick a switch to enable the actual hotplug
	 * actions. Currently, as soon as this fires, we offline all the secondary cores
	 * because all our samples are 0 which is a very bad thing.
	 */
	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */
	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 30);
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&auto_hotplug_suspend);
#endif
	return 0;
}
late_initcall(auto_hotplug_init);
