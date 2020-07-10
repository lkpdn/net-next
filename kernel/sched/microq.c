// SPDX-License-Identifier: GPL-2.0
/*
 * Micro Quanta Scheduling Class
 *
 * Micro quanta is a lightweight scheduling class designed for microsecond
 * level scheduling intervals. It provides a flexible way to share cores
 * between latency sensitive tasks and other tasks - limiting the cpu share
 * of latency sensitive tasks and maintaining low scheduling latency at the
 * same time
 *
 * See sched-microq.txt for more details.
 */

#include "sched.h"
#include "pelt.h"
#include <linux/slab.h>

/*
 * Global micro quanta period and runtime in ns.
 * Can be overridden by per task parameters.
 */
int sysctl_sched_microq_period = 40000;
int sysctl_sched_microq_runtime = 20000;

int sched_microq_timeslice = 1;

static DEFINE_PER_CPU(struct callback_head, microq_push_head);

static enum hrtimer_restart sched_microq_period_timer(struct hrtimer *timer);
static void __task_tick_microq(struct rq *rq, struct task_struct *p, int queued);
static void push_one_microq_task(struct rq *rq);
static void push_microq_tasks(struct rq *rq);

static inline u64 u_saturation_sub(u64 a, u64 b)
{
	return a > b ? a - b : 0;
}

static inline struct task_struct *microq_task_of(struct sched_microq_entity *microq_se)
{
	return container_of(microq_se, struct task_struct, microq);
}

static inline struct rq *rq_of_microq_rq(struct microq_rq *microq_rq)
{
	return container_of(microq_rq, struct rq, microq);
}

static inline struct microq_rq *microq_rq_of_se(struct sched_microq_entity *microq_se)
{
	struct task_struct *p = microq_task_of(microq_se);
	struct rq *rq = task_rq(p);

	return &rq->microq;
}

static inline int on_microq_rq(struct sched_microq_entity *microq_se)
{
	return !list_empty(&microq_se->run_list);
}

int microq_task(struct task_struct *p)
{
	return p->sched_class == &microq_sched_class;
}

/*
 * microq bandwidth can be controlled with either global or per task bandwidth
 * parameters. If none of the microq tasks on a cpu specifies the bandwidth
 * parameters, global parameters take effect.
 *
 * When there are multiple microq tasks on a run queue, the bandwidth
 * parameters of the task with shortest period takes effect. (Note runtime in
 * effect is *not* the sum of microq tasks, it is exactly the runtime requested
 * by one microq task.)
 *
 * See also sched-microq.txt
 */
static inline void get_microq_bandwidth(struct microq_rq *microq_rq, int *period, int *runtime)
{
	if (microq_rq->microq_period != MICROQ_BANDWIDTH_UNDEFINED) {
		*period = microq_rq->microq_period;
		*runtime = microq_rq->microq_runtime;
	} else {
		*period = READ_ONCE(sysctl_sched_microq_period);
		*runtime = READ_ONCE(sysctl_sched_microq_runtime);
		*period = max(MICROQ_MIN_PERIOD, *period);
		if (*runtime != MICROQ_BANDWIDTH_UNDEFINED) /* if runtime != unlimited */
			*runtime = max(MICROQ_MIN_RUNTIME, *runtime);
	}
}

static inline int microq_timer_needed(struct microq_rq *microq_rq)
{
	struct rq *rq = rq_of_microq_rq(microq_rq);
	int period, runtime;

	get_microq_bandwidth(microq_rq, &period, &runtime);
	return runtime != MICROQ_BANDWIDTH_UNDEFINED && rq->microq.microq_nr_running &&
	    rq->nr_running > rq->microq.microq_nr_running;
}

