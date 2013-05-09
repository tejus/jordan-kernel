/*
 * drivers/cpufreq/cpufreq_interactive.c
 *
 * Copyright (C) 2010 Google, Inc.
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
 * Author: Mike Chan (mike@android.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <asm/cputime.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_interactive.h>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>

static atomic_t active_count = ATOMIC_INIT(0);

struct cpufreq_interactive_cpuinfo {
	struct timer_list cpu_timer;
	int timer_idlecancel;
	u64 time_in_idle;
	u64 time_in_iowait;
	u64 idle_exit_time;
	u64 timer_run_time;
	int idling;
	u64 target_set_time;
	u64 target_set_time_in_idle;
	u64 freq_change_time_in_iowait;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int target_freq;
	unsigned int floor_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time;
	int governor_enabled;
	unsigned int *load_history;
	unsigned int total_avg_load;
	unsigned int total_load_history;
	unsigned int low_power_rate_history;
	unsigned int cpu_tune_value;
};

static DEFINE_PER_CPU(struct cpufreq_interactive_cpuinfo, cpuinfo);

/* Workqueues handle frequency scaling */
static struct task_struct *up_task;
static struct workqueue_struct *down_wq;
static struct work_struct freq_scale_down_work;
static cpumask_t up_cpumask;
static spinlock_t up_cpumask_lock;
static cpumask_t down_cpumask;
static spinlock_t down_cpumask_lock;
static struct mutex set_speed_lock;

static struct workqueue_struct *tune_wq;
static struct work_struct tune_work;
static cpumask_t tune_cpumask;
static spinlock_t tune_cpumask_lock;

/* Consider IO as busy */
static unsigned long io_is_busy;

static unsigned int sampling_periods;
static unsigned int history_load_index;
static unsigned int low_power_threshold;
static unsigned int hi_perf_threshold;
static unsigned int low_power_rate;
static enum tune_values {
	LOW_POWER_TUNE = 0,
	DEFAULT_TUNE,
	HIGH_PERF_TUNE
} cur_tune_value;

#define MIN_GO_HISPEED_LOAD 70
#define DEFAULT_LOW_POWER_RATE 10

/* default number of sampling periods to average before hotplug-in decision */
#define DEFAULT_SAMPLING_PERIODS 10
#define DEFAULT_HI_PERF_THRESHOLD 80
#define DEFAULT_LOW_POWER_THRESHOLD 35
#define MAX_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)

/* Hi speed to bump to from lo speed when load burst (default max) */
#define DEFAULT_HISPEED_FREQ 800000
static u64 hispeed_freq;

/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 95
static unsigned long go_hispeed_load;

/* Target load.  Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 90
static unsigned long target_load = DEFAULT_TARGET_LOAD;

/*
 * The minimum amount of time to spend at a frequency before we can ramp down.
 */
#define DEFAULT_MIN_SAMPLE_TIME (20 * USEC_PER_MSEC)
static unsigned long min_sample_time;

/*
 * The sample rate of the timer used to increase frequency
 */
#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC)
static unsigned long timer_rate;

/*
 * Wait this long before raising speed above hispeed, by default a single
 * timer interval.
 */
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_TIMER_RATE
static unsigned long above_hispeed_delay_val;

/*
 * Boost pulse to hispeed on touchscreen input.
 */

/* Enable input boost by default. */
#define DEFAULT_INPUT_BOOST 1
static int input_boost_val = DEFAULT_INPUT_BOOST;

struct cpufreq_interactive_inputopen {
	struct input_handle *handle;
	struct work_struct inputopen_work;
};

static struct cpufreq_interactive_inputopen inputopen;

/*
 * Non-zero means longer-term speed boost active.
 */

static int boost_val;

/*
 * The CPU will be boosted to this frequency when the screen is
 * touched. input_boost needs to be enabled.
 */

static int input_boost_freq = 1000000;

/* Duration of a boot pulse in usecs */
static int boostpulse_duration_val = DEFAULT_MIN_SAMPLE_TIME;
/* End time of boost pulse in ktime converted to usecs */
static u64 boostpulse_endtime;

