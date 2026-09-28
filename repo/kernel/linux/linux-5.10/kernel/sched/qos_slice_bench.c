// SPDX-License-Identifier: GPL-2.0
/*
 * Single-vCPU check that QoS levels change EEVDF request slices.
 *
 * Two CFS kernel threads, same nice, pinned to CPU 0:
 *   hi: in_qos = 5 (QOS_USER_INTERACTIVE, 1ms slice when the switch is on)
 *   lo: in_qos = 0 (QOS_BACKGROUND, 14ms slice when the switch is on)
 *
 * The boot argument eevdf_qos_bench runs both phases back to back and
 * prints EEVDF_QOS_BENCH lines on the console. Slice mapping itself is
 * toggled with /proc/sys/kernel/sched_qos_slice.
 *
 * in_qos is set directly so the result does not depend on qos_manager
 * or on latency_nice. The value is cleared before each thread exits.
 *
 * The same boot argument also runs two EEVDF invariant checks:
 *
 *   EEVDF_FORK_CHECK: a running kthread forks children with
 *   kernel_thread() while a spinner shares CPU 0. Each child records
 *   V - v on its first run. A child that inherited the parent's stashed
 *   deadline as vlag would start roughly that far behind V. Without the
 *   spinner the child would be alone on the queue, so V == v always.
 *
 *   EEVDF_REWEIGHT_CHECK: two spinners on CPU 0 each get their own
 *   autogroup, and the shares of both groups are flipped repeatedly.
 *   Every reweight of a group entity that is still on the tree compares
 *   |V' - V| with the rounding bound in eevdf_note_reweight_queued();
 *   keeping lag means V' == V up to that rounding.
 *
 * The fair.c hooks only record while eevdf_bench_hooks is set, which
 * the boot argument does.
 *
 *   EEVDF_AVG_CHECK: for EEVDF_AVG_CHECK_SECS after late_initcall, every
 *   CPU's cfs_rqs are rescanned under the rq lock. avg_vruntime, avg_load
 *   and every min_deadline must equal what the rb-tree entities imply.
 *   eevdf_reweight_queued counts reweights of entities still on a tree,
 *   the path that used to drift the sums.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/task.h>
#include <linux/sysctl.h>

#include "sched.h"

#define QOS_SLICE_HI 5
#define QOS_SLICE_LO 0
#define QOS_SLICE_NONE (-1)
#define QOS_SLICE_WINDOW_MS 400

extern int sysctl_sched_qos_slice;

struct qos_slice_stat {
	int qos;
	struct completion ready;
	u64 switches;
	u64 exec_delta;
	u64 gap_sum;
	u64 max_gap;
	u64 gaps;
	u64 exec_start;
	u64 switch_start;
};

int qos_slice_count_picks;
atomic64_t qos_slice_picks[7];
atomic64_t eevdf_reweight_queued;
atomic64_t eevdf_reweight_over_bound;
atomic64_t eevdf_reweight_max_dv;
bool eevdf_bench_hooks;

#define EEVDF_REWEIGHT_ROUNDS 200
#define EEVDF_FORK_TRIALS 16
#define EEVDF_FORK_MAX_LAG_NS (20 * NSEC_PER_MSEC)
#define EEVDF_FORK_MIN_STASH_NS (100 * NSEC_PER_MSEC)
#define EEVDF_AVG_CHECK_SECS 60
#define EEVDF_AVG_CHECK_PERIOD_MS 20

static int __init eevdf_qos_bench_setup(char *str)
{
	eevdf_bench_hooks = true;
	return 1;
}
__setup("eevdf_qos_bench", eevdf_qos_bench_setup);

static struct ctl_table qos_slice_sysctls[] = {
	{
		.procname	= "sched_qos_slice",
		.data		= &sysctl_sched_qos_slice,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{}
};

static u64 qos_slice_switch_count(struct task_struct *p)
{
	return p->nvcsw + p->nivcsw;
}

static int qos_slice_worker(void *data)
{
	struct qos_slice_stat *st = data;
	u64 last_in = 0;
	u64 seen;

	set_cpus_allowed_ptr(current, cpumask_of(0));
	current->qts.in_qos = st->qos;
	st->exec_start = current->se.sum_exec_runtime;
	st->switch_start = qos_slice_switch_count(current);
	seen = st->switch_start;
	complete(&st->ready);

	while (!kthread_should_stop()) {
		u64 now = ktime_get_ns();
		u64 sw = qos_slice_switch_count(current);
		u64 end;

		if (sw != seen) {
			if (last_in) {
				u64 gap = now - last_in;

				st->gaps++;
				st->gap_sum += gap;
				if (gap > st->max_gap)
					st->max_gap = gap;
			}
			last_in = now;
			seen = sw;
		}

		end = now + 200000ULL;
		while (ktime_get_ns() < end && !kthread_should_stop())
			cpu_relax();
		cond_resched();
	}

	st->switches = qos_slice_switch_count(current) - st->switch_start;
	st->exec_delta = current->se.sum_exec_runtime - st->exec_start;
	printk(KERN_ALERT
	       "EEVDF_QOS_BENCH sample qos=%d in_qos=%d se_slice_ns=%llu\n",
	       st->qos, current->qts.in_qos, current->se.slice);
	current->qts.in_qos = QOS_SLICE_NONE;
	return 0;
}

static void qos_slice_print(const char *phase, struct qos_slice_stat *st,
			    const char *name)
{
	u64 avg = 0;

	if (st->gaps)
		avg = div64_u64(st->gap_sum, st->gaps);

	printk(KERN_ALERT
	       "EEVDF_QOS_BENCH phase=%s task=%s qos=%d switches=%llu exec_ns=%llu max_gap_ns=%llu avg_gap_ns=%llu\n",
	       phase, name, st->qos, st->switches, st->exec_delta, st->max_gap, avg);
}

static int qos_slice_phase(const char *phase, int slice_on,
			   u64 *pick_ratio_x100, u64 *exec_ratio_x100)
{
	struct qos_slice_stat hi, lo;
	struct task_struct *thi, *tlo;
	int ret = 0;

	memset(&hi, 0, sizeof(hi));
	memset(&lo, 0, sizeof(lo));
	hi.qos = QOS_SLICE_HI;
	lo.qos = QOS_SLICE_LO;
	init_completion(&hi.ready);
	init_completion(&lo.ready);
	sysctl_sched_qos_slice = slice_on;

	thi = kthread_create(qos_slice_worker, &hi, "qos_hi");
	if (IS_ERR(thi))
		return PTR_ERR(thi);
	tlo = kthread_create(qos_slice_worker, &lo, "qos_lo");
	if (IS_ERR(tlo)) {
		kthread_stop(thi);
		return PTR_ERR(tlo);
	}

	/* Set in_qos before the first enqueue, so place_entity sees it. */
	thi->qts.in_qos = hi.qos;
	tlo->qts.in_qos = lo.qos;
	wake_up_process(thi);
	wake_up_process(tlo);
	wait_for_completion(&hi.ready);
	wait_for_completion(&lo.ready);
	atomic64_set(&qos_slice_picks[QOS_SLICE_HI], 0);
	atomic64_set(&qos_slice_picks[QOS_SLICE_LO], 0);
	WRITE_ONCE(qos_slice_count_picks, 1);
	msleep(QOS_SLICE_WINDOW_MS);
	WRITE_ONCE(qos_slice_count_picks, 0);
	kthread_stop(thi);
	kthread_stop(tlo);

	qos_slice_print(phase, &hi, "hi");
	qos_slice_print(phase, &lo, "lo");

	{
		u64 hi_picks = atomic64_read(&qos_slice_picks[QOS_SLICE_HI]);
		u64 lo_picks = atomic64_read(&qos_slice_picks[QOS_SLICE_LO]);

		*pick_ratio_x100 = lo_picks ? div64_u64(hi_picks * 100, lo_picks) : 0;
		printk(KERN_ALERT
		       "EEVDF_QOS_BENCH phase=%s picks hi=%llu lo=%llu\n",
		       phase, hi_picks, lo_picks);
	}
	*exec_ratio_x100 = lo.exec_delta ?
		div64_u64(hi.exec_delta * 100, lo.exec_delta) : 0;
	printk(KERN_ALERT
	       "EEVDF_QOS_BENCH phase=%s slice=%d pick_ratio_x100=%llu exec_ratio_x100=%llu\n",
	       phase, slice_on, *pick_ratio_x100, *exec_ratio_x100);
	return ret;
}