void init_microq_rq(struct microq_rq *microq_rq)
{
	INIT_LIST_HEAD(&microq_rq->tasks);

	microq_rq->microq_period = MICROQ_BANDWIDTH_UNDEFINED;
	microq_rq->microq_runtime = MICROQ_BANDWIDTH_UNDEFINED;
	microq_rq->microq_time = 0;
	microq_rq->microq_target_time = 0;
	microq_rq->microq_throttled = 0;

	hrtimer_init(&microq_rq->microq_period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	microq_rq->microq_period_timer.function = sched_microq_period_timer;
	microq_rq->quanta_start = 0;
	microq_rq->delta_exec_uncharged = 0;
	microq_rq->delta_exec_total = 0;
}

static void account_microq_bandwidth(struct rq *rq)
{
	s64 delta;
	int period;
	int runtime;
	struct microq_rq *microq_rq = &rq->microq;

	delta = sched_clock_cpu(cpu_of(rq)) - microq_rq->quanta_start;
	microq_rq->quanta_start += delta;
	delta = max(0LL, delta);

	get_microq_bandwidth(microq_rq, &period, &runtime);
	if (microq_task(rq->curr)) {
		microq_rq->microq_time += delta;
		microq_rq->delta_exec_uncharged += delta;
	}
	microq_rq->delta_exec_total += delta;
	microq_rq->microq_target_time += delta*runtime/period;
	microq_rq->microq_time = clamp(microq_rq->microq_time,
	    u_saturation_sub(microq_rq->microq_target_time, period*2),
	    microq_rq->microq_target_time + period*2);
}

static void check_microq_timer(struct rq *rq)
{
	struct microq_rq *microq_rq = &rq->microq;
	int period, runtime, expire;

	if (hrtimer_active(&microq_rq->microq_period_timer))
		return;

	get_microq_bandwidth(microq_rq, &period, &runtime);
	account_microq_bandwidth(rq);
	if (microq_rq->microq_time < microq_rq->microq_target_time) {
		microq_rq->microq_throttled = 0;
		expire = microq_rq->microq_target_time - microq_rq->microq_time;
		expire = max(runtime, expire);
	} else {
		microq_rq->microq_throttled = 1;
		expire = microq_rq->microq_time - microq_rq->microq_target_time;
		expire = max(expire, period - runtime);
	}
	microq_rq->period_count = 0;
	microq_rq->periods_to_jiffies = (jiffies_to_usecs(1) * 1000) / period;
	microq_rq->periods_to_jiffies = max(1U, microq_rq->periods_to_jiffies);

	hrtimer_start_range_ns(&microq_rq->microq_period_timer, ns_to_ktime(expire), 0,
			 HRTIMER_MODE_REL_PINNED);
}

void microq_adjust_bandwidth(struct task_struct *p)
{
	struct microq_rq *microq_rq = &task_rq(p)->microq;
	struct sched_microq_entity *microq_se;

	microq_rq->microq_period = MICROQ_BANDWIDTH_UNDEFINED;
	microq_rq->microq_runtime = MICROQ_BANDWIDTH_UNDEFINED;
	list_for_each_entry(microq_se, &microq_rq->tasks, run_list) {
		if (microq_se->sched_period >= 0 &&
		    microq_se->sched_runtime >= MICROQ_BANDWIDTH_UNDEFINED &&
		    (microq_rq->microq_period == MICROQ_BANDWIDTH_UNDEFINED ||
		    microq_se->sched_period <= microq_rq->microq_period)) {
			microq_rq->microq_period = microq_se->sched_period;
			microq_rq->microq_runtime = microq_se->sched_runtime;
		}
	}

}

static void microq_update_load_avg_ratio(struct rq *rq)
{
	struct microq_rq *microq_rq = &rq->microq;
	int period;
	int runtime;
	u64 contrib_ratio;

	/*
	 * For cpu_power accounting (SCHED_POWER_SCALE)
	 *
	 * Note micro's contribution to rt_avg is capped at elapsed_time*runtime/period for
	 * the desired load balancing behavior. We allow microq to go over its bandwidth limit
	 * when there are free cpu cycles. However if we don't cap the rt_avg contribution
	 * the cpu running microq can report cpu_power of 0, such that no cfs task can be
	 * moved to it by load balancer.
	 */
	get_microq_bandwidth(microq_rq, &period, &runtime);
	if (microq_rq->delta_exec_uncharged * period > microq_rq->delta_exec_total * runtime)
		contrib_ratio = runtime * SCHED_FIXEDPOINT_SCALE / period;

	else
		contrib_ratio = SCHED_FIXEDPOINT_SCALE;
	update_rt_rq_load_avg_ratio(rq_clock_pelt(rq), contrib_ratio, rq, 1);
}

static void update_curr_microq(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct microq_rq *microq_rq = &rq->microq;

	if (!microq_task(curr))
		return;

	account_microq_bandwidth(rq);

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, microq_rq->delta_exec_uncharged));

	curr->se.sum_exec_runtime += microq_rq->delta_exec_uncharged;
	account_group_exec_runtime(curr, microq_rq->delta_exec_uncharged);

	curr->se.exec_start = rq_clock_task(rq);
	cpuacct_charge(curr, microq_rq->delta_exec_uncharged);

	microq_update_load_avg_ratio(rq),

	microq_rq->delta_exec_uncharged = 0;
	microq_rq->delta_exec_total = 0;
}