static int cpufreq_governor_interactive(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE
static
#endif
struct cpufreq_governor cpufreq_gov_interactive = {
	.name = "interactive",
	.governor = cpufreq_governor_interactive,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static inline cputime64_t get_cpu_iowait_time(
	unsigned int cpu, cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

static void cpufreq_interactive_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_idle;
	unsigned int delta_iowait;
	unsigned int delta_time;
	int cpu_load;
	int load_since_change;
	u64 time_in_idle;
	u64 time_in_iowait;
	u64 idle_exit_time;
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	u64 now_idle;
	u64 now_iowait;
	unsigned int new_freq, new_tune_value;
	unsigned int index, i, j;
	unsigned long flags;
	bool boosted;

	now = 0;

	smp_rmb();

	if (!pcpu->governor_enabled)
		goto exit;

	/*
	 * Once pcpu->timer_run_time is updated to >= pcpu->idle_exit_time,
	 * this lets idle exit know the current idle time sample has
	 * been processed, and idle exit can generate a new sample and
	 * re-arm the timer.  This prevents a concurrent idle
	 * exit on that CPU from writing a new set of info at the same time
	 * the timer function runs (the timer function can't use that info
	 * until more time passes).
	 */
	time_in_idle = pcpu->time_in_idle;
	time_in_iowait = pcpu->time_in_iowait;
	idle_exit_time = pcpu->idle_exit_time;
	now_idle = get_cpu_idle_time_us(data, &pcpu->timer_run_time);
	now_iowait = get_cpu_iowait_time(data, NULL);
	smp_wmb();

	/* If we raced with cancelling a timer, skip. */
	if (!idle_exit_time)
		goto exit;

	delta_idle = (unsigned int) cputime64_sub(now_idle, time_in_idle);
	delta_iowait = (unsigned int) cputime64_sub(now_iowait, time_in_iowait);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  idle_exit_time);

	/*
	 * If timer ran less than 1ms after short-term sample started, retry.
	 */
	if (delta_time < 1000)
		goto rearm;

	if (delta_idle > delta_time)
		cpu_load = 0;
	else {
		if (io_is_busy && delta_idle >= delta_iowait)
			delta_idle -= delta_iowait;

		cpu_load = 100 * (delta_time - delta_idle) / delta_time;
	}

	delta_idle = (unsigned int) cputime64_sub(now_idle,
						pcpu->target_set_time_in_idle);
	delta_iowait = (unsigned int) cputime64_sub(now_iowait,
						pcpu->freq_change_time_in_iowait);
	delta_time = (unsigned int) cputime64_sub(pcpu->timer_run_time,
						  pcpu->target_set_time);

	if ((delta_time == 0) || (delta_idle > delta_time))
		load_since_change = 0;
	else {
		if (io_is_busy && delta_idle >= delta_iowait)
			delta_idle -= delta_iowait;

		load_since_change =
			100 * (delta_time - delta_idle) / delta_time;
	}

	/*
	 * Choose greater of short-term load (since last idle timer
	 * started or timer function re-armed itself) or long-term load
	 * (since last frequency change).
	 */
	if (load_since_change > cpu_load)
		cpu_load = load_since_change;
	pcpu->load_history[history_load_index] = cpu_load;

	pcpu->total_load_history = 0;
	pcpu->low_power_rate_history = 0;

	/* compute average load across in & out sampling periods */
	for (i = 0, j = history_load_index; i < sampling_periods; i++, j--) {
		pcpu->total_load_history += pcpu->load_history[j];
		if (low_power_rate < sampling_periods)
			if (i < low_power_rate)
				pcpu->low_power_rate_history
						  += pcpu->load_history[j];
		if (j == 0)
			j = sampling_periods;
	}

	/* return to first element if we're at the circular buffer's end */
	if (++history_load_index == sampling_periods)
		history_load_index = 0;

	pcpu->total_avg_load = pcpu->total_load_history / sampling_periods;

	if (pcpu->total_avg_load > hi_perf_threshold)
		new_tune_value = HIGH_PERF_TUNE;
	else if (pcpu->total_avg_load < low_power_threshold)
		new_tune_value = LOW_POWER_TUNE;
	else
		new_tune_value = DEFAULT_TUNE;

	if (new_tune_value != cur_tune_value)
		if ((pcpu->cpu_tune_value != new_tune_value)
			&& ((new_tune_value == HIGH_PERF_TUNE)
				|| (new_tune_value == LOW_POWER_TUNE))) {
			spin_lock_irqsave(&tune_cpumask_lock, flags);
			cpumask_set_cpu(data, &tune_cpumask);
			spin_unlock_irqrestore(&tune_cpumask_lock, flags);
			queue_work(tune_wq, &tune_work);
		}
	pcpu->cpu_tune_value = new_tune_value;

	if (cur_tune_value == LOW_POWER_TUNE) {
		if (low_power_rate < sampling_periods)
			cpu_load = pcpu->low_power_rate_history
						/ low_power_rate;
		else
			cpu_load = pcpu->total_avg_load;
	}
	
	boosted = boost_val || now < boostpulse_endtime;

	if (cpu_load >= go_hispeed_load || boosted) {
		if (pcpu->target_freq < hispeed_freq &&
		    hispeed_freq < pcpu->policy->max) {
			new_freq = hispeed_freq;
		} else {
			new_freq = pcpu->policy->cur * cpu_load / target_load;

			if (new_freq < hispeed_freq)
				new_freq = hispeed_freq;

			if (pcpu->target_freq == hispeed_freq &&
			    new_freq > hispeed_freq &&
			    cputime64_sub(pcpu->timer_run_time,
					  pcpu->hispeed_validate_time)
			    < above_hispeed_delay_val) {
				trace_cpufreq_interactive_notyet(data, cpu_load,
								 pcpu->target_freq,
								 new_freq);
				goto rearm;
			}
		}
	} else {
		new_freq = pcpu->policy->cur * cpu_load / target_load;
	}

	if (new_freq <= hispeed_freq)
		pcpu->hispeed_validate_time = pcpu->timer_run_time;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_L,
					   &index)) {
		printk_once(KERN_WARNING "timer %d: cpufreq_frequency_table_target error\n",
			     (int) data);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (new_freq < pcpu->floor_freq) {
		if (cputime64_sub(pcpu->timer_run_time,
				  pcpu->floor_validate_time)
		    < min_sample_time) {
			trace_cpufreq_interactive_notyet(data, cpu_load,
					 pcpu->target_freq, new_freq);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to hispeed_freq.  If boosted to hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (!boosted || new_freq > hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = pcpu->timer_run_time;
	}

	if (pcpu->target_freq == new_freq) {
		trace_cpufreq_interactive_already(data, cpu_load,
						  pcpu->target_freq, new_freq);
		goto rearm_if_notmax;
	}

	trace_cpufreq_interactive_target(data, cpu_load, pcpu->target_freq,
					 new_freq);
	pcpu->target_set_time_in_idle = now_idle;
	pcpu->target_set_time = pcpu->timer_run_time;

	if (new_freq < pcpu->target_freq) {
		pcpu->target_freq = new_freq;
		spin_lock_irqsave(&down_cpumask_lock, flags);
		cpumask_set_cpu(data, &down_cpumask);
		spin_unlock_irqrestore(&down_cpumask_lock, flags);
		queue_work(down_wq, &freq_scale_down_work);
	} else {
		pcpu->target_freq = new_freq;
		spin_lock_irqsave(&up_cpumask_lock, flags);
		cpumask_set_cpu(data, &up_cpumask);
		spin_unlock_irqrestore(&up_cpumask_lock, flags);
		wake_up_process(up_task);
	}

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
		goto exit;

rearm:
	if (!timer_pending(&pcpu->cpu_timer)) {
		/*
		 * If already at min: if that CPU is idle, don't set timer.
		 * Else cancel the timer if that CPU goes idle.  We don't
		 * need to re-evaluate speed until the next idle exit.
		 */
		if (pcpu->target_freq == pcpu->policy->min) {
			smp_rmb();

			if (pcpu->idling)
				goto exit;

			pcpu->timer_idlecancel = 1;
		}

		pcpu->time_in_idle = get_cpu_idle_time_us(
			data, &pcpu->idle_exit_time);
		pcpu->time_in_iowait = get_cpu_iowait_time(
			data, NULL);

		mod_timer(&pcpu->cpu_timer,
			  jiffies + usecs_to_jiffies(timer_rate));
	}

exit:
	return;
}

static void cpufreq_interactive_tune(struct work_struct *work)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	unsigned int max_total_avg_load = 0;
	unsigned int index;

	spin_lock_irqsave(&tune_cpumask_lock, flags);
	tmp_mask = tune_cpumask;
	cpumask_clear(&tune_cpumask);
	spin_unlock_irqrestore(&tune_cpumask_lock, flags);

	for_each_cpu(cpu, &tmp_mask) {
		unsigned int j;

		pcpu = &per_cpu(cpuinfo, cpu);
		smp_rmb();

		if (!pcpu->governor_enabled)
			continue;

		mutex_lock(&set_speed_lock);

		for_each_cpu(j, pcpu->policy->cpus) {
			struct cpufreq_interactive_cpuinfo *pjcpu =
					&per_cpu(cpuinfo, j);

			if (pjcpu->total_avg_load > max_total_avg_load)
				max_total_avg_load = pjcpu->total_avg_load;
		}

		if ((max_total_avg_load > hi_perf_threshold)
				&& (cur_tune_value != HIGH_PERF_TUNE)) {
				cur_tune_value = HIGH_PERF_TUNE;
				go_hispeed_load = MIN_GO_HISPEED_LOAD;
				min_sample_time = MAX_MIN_SAMPLE_TIME;
				hispeed_freq = pcpu->policy->max;
		} else if ((max_total_avg_load < low_power_threshold)
				&& (cur_tune_value != LOW_POWER_TUNE)) {
			/* Boost down the performance */
				go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
				min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
				cpufreq_frequency_table_target(pcpu->policy,
					pcpu->freq_table, pcpu->policy->min,
					CPUFREQ_RELATION_H, &index);
				hispeed_freq =
					pcpu->freq_table[index-1].frequency;
				cur_tune_value = LOW_POWER_TUNE;
		}
		mutex_unlock(&set_speed_lock);
	}

}

static void cpufreq_interactive_idle_start(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());
	int pending;

	if (!pcpu->governor_enabled)
		return;

	pcpu->idling = 1;
	smp_wmb();
	pending = timer_pending(&pcpu->cpu_timer);

	if (pcpu->target_freq != pcpu->policy->min) {
#ifdef CONFIG_SMP
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending) {
			pcpu->time_in_idle = get_cpu_idle_time_us(
				smp_processor_id(), &pcpu->idle_exit_time);
			pcpu->time_in_iowait = get_cpu_iowait_time(
				smp_processor_id(), NULL);
			pcpu->timer_idlecancel = 0;
			mod_timer(&pcpu->cpu_timer,
				  jiffies + usecs_to_jiffies(timer_rate));
		}
#endif
	} else {
		/*
		 * If at min speed and entering idle after load has
		 * already been evaluated, and a timer has been set just in
		 * case the CPU suddenly goes busy, cancel that timer.  The
		 * CPU didn't go busy; we'll recheck things upon idle exit.
		 */
		if (pending && pcpu->timer_idlecancel) {
			del_timer(&pcpu->cpu_timer);
			/*
			 * Ensure last timer run time is after current idle
			 * sample start time, so next idle exit will always
			 * start a new idle sampling period.
			 */
			pcpu->idle_exit_time = 0;
			pcpu->timer_idlecancel = 0;
		}
	}

}

