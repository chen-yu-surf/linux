// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

void sched_cache_group_put(struct sched_cache_group *grp)
{
	if (!grp || !refcount_dec_and_test(&grp->refcnt))
		return;

	free_percpu(grp->pcpu_sched);
	kfree_rcu(grp, rcu);
}