static enum hrtimer_restart sched_microq_period_timer(struct hrtimer *timer)
{
	int period;
	int runtime;
	struct microq_rq *microq_rq;
	struct rq *rq;
	ktime_t nextslice = {0};
	u64 ns;

	microq_rq = container_of(timer, struct microq_rq, microq_period_timer);
	rq = rq_of_microq_rq(microq_rq);
	get_microq_bandwidth(microq_rq, &period, &runtime);
	nextslice = ns_to_ktime(period);

	raw_spin_lock(&rq->lock);

	account_microq_bandwidth(rq);
	if (microq_timer_needed(microq_rq)) {
		if (microq_rq->microq_throttled) {
			microq_rq->microq_throttled = 0;
			ns = u_saturation_sub(microq_rq->microq_target_time,
				microq_rq->microq_time);
			nextslice = ns_to_ktime(max(ns, (u64)runtime));
		} else {
			microq_rq->microq_throttled = 1;
			ns = u_saturation_sub(microq_rq->microq_time,
				microq_rq->microq_target_time);
			/*
			 * The exact time lower class tasks need to run to maintain the bandwidth
			 * ratio
			 */
			ns = ns*period/runtime;
			/*
			 * Don't want to make the next time slice too short for lower class tasks or
			 * microq tasks. Also don't want the time slice for lower class tasks to
			 * last too long as microq tasks are latency sensitive. The time slice is
			 * not limited in this function for microq tasks (but difference between
			 * microq_time and microq_target_time is clamped).
			 */
			ns = clamp_val(ns, u_saturation_sub(period, runtime),
				u_saturation_sub(period, runtime/2));
			nextslice = ns_to_ktime(ns);

			if (++microq_rq->period_count >= microq_rq->periods_to_jiffies) {
				microq_rq->period_count = 0;
				microq_rq->periods_to_jiffies = (jiffies_to_usecs(1) * 1000) / period;
				microq_rq->periods_to_jiffies = max(1U, microq_rq->periods_to_jiffies);
				update_rq_clock(rq);
				__task_tick_microq(rq, rq->curr, 0);
			}
		}

		resched_curr(rq);
		hrtimer_set_expires(timer,
		    ktime_add(hrtimer_cb_get_time(&microq_rq->microq_period_timer), nextslice));
		raw_spin_unlock(&rq->lock);
		return HRTIMER_RESTART;
	} else {
		microq_rq->microq_throttled = 0;
		raw_spin_unlock(&rq->lock);
		return HRTIMER_NORESTART;
	}
}

static void dequeue_microq_entity(struct sched_microq_entity *microq_se)
{
	struct microq_rq *microq_rq = microq_rq_of_se(microq_se);

	list_del_init(&microq_se->run_list);
	BUG_ON(!microq_rq->microq_nr_running);
	microq_rq->microq_nr_running--;
}

