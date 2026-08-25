// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the PR_SCHED_CACHE prctl() interface: the GET/CREATE/SHARE_FROM/
 * DISABLE/ENABLE sub-operations, their argument validation and the
 * ptrace_may_access() based permission checks.
 *
 * Copyright (c) 2026 Intel Corporation.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../kselftest.h"

#ifndef PR_SCHED_CACHE
#define PR_SCHED_CACHE			82
# define PR_SCHED_CACHE_GET		0
# define PR_SCHED_CACHE_CREATE		1
# define PR_SCHED_CACHE_SHARE_FROM	2
# define PR_SCHED_CACHE_DISABLE		3
# define PR_SCHED_CACHE_ENABLE		4
# define PR_SCHED_CACHE_MAX		5
#endif

/* Mirror the kernel's enum pid_type ordering for the pid_type argument. */
#define SC_PIDTYPE_PID			0
#define SC_PIDTYPE_TGID			1
#define SC_PIDTYPE_PGID			2

static int sched_cache(int subop, pid_t pid, unsigned long arg4, int pid_type)
{
	return prctl(PR_SCHED_CACHE, (unsigned long)subop, (unsigned long)pid,
		     arg4, (unsigned long)pid_type);
}

static int get_cookie(pid_t pid, uint64_t *cookie)
{
	return sched_cache(PR_SCHED_CACHE_GET, pid, (unsigned long)cookie,
			   SC_PIDTYPE_PID);
}

/* Expect a call to fail with a specific errno. */
static void expect_err(const char *name, int ret, int want_errno)
{
	if (ret == 0) {
		ksft_test_result_fail("%s: unexpectedly succeeded\n", name);
		return;
	}
	if (errno != want_errno) {
		ksft_test_result_fail("%s: got errno %d (%s), want %d (%s)\n",
				      name, errno, strerror(errno),
				      want_errno, strerror(want_errno));
		return;
	}
	ksft_test_result_pass("%s\n", name);
}

static void test_get_create(void)
{
	uint64_t before = 0, after = 0;

	if (get_cookie(0, &before)) {
		ksft_test_result_fail("GET self: %s\n", strerror(errno));
		ksft_test_result_skip("cookie changes after CREATE (GET failed)\n");
		return;
	}
	ksft_test_result_pass("GET self\n");

	if (sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_PID)) {
		ksft_test_result_fail("CREATE self: %s\n", strerror(errno));
		ksft_test_result_skip("cookie changes after CREATE (CREATE failed)\n");
		return;
	}

	if (get_cookie(0, &after)) {
		ksft_test_result_fail("cookie changes after CREATE: GET: %s\n",
				      strerror(errno));
		return;
	}

	/*
	 * CREATE installs a brand new group, so the obfuscated cookie must
	 * change and be non-zero.
	 */
	if (after == 0 || after == before)
		ksft_test_result_fail("cookie changes after CREATE: before=%#llx after=%#llx\n",
				      (unsigned long long)before,
				      (unsigned long long)after);
	else
		ksft_test_result_pass("cookie changes after CREATE\n");
}

static void test_share_from(void)
{
	uint64_t parent_cookie = 0, child_cookie = 0;
	int pfd[2];
	pid_t pid;

	if (sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_PID) ||
	    get_cookie(0, &parent_cookie)) {
		ksft_test_result_fail("SHARE_FROM: parent CREATE/GET: %s\n",
				      strerror(errno));
		return;
	}

	if (pipe(pfd)) {
		ksft_test_result_fail("SHARE_FROM: pipe: %s\n", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("SHARE_FROM: fork: %s\n", strerror(errno));
		return;
	}

	if (pid == 0) {			/* child */
		uint64_t c = 0;

		close(pfd[0]);
		/* Pull the parent's group onto ourselves. */
		if (sched_cache(PR_SCHED_CACHE_SHARE_FROM, 0, getppid(),
				SC_PIDTYPE_PID) || get_cookie(0, &c))
			c = 0;
		if (write(pfd[1], &c, sizeof(c)) != sizeof(c))
			_exit(1);
		_exit(0);
	}

	close(pfd[1]);
	if (read(pfd[0], &child_cookie, sizeof(child_cookie)) != sizeof(child_cookie))
		child_cookie = 0;
	close(pfd[0]);
	waitpid(pid, NULL, 0);

	if (child_cookie != 0 && child_cookie == parent_cookie)
		ksft_test_result_pass("SHARE_FROM: child joined parent group\n");
	else
		ksft_test_result_fail("SHARE_FROM: parent=%#llx child=%#llx\n",
				      (unsigned long long)parent_cookie,
				      (unsigned long long)child_cookie);
}

