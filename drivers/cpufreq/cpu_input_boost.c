/*
<<<<<<< HEAD
 * Copyright (C) 2014-2017, Sultanxda <sultanxda@gmail.com>
=======
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/slab.h>

#define CPU_MASK(cpu) (1U << (cpu))

/*
 * For MSM8996 (big.LITTLE). CPU0 and CPU1 are LITTLE CPUs; CPU2 and CPU3 are
 * big CPUs.
 */
#define LITTLE_CPU_MASK (CPU_MASK(0) | CPU_MASK(1))
#define BIG_CPU_MASK    (CPU_MASK(2) | CPU_MASK(3))

/* Available bits for boost_policy state */
#define DRIVER_ENABLED        (1U << 0)
#define SCREEN_AWAKE          (1U << 1)
#define WAKE_BOOST            (1U << 2)
#define INPUT_BOOST           (1U << 3)
#define INPUT_REBOOST         (1U << 4)

/* The duration in milliseconds for the wake boost */
#define FB_BOOST_MS (3000)

/*
 * "fb" = "framebuffer". This is the boost that occurs on framebuffer unblank,
 * AKA when the screen is turned on (wake boost). All online CPUs are boosted
 * to policy->max when this occurs.
 */
struct fb_policy {
	struct work_struct boost_work;
	struct delayed_work unboost_work;
};

/*
 * "ib_pcpu" = "input boost per CPU". This contains the unboost worker used to
 * unboost a single CPU. Useful for when boost durations are not the same
 * across all the CPUs that are boosted (i.e. one CPU can be unboosted earlier
 * than another CPU).
 */
struct ib_pcpu {
	struct delayed_work unboost_work;
	uint32_t cpu;
};

/*
 * "ib_config" = "input-boost configuration". This contains the data and
 * workers used for a single input-boost event.
 */
struct ib_config {
	struct ib_pcpu __percpu *boost_info;
	struct work_struct boost_work;
	struct work_struct reboost_work;
	uint32_t adj_duration_ms;
	uint32_t cpus_to_boost;
	uint32_t duration_ms;
	uint32_t freq[2];
};

/*
 * This is the struct that contains all of the data for the entire driver. It
 * encapsulates all of the other structs, so all data can be accessed through
 * this struct.
 */
struct boost_policy {
	spinlock_t lock;
	struct fb_policy fb;
	struct ib_config ib;
	struct workqueue_struct *wq;
	uint32_t state;
};

/* Global pointer to all of the data for the driver */
static struct boost_policy *boost_policy_g;

static void ib_boost_cpus(struct boost_policy *b);
static uint32_t get_boost_freq(struct boost_policy *b, uint32_t cpu);
static uint32_t get_boost_state(struct boost_policy *b);
static void set_boost_freq(struct boost_policy *b,
		uint32_t cpu, uint32_t freq);
static void set_boost_bit(struct boost_policy *b, uint32_t state);
static void clear_boost_bit(struct boost_policy *b, uint32_t state);
static void unboost_all_cpus(struct boost_policy *b);
static void update_online_cpu_policy(void);
static bool validate_cpu_freq(struct cpufreq_frequency_table *pos,
		uint32_t *freq);

