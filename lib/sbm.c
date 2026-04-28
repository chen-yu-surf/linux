/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/sbm.h>

struct sbm *sbm_alloc(void)
{
	unsigned int nr = arch_sbm_leafs;
	unsigned int nbits = 1U << arch_sbm_shift;
	unsigned int nlongs = BITS_TO_LONGS(nbits);
	struct sbm_root *root = kzalloc_flex(*root, leafs, nr);
	struct sbm_leaf *leaf;
	if (!root)
		return NULL;

	root->type = st_root;

	for (int i = 0; i < nr; i++) {
		leaf = kzalloc(struct_size(leaf, bitmap, nlongs),
			       GFP_KERNEL);
		if (!leaf)
			goto fail;
		leaf->type = st_leaf;
		leaf->nbits = nbits;
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

int sbm_find_next_bit(struct sbm *sbm, int start)
{
	struct sbm_leaf *leaf = (void *)sbm;
	struct sbm_root *root = (void *)sbm;
	int nr = start >> arch_sbm_shift;
	int bit = start & arch_sbm_mask;
	unsigned int found;

	if (sbm->type == st_root) {
		do {
			leaf = root->leafs[nr];
			found = find_next_bit(leaf->bitmap, leaf->nbits, bit);
			if (found < leaf->nbits)
				return (nr << arch_sbm_shift) | found;
			bit = 0;
		} while (++nr < arch_sbm_leafs);
	} else {
		found = find_next_bit(leaf->bitmap, leaf->nbits, bit);
		if (found < leaf->nbits)
			return found;
	}
	return -1;
}