struct eevdf_fork_probe {
	struct completion done;
	s64 lag;
};

static struct eevdf_fork_probe eevdf_fork_probes[EEVDF_FORK_TRIALS];

/*
 * Runs as a kernel_thread() child and must not return: on arm64 a
 * returning fn falls through to ret_to_user.
 */
static int eevdf_fork_child(void *data)
{
	struct eevdf_fork_probe *fp = data;
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(current, &rf);
#ifdef CONFIG_FAIR_GROUP_SCHED
	fp->lag = (s64)(avg_vruntime(current->se.cfs_rq) - current->se.vruntime);
#else
	fp->lag = (s64)(avg_vruntime(&rq->cfs) - current->se.vruntime);
#endif
	task_rq_unlock(rq, current, &rf);

	complete(&fp->done);
	do_exit(0);
}

struct eevdf_spinner {
	struct completion ready;
	bool own_group;
	struct task_group *tg;
};

static int eevdf_spin(void *data)
{
	struct eevdf_spinner *sp = data;

	if (sp->own_group)
		sched_autogroup_create_attach(current);
	sp->tg = task_group(current);
	complete(&sp->ready);

	while (!kthread_should_stop()) {
		u64 end = ktime_get_ns() + 200000ULL;

		while (ktime_get_ns() < end && !kthread_should_stop())
			cpu_relax();
		cond_resched();
	}
	return 0;
}