static void ib_boost_main(struct work_struct *work)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t cpu;

	/* Always boost CPU0 */
	ib->cpus_to_boost = CPU_MASK(0);

	/* Copy the user-set boost duration since it will be altered below */
	ib->adj_duration_ms = ib->duration_ms;

	get_online_cpus();

	/* Start from CPU1 since CPU0 is always boosted */
	for (cpu = 1; cpu < num_possible_cpus(); cpu++) {
		struct cpufreq_policy policy;
		uint32_t boost_freq, freq;
		int ret;

		ret = cpufreq_get_policy(&policy, cpu);
		if (ret)
			continue;

		freq = policy.cur;
		if (freq == policy.min)
			continue;

		boost_freq = get_boost_freq(b, cpu);

		/*
		 * Increase or decrease the boost duration for all CPUs by
		 * dividing each CPU's boost freq by its current freq, and
		 * multiplying the user-set duration by that fraction. CPUs
		 * with a current freq higher than their boost freq will reduce
		 * the overall boost duration, whereas CPUs with a current
		 * freq lower than the boost freq will increase the duration.
		 * CPUs that are running at their min freq are ignored as they
		 * could be idling and increase the boost duration too much.
		 */
		ib->adj_duration_ms *= boost_freq * 100 / freq;
		ib->adj_duration_ms /= 100;

		/*
		 * Only allow two CPUs to be boosted at any given time. The 2nd
		 * CPU that is boosted is one that is running at a freq greater
		 * than its min freq (not idling) but lower than its boost
		 * freq.
		 */
		if (freq < boost_freq && ib->cpus_to_boost == CPU_MASK(0))
			ib->cpus_to_boost |= CPU_MASK(cpu);
	}

	/* Make sure boosts don't become too long or too short */
	ib->adj_duration_ms = max(ib->duration_ms / 4, ib->adj_duration_ms);
	ib->adj_duration_ms = min(2 * ib->duration_ms, ib->adj_duration_ms);

	/* Boost CPUs specified in ib->cpus_to_boost */
	ib_boost_cpus(b);

	put_online_cpus();
}

static void ib_unboost_main(struct work_struct *work)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	struct ib_pcpu *pcpu = container_of(work, typeof(*pcpu),
						unboost_work.work);

	/* Unboost a single CPU */
	ib->cpus_to_boost &= ~CPU_MASK(pcpu->cpu);

	/* Update the CPU's min freq now if it's online */
	get_online_cpus();
	if (cpu_online(pcpu->cpu))
		cpufreq_update_policy(pcpu->cpu);
	put_online_cpus();

	/*
	 * All CPUs are unboosted. Clear the input-boost bit so we can accept
	 * new boosts.
	 */
	if (!ib->cpus_to_boost)
		clear_boost_bit(b, INPUT_BOOST);
}

static void ib_reboost_main(struct work_struct *work)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	struct ib_pcpu *pcpu = per_cpu_ptr(ib->boost_info, 0 /* CPU0 */);

	/* Only keep CPU0 boosted (more efficient) */
	if (cancel_delayed_work_sync(&pcpu->unboost_work))
		queue_delayed_work(b->wq, &pcpu->unboost_work,
			msecs_to_jiffies(ib->adj_duration_ms));

	/* Clear reboost bit */
	clear_boost_bit(b, INPUT_REBOOST);
}

static void fb_boost_main(struct work_struct *work)
{
	struct boost_policy *b = boost_policy_g;
	struct fb_policy *fb = &b->fb;

	/* All CPUs will be boosted to policy->max */
	set_boost_bit(b, WAKE_BOOST);

	/* Immediately boost the online CPUs */
	update_online_cpu_policy();

	queue_delayed_work(b->wq, &fb->unboost_work,
				msecs_to_jiffies(FB_BOOST_MS));
}

static void fb_unboost_main(struct work_struct *work)
{
	struct boost_policy *b = boost_policy_g;

	/* This clears the wake-boost bit and unboosts everything */
	unboost_all_cpus(b);
}

static int do_cpu_boost(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct cpufreq_policy *policy = data;
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t boost_freq, state;
	bool ret;
=======
/* Available bits for boost_drv state */
#define SCREEN_AWAKE		(1U << 0)
#define INPUT_BOOST		(1U << 1)
#define WAKE_BOOST		(1U << 2)

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct input_boost;
	struct delayed_work input_unboost;
	struct work_struct wake_boost;
	struct delayed_work wake_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block fb_notif;
	spinlock_t lock;
	u32 state;
};

static struct boost_drv *boost_drv_g;

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->input_boost);
}

static u32 get_boost_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return CONFIG_INPUT_BOOST_FREQ_LP;

	return CONFIG_INPUT_BOOST_FREQ_PERF;
}

static u32 get_boost_state(struct boost_drv *b)
{
	u32 state;

	spin_lock(&b->lock);
	state = b->state;
	spin_unlock(&b->lock);

	return state;
}

static void set_boost_bit(struct boost_drv *b, u32 state)
{
	spin_lock(&b->lock);
	b->state |= state;
	spin_unlock(&b->lock);
}