static void test_enable_disable(void)
{
	if (sched_cache(PR_SCHED_CACHE_DISABLE, 0, 0, SC_PIDTYPE_PID))
		ksft_test_result_fail("DISABLE self: %s\n", strerror(errno));
	else
		ksft_test_result_pass("DISABLE self\n");

	if (sched_cache(PR_SCHED_CACHE_ENABLE, 0, 0, SC_PIDTYPE_PID))
		ksft_test_result_fail("ENABLE self: %s\n", strerror(errno));
	else
		ksft_test_result_pass("ENABLE self\n");
}

static void test_einval(void)
{
	char buf[32];
	unsigned long aligned = ((unsigned long)buf + 7) & ~7UL;
	unsigned long misaligned = aligned + 1;	/* inside buf, not 8-aligned */
	uint64_t cookie;

	expect_err("EINVAL: subop >= MAX",
		   sched_cache(PR_SCHED_CACHE_MAX, 0, 0, SC_PIDTYPE_PID), EINVAL);

	expect_err("EINVAL: bad pid_type",
		   sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_PGID + 1),
		   EINVAL);

	expect_err("EINVAL: arg4 given to CREATE",
		   sched_cache(PR_SCHED_CACHE_CREATE, 0, 1, SC_PIDTYPE_PID),
		   EINVAL);

	expect_err("EINVAL: GET with non-PID pid_type",
		   sched_cache(PR_SCHED_CACHE_GET, 0, (unsigned long)&cookie,
			       SC_PIDTYPE_TGID), EINVAL);

	expect_err("EINVAL: GET with misaligned pointer",
		   sched_cache(PR_SCHED_CACHE_GET, 0, misaligned, SC_PIDTYPE_PID),
		   EINVAL);

	/* INT_MAX is above the default pid_max, so no such task exists. */
	expect_err("ESRCH: nonexistent pid",
		   get_cookie(2147483647, &cookie), ESRCH);
}

static void test_kthread(void)
{
	char comm[64] = "";
	FILE *f;

	/* pid 2 is kthreadd on a normal boot; verify before relying on it. */
	f = fopen("/proc/2/comm", "r");
	if (!f || !fgets(comm, sizeof(comm), f) ||
	    strncmp(comm, "kthreadd", 8) != 0) {
		if (f)
			fclose(f);
		ksft_test_result_skip("EINVAL on kernel thread (no kthreadd at pid 2)\n");
		return;
	}
	fclose(f);

	expect_err("EINVAL: target is a kernel thread",
		   sched_cache(PR_SCHED_CACHE_CREATE, 2, 0, SC_PIDTYPE_PID),
		   EINVAL);
}

static void test_eperm(void)
{
	struct passwd *nobody;
	pid_t pid;
	int status;

	if (geteuid() != 0) {
		ksft_test_result_skip("EPERM on unowned task (need root to drop privs)\n");
		return;
	}

	nobody = getpwnam("nobody");
	if (!nobody) {
		ksft_test_result_skip("EPERM on unowned task (no 'nobody' user)\n");
		return;
	}

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("EPERM: fork: %s\n", strerror(errno));
		return;
	}

	if (pid == 0) {			/* child drops to nobody and pokes init */
		if (setgid(nobody->pw_gid) || setuid(nobody->pw_uid))
			_exit(2);
		/* Should not be allowed to touch pid 1. */
		if (sched_cache(PR_SCHED_CACHE_CREATE, 1, 0, SC_PIDTYPE_PID) == 0)
			_exit(3);
		_exit(errno == EPERM ? 0 : 4);
	}

	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
		ksft_test_result_fail("EPERM: child did not exit cleanly\n");
		return;
	}

	switch (WEXITSTATUS(status)) {
	case 0:
		ksft_test_result_pass("EPERM on unowned task\n");
		break;
	case 2:
		ksft_test_result_skip("EPERM on unowned task (could not drop privs)\n");
		break;
	case 3:
		ksft_test_result_fail("EPERM: prctl unexpectedly succeeded\n");
		break;
	default:
		ksft_test_result_fail("EPERM: unexpected errno from child\n");
		break;
	}
}

int main(void)
{
	uint64_t probe;

	ksft_print_header();

	/*
	 * Probe support: a valid GET on ourselves must succeed when
	 * CONFIG_SCHED_CACHE is enabled; without it prctl() rejects the
	 * option with EINVAL.
	 */
	if (get_cookie(0, &probe)) {
		if (errno == EINVAL || errno == ENOSYS)
			ksft_exit_skip("PR_SCHED_CACHE not supported (need CONFIG_SCHED_CACHE=y): %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("initial GET failed unexpectedly: %s\n",
				   strerror(errno));
	}

	ksft_set_plan(12);

	test_get_create();	/* 2 results */
	test_share_from();	/* 1 result  */
	test_enable_disable();	/* 2 results */
	test_einval();		/* 6 results */
	test_kthread();		/* 1 result  */
	test_eperm();		/* 1 result  */

	ksft_finished();
}