static void cpufreq_interactive_idle_end(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	pcpu->idling = 0;
	smp_wmb();

	/*
	 * Arm the timer for 1-2 ticks later if not already, and if the timer
	 * function has already processed the previous load sampling
	 * interval.  (If the timer is not pending but has not processed
	 * the previous interval, it is probably racing with us on another
	 * CPU.  Let it compute load based on the previous sample and then
	 * re-arm the timer for another interval when it's done, rather
	 * than updating the interval start time to be "now", which doesn't
	 * give the timer function enough time to make a decision on this
	 * run.)
	 */
	if (timer_pending(&pcpu->cpu_timer) == 0 &&
	    pcpu->timer_run_time >= pcpu->idle_exit_time &&
	    pcpu->governor_enabled) {
		pcpu->time_in_idle =
			get_cpu_idle_time_us(smp_processor_id(),
					     &pcpu->idle_exit_time);
		pcpu->time_in_iowait =
			get_cpu_iowait_time(smp_processor_id(),
				NULL);
		pcpu->timer_idlecancel = 0;
		mod_timer(&pcpu->cpu_timer,
			  jiffies + usecs_to_jiffies(timer_rate));
	}

}

static int cpufreq_interactive_up_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&up_cpumask_lock, flags);

		if (cpumask_empty(&up_cpumask)) {
			spin_unlock_irqrestore(&up_cpumask_lock, flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&up_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = up_cpumask;
		cpumask_clear(&up_cpumask);
		spin_unlock_irqrestore(&up_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;

			pcpu = &per_cpu(cpuinfo, cpu);
			smp_rmb();

			if (!pcpu->governor_enabled)
				continue;

			mutex_lock(&set_speed_lock);

			for_each_cpu(j, pcpu->policy->cpus) {
				struct cpufreq_interactive_cpuinfo *pjcpu =
					&per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq)
					max_freq = pjcpu->target_freq;
			}

			if (max_freq != pcpu->policy->cur)
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
			mutex_unlock(&set_speed_lock);
			trace_cpufreq_interactive_up(cpu, pcpu->target_freq,
						     pcpu->policy->cur);

			pcpu->freq_change_time_in_iowait =
				get_cpu_iowait_time(cpu, NULL);
		}
	}

	return 0;
}