static void clear_boost_bit(struct boost_drv *b, u32 state)
{
	spin_lock(&b->lock);
	b->state &= ~state;
	spin_unlock(&b->lock);
}

static void update_online_cpu_policy(void)
{
	u32 cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void unboost_all_cpus(struct boost_drv *b)
{
	if (!cancel_delayed_work_sync(&b->input_unboost) &&
		!cancel_delayed_work_sync(&b->wake_unboost))
		return;

	clear_boost_bit(b, WAKE_BOOST | INPUT_BOOST);
	update_online_cpu_policy();
}

static void input_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), input_boost);

	if (!cancel_delayed_work_sync(&b->input_unboost)) {
		set_boost_bit(b, INPUT_BOOST);
		update_online_cpu_policy();
	}

	queue_delayed_work(b->wq, &b->input_unboost,
		msecs_to_jiffies(CONFIG_INPUT_BOOST_DURATION_MS));
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), input_unboost);

	clear_boost_bit(b, INPUT_BOOST);
	update_online_cpu_policy();
}

static void wake_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), wake_boost);

	if (!cancel_delayed_work_sync(&b->wake_unboost)) {
		set_boost_bit(b, WAKE_BOOST);
		update_online_cpu_policy();
	}

	queue_delayed_work(b->wq, &b->wake_unboost,
		msecs_to_jiffies(CONFIG_WAKE_BOOST_DURATION_MS));
}

static void wake_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), wake_unboost);

	clear_boost_bit(b, WAKE_BOOST);
	update_online_cpu_policy();
}

static int cpu_notifier_cb(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	u32 boost_freq, state;
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = get_boost_state(b);

<<<<<<< HEAD
	/*
	 * Don't do anything when the driver is disabled, unless there are
	 * still CPUs that need to be unboosted.
	 */
	if (!(state & DRIVER_ENABLED) &&
		policy->min == policy->cpuinfo.min_freq)
		return NOTIFY_OK;

=======
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	/* Boost CPU to max frequency for wake boost */
	if (state & WAKE_BOOST) {
		policy->min = policy->max;
		return NOTIFY_OK;
	}

	/*
<<<<<<< HEAD
	 * Boost to policy->max if the boost frequency is higher than it. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (ib->cpus_to_boost & CPU_MASK(policy->cpu)) {
		boost_freq = get_boost_freq(b, policy->cpu);
		/*
		 * Boost frequency must always be valid. If it's invalid
		 * (validate_cpu_freq() returns true), then update the
		 * input-boost freq array with the validated frequency.
		 */
		ret = validate_cpu_freq(policy->freq_table, &boost_freq);
		if (ret)
			set_boost_freq(b, policy->cpu, boost_freq);
=======
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & INPUT_BOOST) {
		boost_freq = get_boost_freq(b, policy->cpu);
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
		policy->min = min(policy->max, boost_freq);
	} else {
		policy->min = policy->cpuinfo.min_freq;
	}

	return NOTIFY_OK;
}

<<<<<<< HEAD
static struct notifier_block do_cpu_boost_nb = {
	.notifier_call = do_cpu_boost,
};

static int fb_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct boost_policy *b = boost_policy_g;
	struct fb_policy *fb = &b->fb;
	struct fb_event *evdata = data;
	int *blank = evdata->data;
	uint32_t state;

	/* Parse framebuffer events as soon as they occur */
=======
static int fb_notifier_cb(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;
	u32 state;

	/* Parse framebuffer blank events as soon as they occur */
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	state = get_boost_state(b);

<<<<<<< HEAD
	/* Only boost for unblank (i.e. when the screen turns on) */
	switch (*blank) {
	case FB_BLANK_UNBLANK:
		/* Keep track of screen state */
		set_boost_bit(b, SCREEN_AWAKE);
		break;
	default:
		/* Unboost CPUs when the screen turns off */
		if (state & INPUT_BOOST || state & WAKE_BOOST)
			unboost_all_cpus(b);
		clear_boost_bit(b, SCREEN_AWAKE);
		return NOTIFY_OK;
	}

	/* Driver is disabled, so don't boost */
	if (!(state & DRIVER_ENABLED))
		return NOTIFY_OK;

	/* Framebuffer boost is already in progress */
	if (state & WAKE_BOOST)
		return NOTIFY_OK;

	queue_work(b->wq, &fb->boost_work);

	return NOTIFY_OK;
}