static struct task_struct *eevdf_start_spinner(struct eevdf_spinner *sp,
					       bool own_group, const char *name)
{
	struct task_struct *t;

	init_completion(&sp->ready);
	sp->own_group = own_group;
	sp->tg = NULL;
	t = kthread_create(eevdf_spin, sp, name);
	if (IS_ERR(t))
		return t;
	kthread_bind(t, 0);
	wake_up_process(t);
	wait_for_completion(&sp->ready);
	return t;
}

static int eevdf_fork_parent(void *data)
{
	struct completion *finished = data;
	s64 max_lag = 0, min_stash = S64_MAX;
	int ran = 0, stashed = 0, i;
	struct eevdf_spinner sp;
	struct task_struct *spinner;
	const char *verdict;

	spinner = eevdf_start_spinner(&sp, false, "eevdf_fork_spin");
	if (IS_ERR(spinner)) {
		printk(KERN_ALERT "EEVDF_FORK_CHECK verdict=start_failed\n");
		complete(finished);
		return 0;
	}

	for (i = 0; i < EEVDF_FORK_TRIALS; i++) {
		struct eevdf_fork_probe *fp = &eevdf_fork_probes[i];
		u64 end = ktime_get_ns() + NSEC_PER_MSEC;
		s64 stash;
		pid_t pid;

		while (ktime_get_ns() < end)
			cpu_relax();

		init_completion(&fp->done);
		fp->lag = 0;
		stash = current->se.vlag;
		if (stash == (s64)current->se.deadline)
			stashed++;
		if (abs(stash) < min_stash)
			min_stash = abs(stash);

		pid = kernel_thread(eevdf_fork_child, fp,
				    CLONE_FS | CLONE_FILES | SIGCHLD);
		if (pid < 0)
			break;
		if (!wait_for_completion_timeout(&fp->done, 5 * HZ))
			break;

		ran++;
		if (abs(fp->lag) > max_lag)
			max_lag = abs(fp->lag);
	}
	kthread_stop(spinner);

	if (ran < EEVDF_FORK_TRIALS || max_lag > EEVDF_FORK_MAX_LAG_NS)
		verdict = "fail";
	else if (stashed < EEVDF_FORK_TRIALS ||
		 min_stash < EEVDF_FORK_MIN_STASH_NS)
		verdict = "inconclusive";
	else
		verdict = "pass";

	printk(KERN_ALERT
	       "EEVDF_FORK_CHECK trials=%d ran=%d parent_vlag_is_deadline=%d min_parent_vlag_ns=%lld max_child_lag_ns=%lld limit_ns=%llu verdict=%s\n",
	       EEVDF_FORK_TRIALS, ran, stashed, min_stash, max_lag,
	       EEVDF_FORK_MAX_LAG_NS, verdict);
	complete(finished);
	return 0;
}

static void eevdf_fork_check(void)
{
	DECLARE_COMPLETION_ONSTACK(finished);
	struct task_struct *t;

	t = kthread_create(eevdf_fork_parent, &finished, "eevdf_fork");
	if (IS_ERR(t)) {
		printk(KERN_ALERT "EEVDF_FORK_CHECK verdict=start_failed\n");
		return;
	}
	kthread_bind(t, 0);
	wake_up_process(t);
	wait_for_completion(&finished);
}

struct eevdf_avg_stat {
	u64 scans;
	u64 queues;
	u64 entities;
	u64 group_entities;
	u64 bad_avg;
	u64 bad_load;
	u64 bad_min_deadline;
};

static void eevdf_check_cfs_rq(int cpu, struct cfs_rq *cfs_rq,
			       struct eevdf_avg_stat *st)
{
	struct rb_node *node;
	s64 avg = 0;
	u64 load = 0;
	bool bad = false;

	st->queues++;
	for (node = rb_first_cached(&cfs_rq->tasks_timeline); node;
	     node = rb_next(node)) {
		struct sched_entity *se = rb_entry(node, struct sched_entity,
						   run_node);
		unsigned long w = scale_load_down(se->load.weight);
		u64 min_deadline = se->deadline;
		struct sched_entity *child;

		avg += (s64)(se->vruntime - cfs_rq->min_vruntime) * w;
		load += w;
		st->entities++;
		if (!entity_is_task(se))
			st->group_entities++;

		if (node->rb_left) {
			child = rb_entry(node->rb_left, struct sched_entity,
					 run_node);
			if ((s64)(child->min_deadline - min_deadline) < 0)
				min_deadline = child->min_deadline;
		}
		if (node->rb_right) {
			child = rb_entry(node->rb_right, struct sched_entity,
					 run_node);
			if ((s64)(child->min_deadline - min_deadline) < 0)
				min_deadline = child->min_deadline;
		}
		if (se->min_deadline != min_deadline) {
			st->bad_min_deadline++;
			bad = true;
		}
	}

	if (avg != cfs_rq->avg_vruntime) {
		st->bad_avg++;
		bad = true;
	}
	if (load != cfs_rq->avg_load) {
		st->bad_load++;
		bad = true;
	}
	if (bad && st->bad_avg + st->bad_load + st->bad_min_deadline <= 3)
		printk(KERN_ALERT
		       "EEVDF_AVG_CHECK mismatch cpu=%d nr_running=%u avg_vruntime=%lld expect=%lld avg_load=%llu expect=%llu\n",
		       cpu, cfs_rq->nr_running, cfs_rq->avg_vruntime, avg,
		       cfs_rq->avg_load, load);
}

static void eevdf_check_all_cpus(struct eevdf_avg_stat *st)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		struct rq_flags rf;
#ifdef CONFIG_FAIR_GROUP_SCHED
		struct cfs_rq *cfs_rq;
#endif

		rq_lock_irqsave(rq, &rf);
		eevdf_check_cfs_rq(cpu, &rq->cfs, st);
#ifdef CONFIG_FAIR_GROUP_SCHED
		list_for_each_entry(cfs_rq, &rq->leaf_cfs_rq_list,
				    leaf_cfs_rq_list) {
			if (cfs_rq != &rq->cfs)
				eevdf_check_cfs_rq(cpu, cfs_rq, st);
		}
#endif
		rq_unlock_irqrestore(rq, &rf);
	}
	st->scans++;
}

static void eevdf_avg_report(const char *tag, struct eevdf_avg_stat *st)
{
	bool bad = st->bad_avg || st->bad_load || st->bad_min_deadline;

	printk(KERN_ALERT
	       "EEVDF_AVG_CHECK %s scans=%llu queues=%llu entities=%llu group_entities=%llu reweight_queued=%lld bad_avg=%llu bad_load=%llu bad_min_deadline=%llu verdict=%s\n",
	       tag, st->scans, st->queues, st->entities, st->group_entities,
	       atomic64_read(&eevdf_reweight_queued), st->bad_avg, st->bad_load,
	       st->bad_min_deadline, bad ? "fail" : "pass");
}

static int eevdf_avg_check_thread(void *unused)
{
	struct eevdf_avg_stat st = {};
	unsigned long end = jiffies + EEVDF_AVG_CHECK_SECS * HZ;
	unsigned long next_report = jiffies + 15 * HZ;

	if (num_online_cpus() > 1)
		set_cpus_allowed_ptr(current, cpumask_of(1));

	while (time_before(jiffies, end)) {
		eevdf_check_all_cpus(&st);
		if (time_after_eq(jiffies, next_report)) {
			eevdf_avg_report("progress", &st);
			next_report += 15 * HZ;
		}
		msleep(EEVDF_AVG_CHECK_PERIOD_MS);
	}
	eevdf_avg_report("done", &st);
	return 0;
}