static void cpufreq_interactive_freq_down(struct work_struct *work)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	spin_lock_irqsave(&down_cpumask_lock, flags);
	tmp_mask = down_cpumask;
	cpumask_clear(&down_cpumask);
	spin_unlock_irqrestore(&down_cpumask_lock, flags);

	for_each_cpu(cpu, &tmp_mask) {
		unsigned int j;
		unsigned int max_freq = 0;

		pcpu = &per_cpu(cpuinfo, cpu);
		smp_rmb();

		if (!pcpu->governor_enabled)
			continue;

		mutex_lock(&set_speed_lock);

		for_each_cpu(j, pcpu->policy->cpus) {
			struct cpufreq_interactive_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, j);

			if (pjcpu->target_freq > max_freq)
				max_freq = pjcpu->target_freq;
		}

		if (max_freq != pcpu->policy->cur)
			__cpufreq_driver_target(pcpu->policy, max_freq,
						CPUFREQ_RELATION_H);

		mutex_unlock(&set_speed_lock);
		trace_cpufreq_interactive_down(cpu, pcpu->target_freq,
					       pcpu->policy->cur);
		pcpu->freq_change_time_in_iowait =
			get_cpu_iowait_time(cpu, NULL);
	}
}

static void cpufreq_interactive_boost(void)
{
	int i;
	int anyboost = 0;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	spin_lock_irqsave(&up_cpumask_lock, flags);

	for_each_online_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);

		if (pcpu->target_freq < input_boost_freq) {
			pcpu->target_freq = input_boost_freq;
			cpumask_set_cpu(i, &up_cpumask);
			pcpu->target_set_time_in_idle =
				get_cpu_idle_time_us(i, &pcpu->target_set_time);
			pcpu->hispeed_validate_time = pcpu->target_set_time;
			anyboost = 1;
		}

		/*
		 * Set floor freq and (re)start timer for when last
		 * validated.
		 */

		pcpu->floor_freq = input_boost_freq;
		pcpu->floor_validate_time = ktime_to_us(ktime_get());
	}

	spin_unlock_irqrestore(&up_cpumask_lock, flags);

	if (anyboost)
		wake_up_process(up_task);
}