static struct notifier_block fb_notifier_callback_nb = {
	.notifier_call = fb_notifier_callback,
	.priority      = INT_MAX,
};

static void cpu_ib_input_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t state;

	state = get_boost_state(b);

	if (!(state & DRIVER_ENABLED) ||
		!(state & SCREEN_AWAKE) ||
		(state & WAKE_BOOST) ||
		(state & INPUT_REBOOST))
		return;

	/* Continuous boosting (from constant user input) */
	if (state & INPUT_BOOST) {
		set_boost_bit(b, INPUT_REBOOST);
		queue_work(b->wq, &ib->reboost_work);
		return;
	}

	set_boost_bit(b, INPUT_BOOST);
	queue_work(b->wq, &ib->boost_work);
}

static int cpu_ib_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
=======
	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == FB_BLANK_UNBLANK) {
		set_boost_bit(b, SCREEN_AWAKE);
		queue_work(b->wq, &b->wake_boost);
	} else {
		clear_boost_bit(b, SCREEN_AWAKE);
		unboost_all_cpus(b);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
	unsigned int type, unsigned int code, int value)
{
	struct boost_drv *b = handle->handler->private;
	u32 state;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	queue_work(b->wq, &b->input_boost);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
	struct input_dev *dev, const struct input_device_id *id)
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
<<<<<<< HEAD
	handle->name = "cpu_ib_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto err2;

	ret = input_open_device(handle);
	if (ret)
		goto err1;

	return 0;

err1:
	input_unregister_handle(handle);
err2:
=======
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	kfree(handle);
	return ret;
}

<<<<<<< HEAD
static void cpu_ib_input_disconnect(struct input_handle *handle)
=======
static void cpu_input_boost_input_disconnect(struct input_handle *handle)
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

<<<<<<< HEAD
static const struct input_device_id cpu_ib_ids[] = {
	/* multi-touch touchscreen */
=======
static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
<<<<<<< HEAD
	/* touchpad */
=======
	/* Touchpad */
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
<<<<<<< HEAD
	{ },
};

static struct input_handler cpu_ib_input_handler = {
	.event      = cpu_ib_input_event,
	.connect    = cpu_ib_input_connect,
	.disconnect = cpu_ib_input_disconnect,
	.name       = "cpu_ib_handler",
	.id_table   = cpu_ib_ids,
};

/* Make sure calls to this are surrounded by get/put_online_cpus() */
static void ib_boost_cpus(struct boost_policy *b)
{
	struct ib_config *ib = &b->ib;
	struct ib_pcpu *pcpu;
	uint32_t cpu;

	for_each_possible_cpu(cpu) {
		if (!(ib->cpus_to_boost & CPU_MASK(cpu)))
			continue;

		if (cpu_online(cpu))
			cpufreq_update_policy(cpu);

		pcpu = per_cpu_ptr(ib->boost_info, cpu);
		queue_delayed_work(b->wq, &pcpu->unboost_work,
				msecs_to_jiffies(ib->adj_duration_ms));
	}
}

static uint32_t get_boost_freq(struct boost_policy *b, uint32_t cpu)
{
	struct ib_config *ib = &b->ib;
	uint32_t freq;

	/*
	 * The boost frequency for a LITTLE CPU is stored at index 0 of
	 * ib->freq[]. The frequency for a big CPU is stored at index 1.
	 */
	spin_lock(&b->lock);
	freq = ib->freq[CPU_MASK(cpu) & LITTLE_CPU_MASK ? 0 : 1];
	spin_unlock(&b->lock);

	return freq;
}

static void set_boost_freq(struct boost_policy *b,
		uint32_t cpu, uint32_t freq)
{
	struct ib_config *ib = &b->ib;

	/*
	 * The boost frequency for a LITTLE CPU is stored at index 0 of
	 * ib->freq[]. The frequency for a big CPU is stored at index 1.
	 */
	spin_lock(&b->lock);
	ib->freq[CPU_MASK(cpu) & LITTLE_CPU_MASK ? 0 : 1] = freq;
	spin_unlock(&b->lock);
}

