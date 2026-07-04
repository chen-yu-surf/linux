// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

static struct sched_cache_group *sched_cache_alloc_group(void)
{
	struct sched_cache_group *grp;
	struct sched_cache_time __percpu *pcpu_sched;

	pcpu_sched = alloc_percpu(struct sched_cache_time);
	if (!pcpu_sched)
		return NULL;

	grp = kzalloc(sizeof(*grp), GFP_KERNEL);
	if (!grp) {
		free_percpu(pcpu_sched);
		return NULL;
	}

	sched_cache_group_init(grp, pcpu_sched);
	return grp;
}

static void __sched_cache_set(struct task_struct *p,
			      struct sched_cache_group *grp)
{
	struct sched_cache_group *old_grp;

	if (grp)
		sched_cache_group_get(grp);

	old_grp = p->sched_cache_grp;
	WRITE_ONCE(p->sched_cache_grp, grp);
	if (old_grp)
		sched_cache_group_put(old_grp);
}

/*
 * arg2: subcommand, arg3: target pid (0 = self),
 * arg4: cookie out ptr for GET (else 0)
 */
int sched_cache_prctl(int option, unsigned long arg2, unsigned long arg3,
		      unsigned long arg4)
{
	struct sched_cache_group *grp = NULL;
	struct task_struct *task;
	int err = 0;

	if (arg2 >= PR_SCHED_CACHE_MAX || (long)arg3 < 0)
		return -EINVAL;

	if (arg2 != PR_SCHED_CACHE_GET && arg4)
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

	switch (arg2) {
	case PR_SCHED_CACHE_DISABLE:
		scoped_guard(rcu) {
			/*
			 * protect against kfree_rcu() in sched_cache_group_put()
			 */
			grp = READ_ONCE(task->sched_cache_grp);
			if (!grp)
				err = -ENOENT;
			else
				WRITE_ONCE(grp->disabled, 1);
		}
		break;

	case PR_SCHED_CACHE_ENABLE:
		scoped_guard(rcu) {
			grp = READ_ONCE(task->sched_cache_grp);
			if (!grp)
				err = -ENOENT;
			else
				WRITE_ONCE(grp->disabled, 0);
		}
		break;

	case PR_SCHED_CACHE_GET: {
		unsigned long id = 0;

		/* must be 8 byes aligned */
		if (arg4 & 7) {
			err = -EINVAL;
			break;
		}
		scoped_guard(rcu) {
			grp = READ_ONCE(task->sched_cache_grp);
			if (grp)
				ptr_to_hashval((void *)grp, &id);
		}

		if (arg4) {
			err = put_user((u64)id, (u64 __user *)arg4);
			if (err)
				break;
		}
		break;
	}

	case PR_SCHED_CACHE_CREATE:
		grp = sched_cache_alloc_group();
		if (!grp) {
			err = -ENOMEM;
			break;
		}
		__sched_cache_set(task, grp);
		sched_cache_group_put(grp);
		break;

	case PR_SCHED_CACHE_SHARE_TO:
		__sched_cache_set(task, current->sched_cache_grp);
		break;

	case PR_SCHED_CACHE_SHARE_FROM:
		__sched_cache_set(current, task->sched_cache_grp);
		break;

	default:
		err = -EINVAL;
		break;
	}

	put_task_struct(task);
	return err;
}
