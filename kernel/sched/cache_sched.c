// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

static void sched_cache_group_init(struct sched_cache_group *grp,
				   struct sched_cache_time __percpu *_pcpu_sched)
{
	unsigned long epoch = 0;
	int i;

	for_each_possible_cpu(i) {
		struct sched_cache_time *pcpu_sched = per_cpu_ptr(_pcpu_sched, i);
		struct rq *rq = cpu_rq(i);

		pcpu_sched->runtime = 0;
		/* a slightly stale cpu epoch is acceptible */
		pcpu_sched->epoch = rq->cpu_epoch;
		epoch = rq->cpu_epoch;
	}

	raw_spin_lock_init(&grp->lock);
	grp->epoch = epoch;
	grp->cpu = -1;
	grp->next_scan = jiffies;
	grp->nr_running_avg = 0;
	grp->footprint = 0;
	refcount_set(&grp->refcnt, 1);
	/*
	 * The update to grp->pcpu_sched should not be reordered
	 * before initialization to grp's other fields, in case
	 * the readers may get invalid mm_sched_epoch, etc.
	 */
	smp_store_release(&grp->pcpu_sched, _pcpu_sched);
}

struct sched_cache_group *
sched_cache_alloc_group(struct sched_cache_time __percpu *_pcpu_sched)
{
	struct sched_cache_group *grp;

	grp = kzalloc_obj(*grp);
	if (!grp) {
		free_percpu(_pcpu_sched);
		return NULL;
	}

	sched_cache_group_init(grp, _pcpu_sched);
	return grp;
}

static struct sched_cache_group *sched_cache_alloc_all(void)
{
	struct sched_cache_time __percpu *pcpu_sched;

	pcpu_sched = alloc_percpu(struct sched_cache_time);
	if (!pcpu_sched)
		return NULL;

	return sched_cache_alloc_group(pcpu_sched);
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * When prctl() moves a task between groups, move its footprint estimate too:
 * otherwise the group it leaves over-estimates its footprint forever, and the
 * group it joins under-estimates it once the task exits.
 */
static void sched_cache_xfer_footprint(struct task_struct *p,
				       struct sched_cache_group *old,
				       struct sched_cache_group *new)
{
	unsigned long fp, sub;

	if (!old || !new || old == new)
		return;

	sub = READ_ONCE(p->total_numa_faults);
	if (!sub)
		return;

	fp = READ_ONCE(old->footprint);
	sub = min(fp, sub);
	WRITE_ONCE(old->footprint, fp - sub);

	fp = READ_ONCE(new->footprint);
	WRITE_ONCE(new->footprint, fp + sub);
}
#else
static inline void sched_cache_xfer_footprint(struct task_struct *p,
					      struct sched_cache_group *old,
					      struct sched_cache_group *new)
{
}
#endif /* CONFIG_NUMA_BALANCING */

static void __sched_cache_set(struct task_struct *p,
			      struct sched_cache_group *grp)
{
	struct sched_cache_group *old_grp;
	unsigned long flags;

	grp = sched_cache_group_get(grp);

	/*
	 * p->pi_lock serializes the exchange against concurrent writers
	 * (other prctl callers as well as exec_mmap()/exit_mm()). Without
	 * it two writers could fetch the same old pointer and each drop a
	 * reference, over-decrementing the refcount.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);

	/*
	 * Avoid increasing the refcount for an exiting task,
	 * otherwise the grp can not be put after the task exits,
	 * and cause memory leak:
	 * CPU0 (prctl)                    CPU1 (task exiting)
	 * ----                            ----
	 *                                 exit_mm()
	 *                                   put(p->sched_cache_grp)
	 *                                    free(old_grp)
	 *
	 * __sched_cache_set(p, new_grp)
	 *
	 *                                   free_task()
	 *                                     free(p)
	 * the "freed" p is unable to put new_grp
	 *
	 * PF_EXITING is set in exit_signals() without p->pi_lock held, so this
	 * test on its own would only narrow the window. What closes it is that
	 * exit_mm() takes p->pi_lock to clear the pointer, and exit_signals()
	 * runs before exit_mm(). So of the two possible orders:
	 *
	 *  - we get pi_lock before exit_mm() does: we install new_grp and its
	 *    reference, and exit_mm() subsequently drops it, because it puts
	 *    whatever sched_cache_replace_grp() hands back.
	 *
	 *  - we get pi_lock after exit_mm() released it: the lock's release/
	 *    acquire ordering makes the PF_EXITING store visible to us, so the
	 *    test below fires and we install nothing.
	 *
	 * Either way the reference is dropped exactly once. The test is thus
	 * about not leaving a reference behind on a task that is already past
	 * exit_mm(), not about racing with the PF_EXITING store itself.
	 */
	if (p->flags & PF_EXITING) {
		raw_spin_unlock_irqrestore(&p->pi_lock, flags);
		if (grp)
			sched_cache_group_put(grp);
		return;
	}

