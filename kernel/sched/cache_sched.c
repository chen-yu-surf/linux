// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

struct sched_cache_group *sched_cache_group_get(struct sched_cache_group *grp)
{
	/*
	 * refcount_inc_not_zero() is the acquire primitive for lockless
	 * (RCU) lookups; plain refcount_inc() would scribble the count if
	 * it already reached zero. Return NULL in that case.
	 */
	if (grp && !refcount_inc_not_zero(&grp->refcnt))
		grp = NULL;

	return grp;
}

void sched_cache_group_put(struct sched_cache_group *grp)
{
	if (!grp || !refcount_dec_and_test(&grp->refcnt))
		return;

	free_percpu(grp->pcpu_sched);
	kfree_rcu(grp, rcu);
}

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

static void __sched_cache_set(struct task_struct *p,
			      struct sched_cache_group *grp)
{
	struct sched_cache_group *old_grp;
	unsigned long flags;

	if (grp)
		sched_cache_group_get(grp);

	/*
	 * p->pi_lock serializes the exchange against concurrent writers
	 * (other prctl callers as well as exec_mmap()/exit_mm()). Without
	 * it two writers could fetch the same old pointer and each drop a
	 * reference, over-decrementing the refcount. The put() may sleep
	 * (free_percpu()), so it happens after the lock is released.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	old_grp = rcu_dereference_protected(p->sched_cache_grp,
					    lockdep_is_held(&p->pi_lock));
	rcu_assign_pointer(p->sched_cache_grp, grp);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	if (old_grp)
		sched_cache_group_put(old_grp);
}

static struct sched_cache_group *sched_cache_clone_group(struct task_struct *p)
{
	/*
	 * RCU keeps the group alive here; refcount_inc_not_zero() in
	 * sched_cache_group_get() handles a concurrent last put. No
	 * pi_lock needed.
	 */
	guard(rcu)();
	return sched_cache_group_get(rcu_dereference(p->sched_cache_grp));
}

/*
 * arg2: subcommand, arg3: target pid (0 = self),
 * arg4: cookie out ptr for GET (else 0)
 * arg5: pid type
 */
int sched_cache_prctl(int option, unsigned long arg2, unsigned long arg3,
		      unsigned long arg4, unsigned long arg5)
{
	struct sched_cache_group *grp = NULL;
	struct task_struct *task, *p;
	enum pid_type type = arg5;
	struct pid *pid_grp;
	int err = 0;

	if (arg2 >= PR_SCHED_CACHE_MAX || (long)arg3 < 0)
		return -EINVAL;

	if (arg2 != PR_SCHED_CACHE_GET && arg4)
		return -EINVAL;

	if (arg5 > PIDTYPE_PGID)
		return -EINVAL;

	scoped_guard(rcu) {
		if (arg3 == 0) {
			task = current;
		} else {
			task = find_task_by_vpid(arg3);
			if (!task)
				return -ESRCH;
		}
		get_task_struct(task);
	}

	if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
		err = -EPERM;
		goto out;
	}

	switch (arg2) {
	case PR_SCHED_CACHE_GET: {
		unsigned long id = 0;

		/* GET only reports a single thread's cookie */
		if (type != PIDTYPE_PID) {
			err = -EINVAL;
			goto out;
		}

		/* must be 8 byes aligned */
		if (arg4 & 7) {
			err = -EINVAL;
			goto out;
		}
		grp = sched_cache_clone_group(task);
		if (grp)
			ptr_to_hashval((void *)grp, &id);

		if (arg4)
			err = put_user((u64)id, (u64 __user *)arg4);

		/* GET never modifies the task, so it never falls through. */
		goto out;
	}

	case PR_SCHED_CACHE_CREATE:
		/*
		 * Allocator owns ref 1, __sched_cache_set() acquires ref 2.
		 * The sched_cache_group_put() at out: drops ref 1, leaving
		 * ref 1 held by the task.
		 */
		grp = sched_cache_alloc_all();
		if (!grp) {
			err = -ENOMEM;
			goto out;
		}
		break;

	case PR_SCHED_CACHE_SHARE_TO:
		/* share the group of current to task */
		grp = sched_cache_clone_group(current);
		break;

	case PR_SCHED_CACHE_SHARE_FROM:
		if (type != PIDTYPE_PID) {
			/*
			 * It is safe to goto out and
			 * invoke sched_cache_group_put(grp),
			 * because grp is NULl and will be skipped
			 * by sched_cache_group_put(grp).
			 */
			err = -EINVAL;
			goto out;
		}

		/* set the group of current from task */
		grp = sched_cache_clone_group(task);
		__sched_cache_set(current, grp);
		goto out;

	default:
		err = -EINVAL;
		goto out;
	}

	if (type == PIDTYPE_PID) {
		__sched_cache_set(task, grp);
		goto out;
	}

	read_lock(&tasklist_lock);
	pid_grp = task_pid_type(task, type);

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

out:
	sched_cache_group_put(grp);
	put_task_struct(task);
	return err;
}
