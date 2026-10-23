// SPDX-License-Identifier: GPL-2.0-only

#include "sched.h"

static struct task_struct *sched_cache_find_get_task(unsigned long vpid)
{
	struct task_struct *p;

	guard(rcu)();
	p = vpid ? find_task_by_vpid(vpid) : current;
	if (p)
		get_task_struct(p);

	return p;
}

enum sched_cache_info {
	SCHED_CACHE_INFO_ID,
	SCHED_CACHE_INFO_ENABLED,
};

static int sched_cache_get_info(struct task_struct *dst, unsigned long arg4,
				enum pid_type type, enum sched_cache_info info)
{
	u64 __user *uptr = (u64 __user *)arg4;
	struct sched_cache_group *grp;
	unsigned long value = 0;

	if (type != PIDTYPE_PID || arg4 & 7)
		return -EINVAL;

	grp = task_cache_group_get(dst);
	if (!grp) {
		if (info == SCHED_CACHE_INFO_ENABLED)
			return -ENOENT;
		return put_user(0ULL, uptr);
	}

	switch (info) {
	case SCHED_CACHE_INFO_ID:
		ptr_to_hashval((void *)grp, &value);
		break;
	case SCHED_CACHE_INFO_ENABLED:
		value = READ_ONCE(grp->enabled);
		break;
	}

	sched_cache_group_put(grp);
	return put_user((u64)value, uptr);
}

/*
 * arg2: subcommand,
 * arg3: destination pid, SHARE_FROM copies the group of arg4 to arg3
 * arg4: source pid, or output ptr for GET / GET_ENABLED
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

	/* only GET, GET_ENABLED and SHARE_FROM take a 4th argument */
	if (arg4 && arg2 != PR_SCHED_CACHE_GET &&
	    arg2 != PR_SCHED_CACHE_GET_ENABLED &&
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
	case PR_SCHED_CACHE_DISABLE:
	case PR_SCHED_CACHE_ENABLE:
		/*
		 * Setting a single task is OK, because the sched_cache_group is
		 * shared by multiple tasks, setting one equals to setting all.
		 * So only PIDTYPE_PID is accepted.
		 */
		if (type != PIDTYPE_PID) {
			err = -EINVAL;
			goto out_task;
		}

		grp = task_cache_group_get(dst);
		if (!grp) {
			err = -ENOENT;
			goto out_task;
		}
		WRITE_ONCE(grp->enabled, arg2 == PR_SCHED_CACHE_ENABLE);

		/* Drop the stale preferred LLC so load balancing stops honoring it. */
		if (arg2 == PR_SCHED_CACHE_DISABLE)
			WRITE_ONCE(grp->cpu, -1);

		goto out_group;

	case PR_SCHED_CACHE_GET_ENABLED:
		err = sched_cache_get_info(dst, arg4, type,
					   SCHED_CACHE_INFO_ENABLED);
		goto out_task;

	case PR_SCHED_CACHE_GET:
		err = sched_cache_get_info(dst, arg4, type, SCHED_CACHE_INFO_ID);
		goto out_task;

	case PR_SCHED_CACHE_CREATE:
		/*
		 * Allocator owns ref 1, after the "break", sched_cache_group_get()
		 * acquires ref 2. At last sched_cache_group_put() at out_group drops
		 * ref 1, leaving ref 1 held by the task.
		 */
		grp = sched_cache_alloc_group();
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
		sched_cache_set_grp(dst, sched_cache_group_get(grp), true);
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
		sched_cache_set_grp(p, sched_cache_group_get(grp), true);
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
