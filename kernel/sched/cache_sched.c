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