/*
 * Pulsed boost on input event raises CPUs to hispeed_freq and lets
 * usual algorithm of min_sample_time  decide when to allow speed
 * to drop.
 */

static void cpufreq_interactive_input_event(struct input_handle *handle,
					    unsigned int type,
					    unsigned int code, int value)
{
	if (input_boost_val && type == EV_SYN && code == SYN_REPORT) {
		trace_cpufreq_interactive_boost("input");
		cpufreq_interactive_boost();
	}
}

static void cpufreq_interactive_input_open(struct work_struct *w)
{
	struct cpufreq_interactive_inputopen *io =
		container_of(w, struct cpufreq_interactive_inputopen,
			     inputopen_work);
	int error;

	error = input_open_device(io->handle);
	if (error)
		input_unregister_handle(io->handle);
}

static int cpufreq_interactive_input_connect(struct input_handler *handler,
					     struct input_dev *dev,
					     const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	pr_info("%s: connect to %s\n", __func__, dev->name);
	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq_interactive";

	error = input_register_handle(handle);
	if (error)
		goto err;

	inputopen.handle = handle;
	queue_work(down_wq, &inputopen.inputopen_work);
	return 0;
err:
	kfree(handle);
	return error;
}

static void cpufreq_interactive_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpufreq_interactive_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) |
			    BIT_MASK(ABS_MT_POSITION_Y) },
	}, /* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	}, /* touchpad */
	{ },
};

