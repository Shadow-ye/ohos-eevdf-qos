// SPDX-License-Identifier: GPL-2.0
/*
 * Scheduler comparison that builds on both the stock 5.10 CFS and the
 * EEVDF port, so the same boot measures either kernel.
 *
 * Boot with "sched_cmp_bench isolcpus=3". Every measured thread is bound
 * to CPU 3, which boot-time userspace is kept off.
 *
 *   lat_*: a periodic thread sleeps to an absolute hrtimer every 10ms and
 *   then busy-works 0.5ms next to N nice-0 busy hogs. The latency is the
 *   time from timer expiry to the thread running again. The _qos cases
 *   set in_qos 5 on the periodic thread and 0 on the hogs: stock CFS
 *   ignores in_qos, the EEVDF port turns it into 1ms and 14ms slices.
 *   nice stays 0 so only the slice differs.
 *
 *   fair_*: two hogs at different nice for CMP_FAIR_MS. The CPU time
 *   ratio is compared with the weight ratio.
 *
 *   pingpong: two threads hand completions back and forth. Under TCG only
 *   the ratio between kernels means anything, not the absolute cost.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "sched.h"

#ifdef ENQUEUE_INITIAL
#define CMP_KERNEL "eevdf"
#else
#define CMP_KERNEL "cfs"
#endif

#define CMP_CPU 3
#define CMP_MAX_HOGS 4
#define CMP_LAT_PERIOD_NS (10 * NSEC_PER_MSEC)
#define CMP_LAT_WORK_NS (500 * NSEC_PER_USEC)
#define CMP_LAT_SAMPLES 300
#define CMP_WARMUP_MS 20
#define CMP_FAIR_MS 2000
#define CMP_PINGPONG_ROUNDS 20000
#define CMP_PINGPONG_REPEATS 3
#define CMP_QOS_HI 5
#define CMP_QOS_LO 0
#define CMP_NO_QOS (-1)

static bool sched_cmp_enabled;

static int __init sched_cmp_setup(char *str)
{
	sched_cmp_enabled = true;
	return 1;
}
__setup("sched_cmp_bench", sched_cmp_setup);

static void cmp_busy(u64 ns)
{
	u64 end = ktime_get_ns() + ns;

	while (ktime_get_ns() < end)
		cpu_relax();
}

static struct task_struct *cmp_start(int (*fn)(void *), void *data,
				     const char *name, int nice, int qos)
{
	struct task_struct *t;

	t = kthread_create(fn, data, "%s", name);
	if (IS_ERR(t))
		return t;
	kthread_bind(t, CMP_CPU);
	set_user_nice(t, nice);
	/* Set before the first enqueue, so place_entity() sees it. */
	t->qts.in_qos = qos;
	wake_up_process(t);
	return t;
}

struct cmp_hog {
	struct task_struct *t;
	u64 exec;
	u64 invol;
};

static int cmp_hog_fn(void *unused)
{
	while (!kthread_should_stop())
		cmp_busy(200 * NSEC_PER_USEC);
	current->qts.in_qos = CMP_NO_QOS;
	return 0;
}

static int cmp_start_hogs(struct cmp_hog *hogs, int n, const int *nice,
			  int qos)
{
	int i;

	for (i = 0; i < n; i++) {
		hogs[i].t = cmp_start(cmp_hog_fn, NULL, "cmp_hog",
				      nice ? nice[i] : 0, qos);
		if (IS_ERR(hogs[i].t)) {
			while (--i >= 0)
				kthread_stop(hogs[i].t);
			return -ENOMEM;
		}
		get_task_struct(hogs[i].t);
	}
	return 0;
}

static void cmp_snap_hogs(struct cmp_hog *hogs, int n, bool end)
{
	int i;

	for (i = 0; i < n; i++) {
		u64 exec = READ_ONCE(hogs[i].t->se.sum_exec_runtime);
		u64 invol = READ_ONCE(hogs[i].t->nivcsw);

		hogs[i].exec = end ? exec - hogs[i].exec : exec;
		hogs[i].invol = end ? invol - hogs[i].invol : invol;
	}
}

static void cmp_stop_hogs(struct cmp_hog *hogs, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		kthread_stop(hogs[i].t);
		put_task_struct(hogs[i].t);
	}
}

struct cmp_lat {
	struct completion done;
	u64 *samples;
	int n;
};

