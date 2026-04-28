/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/sbm.h>

struct sbm *sbm_alloc(void)
{
	unsigned int nr = arch_sbm_leafs;
	struct sbm_root *root = kzalloc_flex(*root, leafs, nr);
	struct sbm_leaf *leaf;
	if (!root)
		return NULL;

	root->type = st_root;

	for (int i = 0; i < nr; i++) {
		leaf = kzalloc_obj(*leaf);
		if (!leaf)
			goto fail;
		leaf->type = st_leaf;
		root->leafs[i] = leaf;
	}

	if (nr == 1) {
		leaf = root->leafs[0];
		kfree(root);
		return (void *)leaf;
	}

	return (void *)root;

fail:
	for (int i = 0; i < nr; i++)
		kfree(root->leafs[i]);
	kfree(root);
	return NULL;
}

unsigned int sbm_find_next_bit(struct sbm *sbm, int start)
{
	struct sbm_leaf *leaf = (void *)sbm;
	struct sbm_root *root = (void *)sbm;
	int nr = start >> arch_sbm_shift;
	int bit = start & arch_sbm_mask;
	unsigned long tmp, mask = (~0UL) << bit;
	if (sbm->type == st_root) {
		for (; nr < arch_sbm_leafs; nr++, mask = ~0UL) {
			leaf = root->leafs[nr];
			tmp = leaf->bitmap & mask;
			if (!tmp)
				continue;
		}
	} else {
		tmp = leaf->bitmap & mask;
	}
	if (!tmp)
		return -1;
	return (nr << arch_sbm_shift) | __ffs(tmp);
}