static struct input_handler cpufreq_interactive_input_handler = {
	.event          = cpufreq_interactive_input_event,
	.connect        = cpufreq_interactive_input_connect,
	.disconnect     = cpufreq_interactive_input_disconnect,
	.name           = "cpufreq_interactive",
	.id_table       = cpufreq_interactive_ids,
};

static ssize_t show_target_load(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", target_load);
}

static ssize_t store_target_load(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	target_load = val;
	return count;
}

static struct global_attr target_load_attr =
	__ATTR(target_load, S_IRUGO | S_IWUSR,
	show_target_load, store_target_load);

static ssize_t show_hispeed_freq(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", hispeed_freq);
}

static ssize_t store_hispeed_freq(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	u64 val;

	ret = strict_strtoull(buf, 0, &val);
	if (ret < 0)
		return ret;
	hispeed_freq = val;
	return count;
}

static struct global_attr hispeed_freq_attr = __ATTR(hispeed_freq, 0644,
		show_hispeed_freq, store_hispeed_freq);

static ssize_t show_io_is_busy(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", io_is_busy);
}

static ssize_t store_io_is_busy(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	if (!strict_strtoul(buf, 0, &io_is_busy))
		return count;
	return -EINVAL;
}

static struct global_attr io_is_busy_attr = __ATTR(io_is_busy, 0644,
		show_io_is_busy, store_io_is_busy);

static ssize_t show_go_hispeed_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	go_hispeed_load = val;
	return count;
}

static struct global_attr go_hispeed_load_attr = __ATTR(go_hispeed_load, 0644,
		show_go_hispeed_load, store_go_hispeed_load);

static ssize_t show_min_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", min_sample_time);
}

static ssize_t store_min_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	min_sample_time = val;
	return count;
}

static struct global_attr min_sample_time_attr = __ATTR(min_sample_time, 0644,
		show_min_sample_time, store_min_sample_time);

static ssize_t show_above_hispeed_delay(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", above_hispeed_delay_val);
}

static ssize_t store_above_hispeed_delay(struct kobject *kobj,
					 struct attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	above_hispeed_delay_val = val;
	return count;
}

define_one_global_rw(above_hispeed_delay);

static ssize_t show_timer_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", timer_rate);
}

static ssize_t store_timer_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	timer_rate = val;
	return count;
}

static struct global_attr timer_rate_attr = __ATTR(timer_rate, 0644,
		show_timer_rate, store_timer_rate);

static ssize_t show_input_boost(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", input_boost_val);
}

static ssize_t store_input_boost(struct kobject *kobj, struct attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	input_boost_val = val;
	return count;
}

define_one_global_rw(input_boost);

static ssize_t show_boost(struct kobject *kobj, struct attribute *attr,
			  char *buf)
{
	return sprintf(buf, "%d\n", boost_val);
}

static ssize_t store_boost(struct kobject *kobj, struct attribute *attr,
			   const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boost_val = val;

	if (boost_val) {
		trace_cpufreq_interactive_boost("on");
		cpufreq_interactive_boost();
	} else {
		trace_cpufreq_interactive_unboost("off");
	}

	return count;
}

define_one_global_rw(boost);

static ssize_t store_boostpulse(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boostpulse_endtime = ktime_to_us(ktime_get()) + boostpulse_duration_val;
	trace_cpufreq_interactive_boost("pulse");
	cpufreq_interactive_boost();
	return count;
}

static struct global_attr boostpulse =
	__ATTR(boostpulse, 0200, NULL, store_boostpulse);

static ssize_t show_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", boostpulse_duration_val);
}

static ssize_t store_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boostpulse_duration_val = val;
	return count;
}

define_one_global_rw(boostpulse_duration);

static ssize_t show_input_boost_freq(struct kobject *kobj, struct attribute *attr,
			  char *buf)
{
	return sprintf(buf, "%d\n", input_boost_freq);
}

static ssize_t store_input_boost_freq(struct kobject *kobj, struct attribute *attr,
			   const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	input_boost_freq = val;

	return count;
}

static struct global_attr input_boost_freq_attr = __ATTR(input_boost_freq, 0644,
		show_input_boost_freq, store_input_boost_freq);

static ssize_t show_sampling_periods(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sampling_periods);
}

