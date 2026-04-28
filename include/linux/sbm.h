/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SBM_H
#define _LINUX_SBM_H

#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/cpumask.h>
#include <asm/sbm.h>

extern unsigned int arch_sbm_leafs;
extern unsigned int arch_sbm_shift;
extern unsigned int arch_sbm_mask;
extern unsigned int arch_sbm_bits;
extern unsigned int arch_sbm_max_apicid;

extern unsigned int arch_sbm_cpu_to_idx(unsigned int cpu);
extern unsigned int arch_sbm_idx_to_cpu(unsigned int idx);

enum sbm_type {
	st_root = 0,
	st_leaf,
};

struct sbm_root {
	enum sbm_type	type;
	unsigned int	nr;
	struct sbm_leaf *leafs[] __counted_by(nr);
};

struct sbm_leaf {
	enum sbm_type	type;
	unsigned int	nbits;
	unsigned long	bitmap[];
} ____cacheline_aligned;

struct sbm {
	enum sbm_type	type;
};

extern struct sbm *sbm_alloc(void);
extern int sbm_find_next_bit(struct sbm *sbm, int start);

#define __sbm_op(sbm, func)				\
({							\
	struct sbm_leaf *leaf = (void *)sbm;		\
	int idx = arch_sbm_cpu_to_idx(cpu);		\
	if (sbm->type == st_root) {			\
		struct sbm_root *root = (void *)sbm;	\
		int nr = idx >> arch_sbm_shift;		\
		leaf = root->leafs[nr];			\
	}						\
	int bit = idx & arch_sbm_mask;			\
	func(bit, leaf->bitmap);			\
})

static inline void sbm_cpu_set(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, set_bit);
}

static inline void sbm_cpu_clear(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, clear_bit);
}

static inline void __sbm_cpu_set(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, __set_bit);
}

static inline void __sbm_cpu_clear(struct sbm *sbm, int cpu)
{
	__sbm_op(sbm, __clear_bit);
}

static inline bool sbm_cpu_test(struct sbm *sbm, int cpu)
{
	return __sbm_op(sbm, test_bit);
}

#define sbm_for_each_set_bit(sbm, idx) \
	for (int idx = sbm_find_next_bit(sbm, 0); \
	     idx >= 0; idx = sbm_find_next_bit(sbm, idx+1))

#endif /* _LINUX_SBM_H */