static uint32_t get_boost_state(struct boost_policy *b)
{
	uint32_t state;

	spin_lock(&b->lock);
	state = b->state;
	spin_unlock(&b->lock);

	return state;
}

static void set_boost_bit(struct boost_policy *b, uint32_t state)
{
	spin_lock(&b->lock);
	b->state |= state;
	spin_unlock(&b->lock);
}

static void clear_boost_bit(struct boost_policy *b, uint32_t state)
{
	spin_lock(&b->lock);
	b->state &= ~state;
	spin_unlock(&b->lock);
}

static void unboost_all_cpus(struct boost_policy *b)
{
	struct ib_config *ib = &b->ib;

	/* Clear wake boost bit */
	clear_boost_bit(b, WAKE_BOOST);

	/* Clear cpus_to_boost bits for all CPUs */
	ib->cpus_to_boost = 0;

	/* Immediately unboost the online CPUs */
	update_online_cpu_policy();
}

static void update_online_cpu_policy(void)
{
	uint32_t cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static bool validate_cpu_freq(struct cpufreq_frequency_table *pos,
		uint32_t *freq)
{
	struct cpufreq_frequency_table *next;

	/* Set the cursor to the first valid freq */
	cpufreq_next_valid(&pos);

	/* Requested freq is below the lowest freq, so use the lowest freq */
	if (*freq < pos->frequency) {
		*freq = pos->frequency;
		return true;
	}

	while (1) {
		/* This freq exists in the table so it's definitely valid */
		if (*freq == pos->frequency)
			break;

		next = pos + 1;

		/* We've gone past the highest freq, so use the highest freq */
		if (!cpufreq_next_valid(&next)) {
			*freq = pos->frequency;
			return true;
		}

		/* Target the next-highest freq */
		if (*freq > pos->frequency && *freq < next->frequency) {
			*freq = next->frequency;
			return true;
		}

		pos = next;
	}

	return false;
}

static ssize_t enabled_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	struct fb_policy *fb = &b->fb;
	uint32_t data;
	int ret;

	ret = kstrtou32(buf, 10, &data);
	if (ret)
		return -EINVAL;

	if (data) {
		set_boost_bit(b, DRIVER_ENABLED);
	} else {
		clear_boost_bit(b, DRIVER_ENABLED);
		/* Stop everything */
		cancel_work_sync(&fb->boost_work);
		cancel_work_sync(&ib->boost_work);
		unboost_all_cpus(b);
	}

	return size;
}

static ssize_t ib_freqs_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t freq[2];
	int ret;

	ret = sscanf(buf, "%u %u", &freq[0], &freq[1]);
	if (ret != 2)
		return -EINVAL;

	if (!freq[0] || !freq[1])
		return -EINVAL;

	/* freq[0] is assigned to LITTLE cluster, freq[1] to big cluster */
	spin_lock(&b->lock);
	ib->freq[0] = freq[0];
	ib->freq[1] = freq[1];
	spin_unlock(&b->lock);

	return size;
}

static ssize_t ib_duration_ms_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t data;
	int ret;

	ret = kstrtou32(buf, 10, &data);
	if (ret)
		return -EINVAL;

	if (!data)
		return -EINVAL;

	ib->duration_ms = data;

	return size;
}

static ssize_t enabled_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct boost_policy *b = boost_policy_g;

	return snprintf(buf, PAGE_SIZE, "%u\n",
				get_boost_state(b) & DRIVER_ENABLED);
}

static ssize_t ib_freqs_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;
	uint32_t freq[2];

	spin_lock(&b->lock);
	freq[0] = ib->freq[0];
	freq[1] = ib->freq[1];
	spin_unlock(&b->lock);

	return snprintf(buf, PAGE_SIZE, "%u %u\n", freq[0], freq[1]);
}

static ssize_t ib_duration_ms_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct boost_policy *b = boost_policy_g;
	struct ib_config *ib = &b->ib;

	return snprintf(buf, PAGE_SIZE, "%u\n", ib->duration_ms);
}

static DEVICE_ATTR(enabled, 0644,
			enabled_read, enabled_write);
static DEVICE_ATTR(ib_freqs, 0644,
			ib_freqs_read, ib_freqs_write);