static void enqueue_microq_entity(struct sched_microq_entity *microq_se)
{
	struct microq_rq *microq_rq = microq_rq_of_se(microq_se);

	list_add_tail(&microq_se->run_list, &microq_rq->tasks);
	microq_rq->microq_nr_running++;
	microq_rq->last_push_failed = 0;
}

static void
enqueue_task_microq(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_microq_entity *microq_se = &p->microq;
	struct microq_rq *microq_rq = microq_rq_of_se(microq_se);

	/*
	 * microq keeps uncharged runtime stats internally. Need to mark the end of non mircoq task
	 * execution here so their runtime won't be counted as microq.
	 */
	account_microq_bandwidth(rq);

	enqueue_microq_entity(microq_se);
	add_nr_running(rq, 1);

	/* The parameters of the task with shortest period takes effect, see get_microq_bandwidth */
	if (microq_se->sched_period >= 0 &&
	    microq_se->sched_runtime >= MICROQ_BANDWIDTH_UNDEFINED &&
	    (microq_rq->microq_period == MICROQ_BANDWIDTH_UNDEFINED ||
	    microq_se->sched_period <= microq_rq->microq_period)) {
		microq_rq->microq_period = microq_se->sched_period;
		microq_rq->microq_runtime = microq_se->sched_runtime;
	}
}

static void dequeue_task_microq(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_microq_entity *microq_se = &p->microq;

	update_curr_microq(rq);
	dequeue_microq_entity(microq_se);
	sub_nr_running(rq, 1);

	microq_adjust_bandwidth(p);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_microq_entity(struct microq_rq *microq_rq, struct sched_microq_entity *microq_se)
{
	if (on_microq_rq(microq_se))
		list_move_tail(&microq_se->run_list, &microq_rq->tasks);
}

static void requeue_task_microq(struct rq *rq, struct task_struct *p)
{
	struct sched_microq_entity *microq_se = &p->microq;
	struct microq_rq *microq_rq;

	microq_rq = microq_rq_of_se(microq_se);
	requeue_microq_entity(microq_rq, microq_se);
}

static void yield_task_microq(struct rq *rq)
{
	requeue_task_microq(rq, rq->curr);
}

static void check_preempt_curr_microq(struct rq *rq, struct task_struct *p, int flags)
{
}

static struct task_struct *pick_next_task_microq(struct rq *rq)
{
	struct sched_microq_entity *microq_se;
	struct task_struct *p;
	struct microq_rq *microq_rq = &rq->microq;

	if (!microq_rq->microq_nr_running)
		return NULL;

	if (microq_timer_needed(microq_rq)) {
		check_microq_timer(rq);
		if (microq_rq->microq_throttled)
			return NULL;
	} else {
		microq_rq->microq_throttled = 0;
	}

	microq_se = list_entry(microq_rq->tasks.next, struct sched_microq_entity, run_list);
	BUG_ON(!microq_se);

	p = microq_task_of(microq_se);

#ifdef CONFIG_SMP
	if (rq->microq.microq_nr_running > 1 && !rq->microq.last_push_failed)
		queue_balance_callback(rq, &per_cpu(microq_push_head, rq->cpu),
		    push_one_microq_task);
#endif

	if (rq->curr->sched_class != &rt_sched_class && rq->curr->sched_class != &microq_sched_class)
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	return p;
}

static void put_prev_task_microq(struct rq *rq, struct task_struct *p)
{
	microq_update_load_avg_ratio(rq);
}

#ifdef CONFIG_SMP

static int
balance_microq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return 0;
}