static ssize_t store_sampling_periods(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int val;
	unsigned int *temp;
	unsigned int j, i;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1)
		return ret;

	if (val == sampling_periods)
		return count;

	if (val <= sampling_periods) {
		sampling_periods = val;
		history_load_index = 0;
		return count;
	}

	mutex_lock(&set_speed_lock);

	for_each_online_cpu(j) {
		struct cpufreq_interactive_cpuinfo *pcpu;

		temp = kmalloc((sizeof(unsigned int) * val), GFP_KERNEL);
		if (!temp) {
			mutex_unlock(&set_speed_lock);
			pr_err("%s:can't allocate memory for history\n",
					__func__);
			return -ENOMEM;
		}
		pcpu = &per_cpu(cpuinfo, j);
		ret = del_timer_sync(&pcpu->cpu_timer);
		memcpy(temp, pcpu->load_history,
				(sampling_periods * sizeof(unsigned int)));
		for (i = sampling_periods; i < val; i++)
			temp[i] = 50;

		kfree(pcpu->load_history);
		pcpu->load_history = temp;

		if (ret)
			mod_timer(&pcpu->cpu_timer,
				  jiffies + usecs_to_jiffies(timer_rate));
	}
	sampling_periods = val;
	history_load_index = 0;

	mutex_unlock(&set_speed_lock);

	return count;
}

static struct global_attr sampling_periods_attr = __ATTR(sampling_periods,
			0644, show_sampling_periods, store_sampling_periods);

static ssize_t show_hi_perf_threshold(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", hi_perf_threshold);
}

static ssize_t store_hi_perf_threshold(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	hi_perf_threshold = val;
	return count;
}

static struct global_attr hi_perf_threshold_attr = __ATTR(hi_perf_threshold,
			0644, show_hi_perf_threshold, store_hi_perf_threshold);


static ssize_t show_low_power_threshold(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", low_power_threshold);
}

static ssize_t store_low_power_threshold(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	low_power_threshold = val;
	return count;
}

static struct global_attr low_power_threshold_attr = __ATTR(low_power_threshold,
		     0644, show_low_power_threshold, store_low_power_threshold);

static ssize_t show_low_power_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", low_power_rate);
}

static ssize_t store_low_power_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	low_power_rate = val;
	return count;
}

static struct global_attr low_power_rate_attr = __ATTR(low_power_rate,
		     0644, show_low_power_rate, store_low_power_rate);


static struct attribute *interactive_attributes[] = {
	&target_load_attr.attr,
	&hispeed_freq_attr.attr,
	&go_hispeed_load_attr.attr,
	&above_hispeed_delay.attr,
	&min_sample_time_attr.attr,
	&io_is_busy_attr.attr,
	&timer_rate_attr.attr,
	&input_boost.attr,
	&boost.attr,
	&boostpulse.attr,
	&input_boost_freq_attr.attr,
	&low_power_threshold_attr.attr,
	&hi_perf_threshold_attr.attr,
	&sampling_periods_attr.attr,
	&low_power_rate_attr.attr,
	&boostpulse_duration.attr,
	NULL,
};

static struct attribute_group interactive_attr_group = {
	.attrs = interactive_attributes,
	.name = "interactive",
};

