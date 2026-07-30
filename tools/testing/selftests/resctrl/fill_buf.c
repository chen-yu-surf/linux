// SPDX-License-Identifier: GPL-2.0
/*
 * fill_buf benchmark
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "resctrl.h"

#define CL_SIZE			(64)
#define PAGE_SIZE		(4 * 1024)
#define MB			(1024 * 1024)

static void sb(void)
{
#if defined(__i386) || defined(__x86_64)
	asm volatile("sfence\n\t"
		     : : : "memory");
#endif
}

static void cl_flush(void *p)
{
#if defined(__i386) || defined(__x86_64)
	asm volatile("clflush (%0)\n\t"
		     : : "r"(p) : "memory");
#endif
}

void mem_flush(unsigned char *buf, size_t buf_size)
{
	unsigned char *cp = buf;
	size_t i = 0;

	buf_size = buf_size / CL_SIZE; /* mem size in cache lines */

	for (i = 0; i < buf_size; i++)
		cl_flush(&cp[i * CL_SIZE]);

	sb();
}

/*
 * Buffer index step advance to workaround HW prefetching interfering with
 * the measurements.
 *
 * Must be a prime to step through all indexes of the buffer.
 *
 * Some primes work better than others on some architectures (from MBA/MBM
 * result stability point of view).
 */
#define FILL_IDX_MULT	23

static int fill_one_span_read(unsigned char *buf, size_t buf_size)
{
	unsigned int size = buf_size / (CL_SIZE / 2);
	unsigned int i, idx = 0;
	unsigned char sum = 0;

	/*
	 * Read the buffer in an order that is unexpected by HW prefetching
	 * optimizations to prevent them interfering with the caching pattern.
	 *
	 * The read order is (in terms of halves of cachelines):
	 *	i * FILL_IDX_MULT % size
	 * The formula is open-coded below to avoiding modulo inside the loop
	 * as it improves MBA/MBM result stability on some architectures.
	 */
	for (i = 0; i < size; i++) {
		sum += buf[idx * (CL_SIZE / 2)];

		idx += FILL_IDX_MULT;
		while (idx >= size)
			idx -= size;
	}

	return sum;
}

void fill_cache_read(unsigned char *buf, size_t buf_size, bool once)
{
	int ret = 0;

	while (1) {
		ret = fill_one_span_read(buf, buf_size);
		if (once)
			break;
	}

	/* Consume read result so that reading memory is not optimized out. */
	*value_sink = ret;
}

static void fill_one_span_write(unsigned char *buf, size_t buf_size,
				unsigned char val)
{
	unsigned int size = buf_size / (CL_SIZE / 2);
	unsigned int i, idx = 0;

	for (i = 0; i < size; i++) {
		buf[idx * (CL_SIZE / 2)] = val;

		idx += FILL_IDX_MULT;
		while (idx >= size)
			idx -= size;
	}
}

void fill_cache_write(unsigned char *buf, size_t buf_size, bool once)
{
	unsigned char val = 1;

	while (1) {
		fill_one_span_write(buf, buf_size, val++);
		if (once)
			break;
	}
}

struct fill_thread {
	pthread_t	tid;		/* worker thread handle */
	unsigned char	*buf;		/* start of this thread's buffer slice */
	size_t		buf_size;	/* size of this thread's slice in bytes */
	int		cpu;		/* CPU this thread is pinned to */
};

static void *fill_thread_fn(void *arg)
{
	struct fill_thread *ft = arg;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(ft->cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);

	fill_cache_write(ft->buf, ft->buf_size, false);

	return NULL;
}

int fill_cache_parallel(unsigned char *buf, size_t buf_size,
			const int *cpus, int ncpus)
{
	struct fill_thread *ft;
	size_t chunk;
	int i;

	ft = calloc(ncpus, sizeof(*ft));
	if (!ft)
		return -ENOMEM;

	chunk = (buf_size / ncpus) & ~((size_t)CL_SIZE - 1);

	for (i = 0; i < ncpus; i++) {
		/* Give each thread a disjoint slice; the last takes the remainder. */
		ft[i].buf = buf + i * chunk;
		ft[i].buf_size = (i == ncpus - 1) ? buf_size - i * chunk : chunk;
		ft[i].cpu = cpus[i];
		if (pthread_create(&ft[i].tid, NULL, fill_thread_fn, &ft[i]))
			break;
	}

	while (--i >= 0)
		pthread_join(ft[i].tid, NULL);

	free(ft);
	return 0;
}

unsigned char *alloc_buffer(size_t buf_size, bool memflush)
{
	void *buf = NULL;
	uint64_t *p64;
	ssize_t s64;
	int ret;

	ret = posix_memalign(&buf, PAGE_SIZE, buf_size);
	if (ret < 0)
		return NULL;

	/* Initialize the buffer */
	p64 = buf;
	s64 = buf_size / sizeof(uint64_t);

	while (s64 > 0) {
		*p64 = (uint64_t)rand();
		p64 += (CL_SIZE / sizeof(uint64_t));
		s64 -= (CL_SIZE / sizeof(uint64_t));
	}

	/* Flush the memory before using to avoid "cache hot pages" effect */
	if (memflush)
		mem_flush(buf, buf_size);

	return buf;
}

ssize_t get_fill_buf_size(int cpu_no, const char *cache_type)
{
	unsigned long cache_total_size = 0;
	int ret;

	ret = get_cache_size(cpu_no, cache_type, &cache_total_size);
	if (ret)
		return ret;

	return cache_total_size * 4 > MINIMUM_SPAN ?
			cache_total_size * 4 : MINIMUM_SPAN;
}