static int microq_find_rq(struct task_struct *p)
{
	int pcpu = task_cpu(p);
	int cpu, tcpu;
	struct rq *rq;
	int idle_ht = -1;
	int lowprio_cpu = -1;
	int low_nmicroq_cpu = -1;
	unsigned int low_nmicroq = task_rq(p)->microq.microq_nr_running;

	if (!cpumask_test_cpu(pcpu, p->cpus_ptr))
		pcpu = cpumask_first(p->cpus_ptr);

	for (cpu = pcpu;;) {
		/* search from current cpu to avoid crowding lower numbered cpus */
		cpu = cpumask_next(cpu, p->cpus_ptr);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_first(p->cpus_ptr);
		if (cpu == pcpu)
			break;

		for_each_cpu(tcpu, topology_sibling_cpumask(cpu)) {
			if (!available_idle_cpu(tcpu))
				break;
		}
		if (tcpu >= nr_cpu_ids)
			return cpu;

		if (idle_ht == -1) {
			rq = cpu_rq(cpu);
			if (idle_cpu(cpu)) {
				idle_ht = cpu;
			} else if (lowprio_cpu == -1) {
				if (rq->nr_running == rq->cfs.h_nr_running) {
					lowprio_cpu = cpu;
				} else if (rq->microq.microq_nr_running + 1 < low_nmicroq) {
					low_nmicroq_cpu = cpu;
					low_nmicroq = rq->microq.microq_nr_running;
				}
			}
		}
	}
	if (idle_ht != -1)
		return idle_ht;
	if (lowprio_cpu != -1)
		return lowprio_cpu;
	return low_nmicroq_cpu;
}

/* Will lock the rq it finds */
static struct rq *microq_find_potential_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *t_rq = NULL;
	int cpu;

	cpu = microq_find_rq(task);

	if (cpu == -1)
		return NULL;

	t_rq = cpu_rq(cpu);

	if (double_lock_balance(rq, t_rq)) {
		/*
		 * We had to unlock the run queue. In
		 * the mean time, task could have
		 * migrated already or had its affinity changed.
		 * Also make sure that it wasn't scheduled on its rq.
		 */
		if (unlikely(task_rq(task) != rq ||
			     !cpumask_test_cpu(t_rq->cpu,
					       task->cpus_ptr) ||
			     task_running(rq, task) ||
			     !task->on_rq)) {

			double_unlock_balance(rq, t_rq);
			t_rq = NULL;
		}
	}

	return t_rq;
}

/*
 * If the current CPU has more than one microq task, see if a non
 * running task can be kicked out.
 */
static int __push_microq_task(struct rq *rq)
{
	struct microq_rq *microq_rq = &rq->microq;
	struct task_struct *next_task = NULL;
	struct sched_microq_entity *microq_se;
	struct rq *t_rq;
	int ret = 0;

	if (microq_rq->microq_nr_running <= 1 || microq_rq->last_push_failed)
		return 0;

	list_for_each_entry(microq_se, &rq->microq.tasks, run_list) {
		struct task_struct *p = microq_task_of(microq_se);

		if (!task_running(rq, p) && p->nr_cpus_allowed > 1) {
			next_task = p;
			break;
		}
	}
	if (!next_task)
		goto out;

	BUG_ON(rq->cpu != task_cpu(next_task));
	BUG_ON(!next_task->on_rq);

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_rq locks the rq if found */
	t_rq = microq_find_potential_rq(next_task, rq);

	if (t_rq) {
		deactivate_task(rq, next_task, 0);
		set_task_cpu(next_task, t_rq->cpu);
		activate_task(t_rq, next_task, 0);
		resched_curr(t_rq);
		double_unlock_balance(rq, t_rq);
		ret = 1;
	}

	put_task_struct(next_task);

out:
	if (!ret)
		microq_rq->last_push_failed = 1;
	return ret;
}

static void push_one_microq_task(struct rq *rq)
{
	__push_microq_task(rq);
}