static DEVICE_ATTR(ib_duration_ms, 0644,
			ib_duration_ms_read, ib_duration_ms_write);

static struct attribute *cpu_ib_attr[] = {
	&dev_attr_enabled.attr,
	&dev_attr_ib_freqs.attr,
	&dev_attr_ib_duration_ms.attr,
	NULL
};

static struct attribute_group cpu_ib_attr_group = {
	.attrs = cpu_ib_attr,
};

static int sysfs_ib_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("cpu_input_boost", kernel_kobj);
	if (!kobj) {
		pr_err("Failed to create kobject\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(kobj, &cpu_ib_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs interface\n");
		kobject_put(kobj);
	}

	return ret;
}

static struct boost_policy *alloc_boost_policy(void)
{
	struct boost_policy *b;
=======
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static struct boost_drv *alloc_boost_drv(void)
{
	struct boost_drv *b;
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return NULL;

<<<<<<< HEAD
	b->wq = alloc_workqueue("cpu_ib_wq", WQ_HIGHPRI, 0);
=======
	b->wq = alloc_workqueue("cpu_input_boost_wq", WQ_HIGHPRI, 0);
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
	if (!b->wq) {
		pr_err("Failed to allocate workqueue\n");
		goto free_b;
	}

<<<<<<< HEAD
	b->ib.boost_info = alloc_percpu(typeof(*b->ib.boost_info));
	if (!b->ib.boost_info) {
		pr_err("Failed to allocate percpu definition\n");
		goto destroy_wq;
	}

	return b;

destroy_wq:
	destroy_workqueue(b->wq);
=======
	return b;

>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
free_b:
	kfree(b);
	return NULL;
}

<<<<<<< HEAD
static int __init cpu_ib_init(void)
{
	struct boost_policy *b;
	uint32_t cpu;
	int ret;

	b = alloc_boost_policy();
	if (!b) {
		pr_err("Failed to allocate boost policy\n");
=======
static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b;
	int ret;

	b = alloc_boost_drv();
	if (!b) {
		pr_err("Failed to allocate boost_drv struct\n");
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
		return -ENOMEM;
	}

	spin_lock_init(&b->lock);
<<<<<<< HEAD

	INIT_WORK(&b->fb.boost_work, fb_boost_main);
	INIT_DELAYED_WORK(&b->fb.unboost_work, fb_unboost_main);
	INIT_WORK(&b->ib.boost_work, ib_boost_main);
	INIT_WORK(&b->ib.reboost_work, ib_reboost_main);

	for_each_possible_cpu(cpu) {
		struct ib_pcpu *pcpu = per_cpu_ptr(b->ib.boost_info, cpu);

		pcpu->cpu = cpu;
		INIT_DELAYED_WORK(&pcpu->unboost_work, ib_unboost_main);
	}

	/* Allow global boost config access */
	boost_policy_g = b;

	ret = input_register_handler(&cpu_ib_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto free_mem;
	}

	ret = sysfs_ib_init();
	if (ret)
		goto input_unregister;

	cpufreq_register_notifier(&do_cpu_boost_nb, CPUFREQ_POLICY_NOTIFIER);

	fb_register_client(&fb_notifier_callback_nb);

	return 0;

input_unregister:
	input_unregister_handler(&cpu_ib_input_handler);
free_mem:
	free_percpu(b->ib.boost_info);
	kfree(b);
	return ret;
}
late_initcall(cpu_ib_init);
=======
	INIT_WORK(&b->input_boost, input_boost_worker);
	INIT_DELAYED_WORK(&b->input_unboost, input_unboost_worker);
	INIT_WORK(&b->wake_boost, wake_boost_worker);
	INIT_DELAYED_WORK(&b->wake_unboost, wake_unboost_worker);
	b->state = SCREEN_AWAKE;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		goto free_b;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->fb_notif.notifier_call = fb_notifier_cb;
	b->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	/* Allow global boost config access for external boosts */
	boost_drv_g = b;

	return 0;

unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
free_b:
	kfree(b);
	return ret;
}
late_initcall(cpu_input_boost_init);
>>>>>>> 773be92697b1... cpu_input_boost: Introduce driver for event-based CPU boosting