static int cmp_lat_fn(void *data)
{
	struct cmp_lat *l = data;
	ktime_t next = ktime_add_ns(ktime_get(), CMP_LAT_PERIOD_NS);

	while (l->n < CMP_LAT_SAMPLES) {
		s64 lat;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_hrtimeout_range(&next, 0, HRTIMER_MODE_ABS);
		lat = ktime_to_ns(ktime_sub(ktime_get(), next));
		l->samples[l->n++] = lat > 0 ? lat : 0;
		cmp_busy(CMP_LAT_WORK_NS);
		next = ktime_add_ns(next, CMP_LAT_PERIOD_NS);
		if (ktime_before(next, ktime_get()))
			next = ktime_add_ns(ktime_get(), CMP_LAT_PERIOD_NS);
	}
	current->qts.in_qos = CMP_NO_QOS;
	complete(&l->done);
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

static void cmp_lat_case(const char *name, int nr_hogs, bool qos)
{
	struct cmp_hog hogs[CMP_MAX_HOGS];
	struct task_struct *t;
	struct cmp_lat l;
	u64 start, elapsed, sum = 0, invol = 0;
	int i, n;

	l.samples = kcalloc(CMP_LAT_SAMPLES, sizeof(u64), GFP_KERNEL);
	if (!l.samples)
		goto fail;
	l.n = 0;
	init_completion(&l.done);

	if (cmp_start_hogs(hogs, nr_hogs, NULL, qos ? CMP_QOS_LO : CMP_NO_QOS))
		goto fail_free;
	msleep(CMP_WARMUP_MS);
	cmp_snap_hogs(hogs, nr_hogs, false);
	start = ktime_get_ns();

	t = cmp_start(cmp_lat_fn, &l, "cmp_lat", 0,
		      qos ? CMP_QOS_HI : CMP_NO_QOS);
	if (IS_ERR(t)) {
		cmp_stop_hogs(hogs, nr_hogs);
		goto fail_free;
	}
	wait_for_completion(&l.done);
	elapsed = ktime_get_ns() - start;
	cmp_snap_hogs(hogs, nr_hogs, true);
	cmp_stop_hogs(hogs, nr_hogs);

	n = l.n;
	sort(l.samples, n, sizeof(u64), cmp_u64, NULL);
	for (i = 0; i < n; i++)
		sum += l.samples[i];
	for (i = 0; i < nr_hogs; i++)
		invol += hogs[i].invol;

	printk(KERN_ALERT
	       "SCHED_CMP kernel=%s case=%s hogs=%d samples=%d mean_ns=%llu p50_ns=%llu p90_ns=%llu p99_ns=%llu max_ns=%llu hog_preempt_per_s=%llu\n",
	       CMP_KERNEL, name, nr_hogs, n, div64_u64(sum, n),
	       l.samples[n / 2], l.samples[n * 9 / 10], l.samples[n * 99 / 100],
	       l.samples[n - 1],
	       div64_u64(invol * NSEC_PER_SEC, elapsed ? elapsed : 1));
	kfree(l.samples);
	return;

fail_free:
	kfree(l.samples);
fail:
	printk(KERN_ALERT "SCHED_CMP kernel=%s case=%s verdict=start_failed\n",
	       CMP_KERNEL, name);
}

static void cmp_fair_case(const char *name, int nice_a, int nice_b)
{
	struct cmp_hog hogs[2];
	int nice[2] = { nice_a, nice_b };
	u64 ratio, theory, err;

	if (cmp_start_hogs(hogs, 2, nice, CMP_NO_QOS)) {
		printk(KERN_ALERT
		       "SCHED_CMP kernel=%s case=%s verdict=start_failed\n",
		       CMP_KERNEL, name);
		return;
	}
	msleep(CMP_WARMUP_MS);
	cmp_snap_hogs(hogs, 2, false);
	msleep(CMP_FAIR_MS);
	cmp_snap_hogs(hogs, 2, true);
	cmp_stop_hogs(hogs, 2);

	ratio = div64_u64(hogs[0].exec * 1000, hogs[1].exec ? hogs[1].exec : 1);
	theory = div64_u64((u64)sched_prio_to_weight[nice_a + 20] * 1000,
			   sched_prio_to_weight[nice_b + 20]);
	err = div64_u64((ratio > theory ? ratio - theory : theory - ratio) * 1000,
			theory);
	printk(KERN_ALERT
	       "SCHED_CMP kernel=%s case=%s exec_a_ms=%llu exec_b_ms=%llu ratio_x1000=%llu theory_x1000=%llu err_permille=%llu\n",
	       CMP_KERNEL, name, div64_u64(hogs[0].exec, NSEC_PER_MSEC),
	       div64_u64(hogs[1].exec, NSEC_PER_MSEC), ratio, theory, err);
}

struct cmp_pp {
	struct completion ping, pong, done;
	u64 ns;
};

static int cmp_pp_peer(void *data)
{
	struct cmp_pp *pp = data;
	int i;

	for (i = 0; i < CMP_PINGPONG_ROUNDS; i++) {
		wait_for_completion(&pp->ping);
		complete(&pp->pong);
	}
	return 0;
}

static int cmp_pp_driver(void *data)
{
	struct cmp_pp *pp = data;
	u64 start = ktime_get_ns();
	int i;

	for (i = 0; i < CMP_PINGPONG_ROUNDS; i++) {
		complete(&pp->ping);
		wait_for_completion(&pp->pong);
	}
	pp->ns = ktime_get_ns() - start;
	complete(&pp->done);
	return 0;
}

static void cmp_pingpong_case(void)
{
	u64 round_ns[CMP_PINGPONG_REPEATS];
	struct cmp_pp pp;
	int r;

	for (r = 0; r < CMP_PINGPONG_REPEATS; r++) {
		init_completion(&pp.ping);
		init_completion(&pp.pong);
		init_completion(&pp.done);
		pp.ns = 0;
		if (IS_ERR(cmp_start(cmp_pp_peer, &pp, "cmp_pp_peer", 0,
				     CMP_NO_QOS)) ||
		    IS_ERR(cmp_start(cmp_pp_driver, &pp, "cmp_pp_drv", 0,
				     CMP_NO_QOS))) {
			printk(KERN_ALERT
			       "SCHED_CMP kernel=%s case=pingpong verdict=start_failed\n",
			       CMP_KERNEL);
			return;
		}
		wait_for_completion(&pp.done);
		/* Let the peer finish its last complete() before pp is reused. */
		msleep(CMP_WARMUP_MS);
		round_ns[r] = div64_u64(pp.ns, CMP_PINGPONG_ROUNDS);
	}
	sort(round_ns, CMP_PINGPONG_REPEATS, sizeof(u64), cmp_u64, NULL);
	printk(KERN_ALERT
	       "SCHED_CMP kernel=%s case=pingpong rounds=%d min_round_ns=%llu median_round_ns=%llu max_round_ns=%llu\n",
	       CMP_KERNEL, CMP_PINGPONG_ROUNDS, round_ns[0],
	       round_ns[CMP_PINGPONG_REPEATS / 2],
	       round_ns[CMP_PINGPONG_REPEATS - 1]);
}

static int sched_cmp_thread(void *unused)
{
	printk(KERN_ALERT "SCHED_CMP kernel=%s start cpu=%d\n", CMP_KERNEL,
	       CMP_CPU);

	cmp_lat_case("lat_h0", 0, false);
	cmp_lat_case("lat_h1", 1, false);
	cmp_lat_case("lat_h4", 4, false);
	cmp_lat_case("lat_h1_qos", 1, true);
	cmp_lat_case("lat_h4_qos", 4, true);
	cmp_fair_case("fair_n0_n5", 0, 5);
	cmp_fair_case("fair_n0_n10", 0, 10);
	cmp_pingpong_case();

	printk(KERN_ALERT "SCHED_CMP kernel=%s done\n", CMP_KERNEL);
	return 0;
}

static int __init sched_cmp_init(void)
{
	if (!sched_cmp_enabled)
		return 0;
	if (num_online_cpus() <= CMP_CPU) {
		printk(KERN_ALERT "SCHED_CMP kernel=%s verdict=skipped cpus=%u\n",
		       CMP_KERNEL, num_online_cpus());
		return 0;
	}
	if (IS_ERR(kthread_run(sched_cmp_thread, NULL, "sched_cmp")))
		printk(KERN_ALERT "SCHED_CMP kernel=%s verdict=start_failed\n",
		       CMP_KERNEL);
	return 0;
}
late_initcall(sched_cmp_init);