static void push_microq_tasks(struct rq *rq)
{
	while (__push_microq_task(rq)) {}
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_microq(struct rq *rq, struct task_struct *p)
{
	if (!test_tsk_need_resched(rq->curr) &&
	    p->nr_cpus_allowed > 1 &&
	    rq->microq.microq_nr_running)
		push_microq_tasks(rq);
}

/* Assumes rq->lock is held */
static void rq_online_microq(struct rq *rq)
{
	struct microq_rq *microq_rq = &rq->microq;

	microq_rq->microq_period = MICROQ_BANDWIDTH_UNDEFINED;
	microq_rq->microq_runtime = MICROQ_BANDWIDTH_UNDEFINED;
	microq_rq->microq_time = 0;
	microq_rq->microq_target_time = 0;
	microq_rq->microq_throttled = 0;
	microq_rq->quanta_start = 0;
	microq_rq->delta_exec_uncharged = 0;
	microq_rq->delta_exec_total = 0;
}

static int select_task_rq_microq(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;

	if (p->nr_cpus_allowed == 1)
		goto out;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/* Try to push the incoming microq task if the current rq is busy */
	if (unlikely(rq->microq.microq_nr_running) && (p->nr_cpus_allowed > 1)) {
		int target = microq_find_rq(p);

		if (target != -1)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

#endif /* CONFIG_SMP */

static void switched_to_microq(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	if (p->on_rq && rq->curr != p) {
		rq->microq.last_push_failed = 0;
		if (rq->microq.microq_nr_running > 1)
			queue_balance_callback(rq, &per_cpu(microq_push_head, rq->cpu),
			    push_one_microq_task);
	}
#endif
}

static void prio_changed_microq(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void __task_tick_microq(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_microq_entity *microq_se = &p->microq;

	if (microq_timer_needed(&rq->microq))
		check_microq_timer(rq);

	update_curr_microq(rq);

	if (p->microq.time_slice--)
		return;
	p->microq.time_slice = sched_microq_timeslice;
	if (microq_se->run_list.prev != microq_se->run_list.next) {
		requeue_task_microq(rq, p);
		resched_curr(rq);
		return;
	}
}

static void task_tick_microq(struct rq *rq, struct task_struct *p, int queued)
{

	if (hrtimer_active(&rq->microq.microq_period_timer))
		return;

	__task_tick_microq(rq, p, queued);
}

static void set_next_task_microq(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);
}

static unsigned int get_rr_interval_microq(struct rq *rq, struct task_struct *task)
{
	return sched_microq_timeslice;
}

const struct sched_class microq_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_microq,
	.dequeue_task		= dequeue_task_microq,
	.yield_task		= yield_task_microq,
	.check_preempt_curr	= check_preempt_curr_microq,
	.pick_next_task		= pick_next_task_microq,
	.put_prev_task		= put_prev_task_microq,
#ifdef CONFIG_SMP
	.balance		= balance_microq,
	.select_task_rq		= select_task_rq_microq,
	.set_cpus_allowed	= set_cpus_allowed_common,
	.rq_online              = rq_online_microq,
	.task_woken		= task_woken_microq,
#endif
	.set_next_task          = set_next_task_microq,
	.task_tick		= task_tick_microq,
	.get_rr_interval	= get_rr_interval_microq,
	.prio_changed		= prio_changed_microq,
	.switched_to		= switched_to_microq,
	.update_curr		= update_curr_microq,
};

int sched_microq_proc_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);
	old_period = sysctl_sched_microq_period;
	old_runtime = sysctl_sched_microq_runtime;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		if (sysctl_sched_microq_period < MICROQ_MIN_PERIOD) {
			sysctl_sched_microq_period = old_period;
			ret = -EINVAL;
		}
		if (sysctl_sched_microq_runtime < MICROQ_MIN_RUNTIME &&
		    sysctl_sched_microq_runtime != MICROQ_BANDWIDTH_UNDEFINED) {
			sysctl_sched_microq_runtime = old_runtime;
			ret = -EINVAL;
		}
	}

	mutex_unlock(&mutex);
	return ret;
}

#ifdef CONFIG_SCHED_DEBUG
extern void print_microq_rq(struct seq_file *m, int cpu, struct microq_rq *microq_rq);

void print_microq_stats(struct seq_file *m, int cpu)
{
	rcu_read_lock();
	print_microq_rq(m, cpu, &cpu_rq(cpu)->microq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */
