// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the PR_SCHED_CACHE prctl() interface: the GET/CREATE/SHARE_FROM/
 * DISABLE/ENABLE/GET_ENABLED sub-operations, their argument validation and
 * the ptrace_may_access() based permission checks.
 *
 * Copyright (c) 2026 Intel Corporation.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
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
# define PR_SCHED_CACHE_GET_ENABLED	5
# define PR_SCHED_CACHE_MAX		6
#endif

#ifndef PR_SCHED_CACHE_SCOPE_THREAD
# define PR_SCHED_CACHE_SCOPE_THREAD		0
# define PR_SCHED_CACHE_SCOPE_THREAD_GROUP	1
# define PR_SCHED_CACHE_SCOPE_PROCESS_GROUP	2
#endif

#define SC_PIDTYPE_PID			PR_SCHED_CACHE_SCOPE_THREAD
#define SC_PIDTYPE_TGID			PR_SCHED_CACHE_SCOPE_THREAD_GROUP
#define SC_PIDTYPE_PGID			PR_SCHED_CACHE_SCOPE_PROCESS_GROUP

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

static int get_enabled(pid_t pid, uint64_t *enabled)
{
	return sched_cache(PR_SCHED_CACHE_GET_ENABLED, pid,
			   (unsigned long)enabled, SC_PIDTYPE_PID);
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

	/* Exactly one more result below, whichever way this goes. */
	if (sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_PID)) {
		ksft_test_result_fail("cookie changes after CREATE: CREATE: %s\n",
				      strerror(errno));
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

/*
 * A third party moves two other tasks into one group without joining it:
 * CREATE on child A, then SHARE_FROM A onto child B.
 */
static void test_share_from_third_party(void)
{
	uint64_t self = 0, a_cookie = 0, b_cookie = 0;
	pid_t a, b;
	int pfd[2];

	if (pipe(pfd)) {
		ksft_test_result_fail("SHARE_FROM third party: pipe: %s\n",
				      strerror(errno));
		return;
	}

	a = fork();
	if (a == 0) {
		char c;

		close(pfd[1]);
		_exit(read(pfd[0], &c, 1) < 0);
	}
	b = a > 0 ? fork() : -1;
	if (b == 0) {
		char c;

		close(pfd[1]);
		_exit(read(pfd[0], &c, 1) < 0);
	}
	close(pfd[0]);

	if (a < 0 || b < 0) {
		ksft_test_result_fail("SHARE_FROM third party: fork: %s\n",
				      strerror(errno));
		goto out;
	}

	if (get_cookie(0, &self) ||
	    sched_cache(PR_SCHED_CACHE_CREATE, a, 0, SC_PIDTYPE_PID) ||
	    sched_cache(PR_SCHED_CACHE_SHARE_FROM, b, a, SC_PIDTYPE_PID) ||
	    get_cookie(a, &a_cookie) || get_cookie(b, &b_cookie)) {
		ksft_test_result_fail("SHARE_FROM third party: %s\n",
				      strerror(errno));
		goto out;
	}

	if (a_cookie && a_cookie == b_cookie && a_cookie != self)
		ksft_test_result_pass("SHARE_FROM third party\n");
	else
		ksft_test_result_fail("SHARE_FROM third party: self=%#llx a=%#llx b=%#llx\n",
				      (unsigned long long)self,
				      (unsigned long long)a_cookie,
				      (unsigned long long)b_cookie);
out:
	close(pfd[1]);		/* release the children */
	if (a > 0)
		waitpid(a, NULL, 0);
	if (b > 0)
		waitpid(b, NULL, 0);
}

static int tgid_pipe[2];
static pid_t tgid_thread_tid;

static void *tgid_thread(void *arg)
{
	char c;

	tgid_thread_tid = syscall(SYS_gettid);
	if (write(tgid_pipe[1], &c, 1) != 1)
		return NULL;
	/* Wait until the main thread is done. */
	if (read(tgid_pipe[0], &c, 1) < 0)
		return NULL;
	return NULL;
}

/* CREATE with the thread group scope moves every thread of the process. */
static void test_tgid_scope(void)
{
	uint64_t before = 0, mine = 0, theirs = 0;
	pthread_t th;
	char c = 0;

	if (pipe(tgid_pipe) || pthread_create(&th, NULL, tgid_thread, NULL)) {
		ksft_test_result_fail("CREATE THREAD_GROUP: setup failed\n");
		return;
	}

	/* Wait for the thread to publish its tid. */
	if (read(tgid_pipe[0], &c, 1) != 1) {
		ksft_test_result_fail("CREATE THREAD_GROUP: sync failed\n");
		goto out;
	}

	if (get_cookie(0, &before) ||
	    sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_TGID) ||
	    get_cookie(0, &mine) || get_cookie(tgid_thread_tid, &theirs)) {
		ksft_test_result_fail("CREATE THREAD_GROUP: %s\n",
				      strerror(errno));
		goto out;
	}

	if (mine && mine != before && mine == theirs)
		ksft_test_result_pass("CREATE THREAD_GROUP moves all threads\n");
	else
		ksft_test_result_fail("CREATE THREAD_GROUP: before=%#llx main=%#llx thread=%#llx\n",
				      (unsigned long long)before,
				      (unsigned long long)mine,
				      (unsigned long long)theirs);
out:
	if (write(tgid_pipe[1], &c, 1) != 1)
		ksft_print_msg("CREATE THREAD_GROUP: could not wake thread\n");
	pthread_join(th, NULL);
	close(tgid_pipe[0]);
	close(tgid_pipe[1]);
}

/* Check that GET_ENABLED on self reads back @want. */
static void expect_enabled(const char *name, uint64_t want)
{
	uint64_t enabled = ~0ULL;

	if (get_enabled(0, &enabled))
		ksft_test_result_fail("%s: GET_ENABLED: %s\n", name,
				      strerror(errno));
	else if (enabled != want)
		ksft_test_result_fail("%s: enabled=%llu, want %llu\n", name,
				      (unsigned long long)enabled,
				      (unsigned long long)want);
	else
		ksft_test_result_pass("%s\n", name);
}

static void test_enable_disable(void)
{
	/* A newly created group starts opted out. */
	if (sched_cache(PR_SCHED_CACHE_CREATE, 0, 0, SC_PIDTYPE_PID))
		ksft_test_result_fail("GET_ENABLED after CREATE: CREATE: %s\n",
				      strerror(errno));
	else
		expect_enabled("GET_ENABLED after CREATE", 0);

	if (sched_cache(PR_SCHED_CACHE_ENABLE, 0, 0, SC_PIDTYPE_PID))
		ksft_test_result_fail("ENABLE self: %s\n", strerror(errno));
	else
		ksft_test_result_pass("ENABLE self\n");
	expect_enabled("GET_ENABLED after ENABLE", 1);

	if (sched_cache(PR_SCHED_CACHE_DISABLE, 0, 0, SC_PIDTYPE_PID))
		ksft_test_result_fail("DISABLE self: %s\n", strerror(errno));
	else
		ksft_test_result_pass("DISABLE self\n");
	expect_enabled("GET_ENABLED after DISABLE", 0);
}

static void test_get_enabled_zombie(void)
{
	uint64_t enabled;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("ENOENT: fork: %s\n", strerror(errno));
		return;
	}

	if (pid == 0)
		_exit(0);

	/*
	 * Wait for the child to exit without reaping it: exit_mm() has
	 * dropped its cache group, so the zombie has none.
	 */
	if (waitid(P_PID, pid, NULL, WEXITED | WNOWAIT)) {
		ksft_test_result_fail("ENOENT: waitid: %s\n", strerror(errno));
		waitpid(pid, NULL, 0);
		return;
	}

	expect_err("ENOENT: GET_ENABLED on task without group",
		   get_enabled(pid, &enabled), ENOENT);

	waitpid(pid, NULL, 0);
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

	expect_err("EINVAL: GET_ENABLED with non-PID pid_type",
		   sched_cache(PR_SCHED_CACHE_GET_ENABLED, 0,
			       (unsigned long)&cookie, SC_PIDTYPE_TGID), EINVAL);

	expect_err("EINVAL: GET_ENABLED with misaligned pointer",
		   sched_cache(PR_SCHED_CACHE_GET_ENABLED, 0, misaligned,
			       SC_PIDTYPE_PID), EINVAL);

	expect_err("EFAULT: GET_ENABLED with NULL pointer",
		   sched_cache(PR_SCHED_CACHE_GET_ENABLED, 0, 0, SC_PIDTYPE_PID),
		   EFAULT);

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
		if (errno == EINVAL)
			ksft_exit_skip("PR_SCHED_CACHE not supported (need CONFIG_SCHED_CACHE=y): %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("initial GET failed unexpectedly: %s\n",
				   strerror(errno));
	}

	ksft_set_plan(22);

	test_get_create();		/* 2 results */
	test_share_from();		/* 1 result  */
	test_share_from_third_party();	/* 1 result  */
	test_tgid_scope();		/* 1 result  */
	test_enable_disable();		/* 5 results */
	test_get_enabled_zombie();	/* 1 result  */
	test_einval();			/* 9 results */
	test_kthread();			/* 1 result  */
	test_eperm();			/* 1 result  */

	ksft_finished();
}