static int cpufreq_governor_interactive(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j, i;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;

	switch (event) {
	case CPUFREQ_GOV_START:
		if (!cpu_online(policy->cpu))
			return -EINVAL;

		freq_table =
			cpufreq_frequency_get_table(policy->cpu);

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->target_set_time_in_idle =
				get_cpu_idle_time_us(j,
					     &pcpu->target_set_time);
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				pcpu->target_set_time;
			pcpu->freq_change_time_in_iowait =
				get_cpu_iowait_time(j, NULL);
			pcpu->time_in_iowait = pcpu->freq_change_time_in_iowait;

			pcpu->hispeed_validate_time =
				pcpu->target_set_time;
			pcpu->governor_enabled = 1;
			pcpu->load_history = kmalloc(
				(sizeof(unsigned int) * sampling_periods),
				 GFP_KERNEL);
			if (!pcpu->load_history)
				return -ENOMEM;
			for (i = 0; i < sampling_periods; i++)
				pcpu->load_history[i] = 0;
			smp_wmb();
		}

		if (!hispeed_freq) {
			hispeed_freq = policy->max;
			input_boost_freq = hispeed_freq;
		}
		history_load_index = 0;

		/*
		 * Do not register the idle hook and create sysfs
		 * entries if we have already done so.
		 */
		if (atomic_inc_return(&active_count) > 1)
			return 0;

		rc = sysfs_create_group(cpufreq_global_kobject,
				&interactive_attr_group);
		if (rc)
			return rc;

		rc = input_register_handler(&cpufreq_interactive_input_handler);
		if (rc)
			printk(KERN_WARNING "%s: failed to register input handler\n",
				__func__);

		break;

	case CPUFREQ_GOV_STOP:
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->governor_enabled = 0;
			smp_wmb();
			del_timer_sync(&pcpu->cpu_timer);

			/*
			 * Reset idle exit time since we may cancel the timer
			 * before it can run after the last idle exit time,
			 * to avoid tripping the check in idle exit for a timer
			 * that is trying to run.
			 */
			pcpu->idle_exit_time = 0;
			kfree(pcpu->load_history);
		}

		flush_work(&freq_scale_down_work);
		flush_work(&tune_work);

		if (atomic_dec_return(&active_count) > 0)
			return 0;

		input_unregister_handler(&cpufreq_interactive_input_handler);
		sysfs_remove_group(cpufreq_global_kobject,
				&interactive_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
			__cpufreq_driver_target(policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
			__cpufreq_driver_target(policy,
					policy->min, CPUFREQ_RELATION_L);
		break;
	}
	return 0;
}

static int cpufreq_interactive_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	switch (val) {
	case IDLE_START:
		cpufreq_interactive_idle_start();
		break;
	case IDLE_END:
		cpufreq_interactive_idle_end();
		break;
	}

	return 0;
}

static struct notifier_block cpufreq_interactive_idle_nb = {
	.notifier_call = cpufreq_interactive_idle_notifier,
};

static int __init cpufreq_interactive_init(void)
{
	unsigned int i;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	hispeed_freq = DEFAULT_HISPEED_FREQ;
	go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	above_hispeed_delay_val = DEFAULT_ABOVE_HISPEED_DELAY;
	timer_rate = DEFAULT_TIMER_RATE;
	input_boost_val = DEFAULT_INPUT_BOOST;

	sampling_periods = DEFAULT_SAMPLING_PERIODS;
	hi_perf_threshold = DEFAULT_HI_PERF_THRESHOLD;
	low_power_threshold = DEFAULT_LOW_POWER_THRESHOLD;
	low_power_rate = DEFAULT_LOW_POWER_RATE;
	cur_tune_value = DEFAULT_TUNE;
	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_interactive_timer;
		pcpu->cpu_timer.data = i;
		pcpu->cpu_tune_value = DEFAULT_TUNE;
	}

	up_task = kthread_create(cpufreq_interactive_up_task, NULL,
				 "kinteractiveup");
	if (IS_ERR(up_task))
		return PTR_ERR(up_task);

	sched_setscheduler_nocheck(up_task, SCHED_FIFO, &param);
	get_task_struct(up_task);

	/* No rescuer thread, bind to CPU queuing the work for possibly
	   warm cache (probably doesn't matter much). */
	down_wq = create_workqueue("knteractive_down");
	tune_wq = create_workqueue("knteractive_tune");

	if (!down_wq)
		goto err_freeuptask;

	INIT_WORK(&freq_scale_down_work,
		  cpufreq_interactive_freq_down);

	INIT_WORK(&tune_work,
		  cpufreq_interactive_tune);

	spin_lock_init(&up_cpumask_lock);
	spin_lock_init(&down_cpumask_lock);
	spin_lock_init(&tune_cpumask_lock);
	mutex_init(&set_speed_lock);

	idle_notifier_register(&cpufreq_interactive_idle_nb);
	INIT_WORK(&inputopen.inputopen_work, cpufreq_interactive_input_open);
	return cpufreq_register_governor(&cpufreq_gov_interactive);

err_freeuptask:
	put_task_struct(up_task);
	return -ENOMEM;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE
fs_initcall(cpufreq_interactive_init);
#else
module_init(cpufreq_interactive_init);
#endif

static void __exit cpufreq_interactive_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_interactive);
	kthread_stop(up_task);
	put_task_struct(up_task);
	destroy_workqueue(down_wq);
	destroy_workqueue(tune_wq);
}

module_exit(cpufreq_interactive_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com>");
MODULE_DESCRIPTION("'cpufreq_interactive' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPL");

