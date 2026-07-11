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