	old_grp = sched_cache_replace_grp(p, grp);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	/* Carry this task's footprint estimate to the group it just joined. */
	sched_cache_xfer_footprint(p, old_grp, grp);

	if (old_grp)
		sched_cache_group_put(old_grp);
}

static struct task_struct *sched_cache_find_get_task(unsigned long vpid)
{
	struct task_struct *p;

	guard(rcu)();
	p = vpid ? find_task_by_vpid(vpid) : current;
	if (p)
		get_task_struct(p);

	return p;
}

/*
 * arg2: subcommand,
 * arg3: destination pid, SHARE_FROM copies the group of arg4 to arg3
 * arg4: source pid, or cookie out ptr for GET
 * arg5: pid type, the scope of the destination
 *
 */
int sched_cache_prctl(int option, unsigned long arg2, unsigned long arg3,
		      unsigned long arg4, unsigned long arg5)
{
	struct task_struct *dst = NULL, *src = NULL, *p;
	struct sched_cache_group *grp = NULL;
	enum pid_type type = arg5;
	struct pid *pid_grp;
	int err = 0;

	if (arg2 >= PR_SCHED_CACHE_MAX || arg3 > INT_MAX ||
	    arg5 > PIDTYPE_PGID)
		return -EINVAL;

	/* only GET and SHARE_FROM take a 4th argument */
	if (arg4 && arg2 != PR_SCHED_CACHE_GET &&
	    arg2 != PR_SCHED_CACHE_SHARE_FROM)
		return -EINVAL;

	dst = sched_cache_find_get_task(arg3);
	if (!dst)
		return -ESRCH;

	if (dst->flags & PF_KTHREAD) {
		err = -EINVAL;
		goto out_task;
	}

	if (!ptrace_may_access(dst, PTRACE_MODE_READ_REALCREDS)) {
		err = -EPERM;
		goto out_task;
	}

	switch (arg2) {
	case PR_SCHED_CACHE_GET: {
		unsigned long id = 0;

		if (type != PIDTYPE_PID || arg4 & 7) {
			err = -EINVAL;
			goto out_task;
		}

		grp = task_cache_group_get(dst);
		if (grp)
			ptr_to_hashval((void *)grp, &id);

		if (arg4)
			err = put_user((u64)id, (u64 __user *)arg4);

		goto out_group;
	}

	case PR_SCHED_CACHE_CREATE:
		/*
		 * Allocator owns ref 1, __sched_cache_set() acquires ref 2.
		 * The sched_cache_group_put() at out_group: drops ref 1, leaving
		 * ref 1 held by the task.
		 */
		grp = sched_cache_alloc_all();
		if (!grp) {
			err = -ENOMEM;
			goto out_task;
		}
		break;

	case PR_SCHED_CACHE_SHARE_FROM:
		if (arg4 > INT_MAX) {
			err = -EINVAL;
			goto out_task;
		}

		src = sched_cache_find_get_task(arg4);
		if (!src) {
			err = -ESRCH;
			goto out_task;
		}

		if (src->flags & PF_KTHREAD) {
			err = -EINVAL;
			goto out_task;
		}

		if (!ptrace_may_access(src, PTRACE_MODE_READ_REALCREDS)) {
			err = -EPERM;
			goto out_task;
		}

		/* copy the group of src to dst */
		grp = task_cache_group_get(src);
		if (!grp) {
			err = -ENOENT;
			goto out_task;
		}
		break;

	default:
		err = -EINVAL;
		goto out_task;
	}

	if (type == PIDTYPE_PID) {
		__sched_cache_set(dst, grp);
		goto out_group;
	}

	read_lock(&tasklist_lock);
	pid_grp = task_pid_type(dst, type);

	do_each_pid_thread(pid_grp, type, p) {
		if (!ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS)) {
			err = -EPERM;
			goto out_tasklist;
		}
	} while_each_pid_thread(pid_grp, type, p);

	do_each_pid_thread(pid_grp, type, p) {
		__sched_cache_set(p, grp);
	} while_each_pid_thread(pid_grp, type, p);
out_tasklist:
	read_unlock(&tasklist_lock);

out_group:
	sched_cache_group_put(grp);
out_task:
	if (src)
		put_task_struct(src);
	if (dst)
		put_task_struct(dst);
	return err;
}