static void eevdf_reweight_check(void)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct eevdf_avg_stat st = {};
	struct eevdf_spinner a, b;
	struct task_struct *ta, *tb;
	s64 queued, over, max_dv;
	bool grouped;
	int i, err = 0;
	const char *verdict;

	ta = eevdf_start_spinner(&a, true, "eevdf_rw_a");
	if (IS_ERR(ta)) {
		printk(KERN_ALERT "EEVDF_REWEIGHT_CHECK verdict=start_failed\n");
		return;
	}
	tb = eevdf_start_spinner(&b, true, "eevdf_rw_b");
	if (IS_ERR(tb)) {
		kthread_stop(ta);
		printk(KERN_ALERT "EEVDF_REWEIGHT_CHECK verdict=start_failed\n");
		return;
	}
	grouped = a.tg != &root_task_group && b.tg != &root_task_group &&
		  a.tg != b.tg;

	atomic64_set(&eevdf_reweight_max_dv, 0);
	over = atomic64_read(&eevdf_reweight_over_bound);
	queued = atomic64_read(&eevdf_reweight_queued);
	for (i = 0; grouped && !err && i < EEVDF_REWEIGHT_ROUNDS; i++) {
		unsigned long wa = (i & 1) ? 2048 : 128;
		unsigned long wb = (i & 1) ? 128 : 2048;

		err = sched_group_set_shares(a.tg, scale_load(wa));
		if (!err)
			err = sched_group_set_shares(b.tg, scale_load(wb));
		eevdf_check_all_cpus(&st);
		usleep_range(1000, 2000);
	}
	kthread_stop(ta);
	kthread_stop(tb);

	queued = atomic64_read(&eevdf_reweight_queued) - queued;
	over = atomic64_read(&eevdf_reweight_over_bound) - over;
	max_dv = atomic64_read(&eevdf_reweight_max_dv);
	if (err || over || st.bad_avg || st.bad_load || st.bad_min_deadline)
		verdict = "fail";
	else if (!grouped || !queued)
		verdict = "inconclusive";
	else
		verdict = "pass";

	printk(KERN_ALERT
	       "EEVDF_REWEIGHT_CHECK rounds=%d grouped=%d reweight_queued=%lld over_bound=%lld max_dv_ns=%lld scans=%llu group_entities=%llu bad_avg=%llu bad_load=%llu bad_min_deadline=%llu err=%d verdict=%s\n",
	       i, grouped, queued, over, max_dv, st.scans, st.group_entities,
	       st.bad_avg, st.bad_load, st.bad_min_deadline, err, verdict);
#else
	printk(KERN_ALERT "EEVDF_REWEIGHT_CHECK verdict=skipped no_group_sched\n");
#endif
}

static int qos_slice_bench_thread(void *unused)
{
	u64 off_run = 0, off_exec = 0, on_run = 0, on_exec = 0;
	int err;
	const char *verdict = "inconclusive";

	if (num_online_cpus() > 1)
		set_cpus_allowed_ptr(current, cpumask_of(1));

	printk(KERN_ALERT
	       "EEVDF_QOS_BENCH start window_ms=%d cpu=0\n",
	       QOS_SLICE_WINDOW_MS);

	err = qos_slice_phase("slice_off", 0, &off_run, &off_exec);
	if (!err)
		err = qos_slice_phase("slice_on", 1, &on_run, &on_exec);

	if (!err &&
	    off_run >= 50 && off_run <= 200 &&
	    on_run >= 200 &&
	    off_exec >= 40 && off_exec <= 250 &&
	    on_exec >= 40 && on_exec <= 250)
		verdict = "pass";

	printk(KERN_ALERT "EEVDF_QOS_BENCH verdict=%s err=%d\n", verdict, err);
	sysctl_sched_qos_slice = 1;

	eevdf_fork_check();
	eevdf_reweight_check();
	return 0;
}

static int __init qos_slice_bench_init(void)
{
	register_sysctl("kernel", qos_slice_sysctls);

	if (!eevdf_bench_hooks)
		return 0;

	if (IS_ERR(kthread_run(qos_slice_bench_thread, NULL, "qos_bench")))
		printk(KERN_ALERT "EEVDF_QOS_BENCH verdict=start_failed err=-1\n");
	if (IS_ERR(kthread_run(eevdf_avg_check_thread, NULL, "eevdf_avg")))
		printk(KERN_ALERT "EEVDF_AVG_CHECK verdict=start_failed\n");
	return 0;
}
late_initcall(qos_slice_bench_init);
