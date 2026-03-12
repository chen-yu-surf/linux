// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enhanced Resource Director Technology (ERDT)
 *
 * Copyright (C) 2026 Intel Corporation
 *
 */

#define pr_fmt(fmt)     "resctrl: " fmt

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/resctrl.h>
#include <linux/sizes.h>

#include <asm/apic.h>
#include <asm/cpufeatures.h>

#include "internal.h"

static LIST_HEAD(domain_info_list);

static bool __erdt_enabled;

#define ERDT_VALID_VERSION		1
#define CMRC_SUPPORTED_INDEX_FN		1
#define MMRC_SUPPORTED_INDEX_FN		1
#define UNAVAILABLE_COUNTER		BIT_ULL(63)
#define RMDD_FLAG_CPU_L3_DOMAIN		BIT(0)

#define FIXPOINT_LOW_BITS		16

/* Bitmask of valid sub-tables found in the first RMDD, used to ensure all RMDDs match. */
static u32 valid_subtbl_mask;

bool erdt_support_features(int flag)
{
	if (flag == X86_FEATURE_CQM_OCCUP_LLC)
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC);

	if (flag == X86_FEATURE_CQM_MBM_TOTAL)
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_MMRC);

	return false;
}

bool erdt_enable_mon(void)
{
	int i, max_regions;

	if (!erdt_cpu_has(X86_FEATURE_CQM_MBM_TOTAL))
		return false;

	max_regions = acpi_mrrm_max_mem_region();
	for_each_rmbm_event_id(i) {
		if (!max_regions--)
			break;

		resctrl_enable_mon_event(i, true, 0, NULL);
	}

	return true;
}

int erdt_get_max_rmid(int cpu)
{
	struct erdt_domain_info *d;
	struct list_head *pos;

	if (!__erdt_enabled)
		return 0;

	list_for_each(pos, &domain_info_list) {
		d = container_of(pos, struct erdt_domain_info, list);

		if (cpumask_test_cpu(cpu, d->cpu_mask))
			return d->max_rmid;
	}

	return -1;
}

static void __iomem *cmrc_index_function_1(struct erdt_domain_info *d,
					   struct acpi_erdt_cmrc *cmrc, int rmid)
{
	u16 clump_size, stride_size;
	void __iomem *vaddr;

	clump_size = cmrc->clump_size;
	stride_size = cmrc->clump_stride;

	/*
	 * MMIO_ADDRESS_for_RMID# = CMRC Base +
	 *   (RMID / ClumpSize) * Stride +
	 *   (RMID % ClumpSize) * 8
	 */
	vaddr = d->base[ERDT_MMIO_CMRC_BASE] +
		(rmid / clump_size) * stride_size +
		(rmid % clump_size) * 8;

	return vaddr;
}

static int erdt_read_l3_occupancy(struct erdt_domain_info *d, int rmid, u64 *val)
{
	struct acpi_erdt_cmrc *cmrc;
	void __iomem *vaddr;
	u64 l3_cmt_count;
	u32 offset;

	cmrc = d->cmrc;
	if (!cmrc)
		return -EIO;

	offset = (rmid / cmrc->clump_size) * cmrc->clump_stride +
		 (rmid % cmrc->clump_size) * 8;
	/* Overflow of cmt_reg_size * SZ_4K already validated in erdt_ioremap(). */
	if (offset + sizeof(u64) > (u32)cmrc->cmt_reg_size * SZ_4K)
		return -EINVAL;

	vaddr = cmrc_index_function_1(d, cmrc, rmid);

	l3_cmt_count = readq(vaddr);
	if (l3_cmt_count & UNAVAILABLE_COUNTER)
		return -EINVAL;

	*val = l3_cmt_count * cmrc->up_scale;

	return 0;
}

static void __iomem *mmrc_index_function_1(struct erdt_domain_info *d,
					   struct acpi_erdt_mmrc *mmrc,
					   int rmid, int region_idx)
{
	u64 blk_rmid, blk_offset;
	void __iomem *vaddr;

	/*
	 * Block_to_locate_RMID# = floor((RMID# % 32) / 8) x 4 x 4096B;
	 * Offset_within_this_Block = (floor(((RMID#/32)x8)+RMID#%8) x
	 * 8B)+(Region# x 2048B);
	 * MMIO_ADDRESS_for_RMID#_Region# =
	 * MBM Register Block Base Address + Block_to_locate_RMID# +
	 * Offset_within_this_Block;
	 */
	blk_rmid = ((rmid % 32) / 8) * 4 * 4096;
	blk_offset = ((rmid / 32) * 8 + (rmid % 8)) * 8 + region_idx * 2048;
	vaddr = d->base[ERDT_MMIO_MMRC_BASE] + blk_rmid + blk_offset;

	return vaddr;
}

static u64 apply_correction_factor(u64 val, u32 factor)
{
	if (!factor)
		return val;

	return ((val * factor) >> FIXPOINT_LOW_BITS);
}

static int erdt_read_region_mbm(struct rdt_domain_hdr *hdr,
				struct erdt_domain_info *d, int rmid,
				int eventid, u64 *val)
{
	int region_idx = RMBM_STATE_IDX(eventid);
	int corr_factor_len, corr_factor = 0;
	struct rdt_hw_l3_mon_domain *hw_dom;
	u64 mbm_rmid_count = 0, chunks = 0;
	struct acpi_erdt_mmrc *mmrc = NULL;
	struct rdt_l3_mon_domain *mon_dom;
	struct arch_mbm_state *am;
	void __iomem *vaddr;

	mmrc = d->mmrc;
	if (!mmrc)
		return -EIO;

	if (!domain_header_is_valid(hdr, RESCTRL_MON_DOMAIN, RDT_RESOURCE_L3))
		return -EIO;

	mon_dom = container_of(hdr, struct rdt_l3_mon_domain, hdr);
	hw_dom = resctrl_to_arch_mon_dom(mon_dom);

	vaddr = mmrc_index_function_1(d, mmrc, rmid, region_idx);
	mbm_rmid_count = readq(vaddr);

	/* verify if the data is valid */
	if (mbm_rmid_count & UNAVAILABLE_COUNTER)
		return -EINVAL;

	corr_factor_len = mmrc->corr_factor_list_len;
	if (corr_factor_len) {
		/*
		 * 0: Do not apply a correction factor to
		 *    the MBM values.
		 * 1: Apply a single correction factor.
		 * Max RMID+1: Apply the indicated indexed
		 * correction factor to the corresponding
		 * RMID value for MBM counter.
		 */
		if (corr_factor_len == 1)
			corr_factor = mmrc->corr_factor_list[0];
		else
			corr_factor = mmrc->corr_factor_list[rmid];
	}

	am = get_arch_mbm_state(hw_dom, rmid, eventid);
	if (am) {
		am->chunks += mbm_overflow_count(am->prev_mon_val, mbm_rmid_count,
						mmrc->counter_width);

		chunks = apply_correction_factor(am->chunks, corr_factor);
		am->prev_mon_val = mbm_rmid_count;
	}

	*val = chunks * mmrc->up_scale;

	return 0;
}

int erdt_mon_read(struct rdt_domain_hdr *hdr, int ev_id, int rmid, u64 *val)
{
	struct rdt_hw_l3_mon_domain *hw_dom;
	struct erdt_domain_info *d;

	hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));
	d = hw_dom->d_info;
	if (!d)
		return -EIO;

	if (ev_id == QOS_L3_OCCUP_EVENT_ID)
		return erdt_read_l3_occupancy(d, rmid, val);

	if (rmbm_event(ev_id))
		return erdt_read_region_mbm(hdr, d, rmid, ev_id, val);

	return -EIO;
}

static void __iomem *erdt_ioremap(phys_addr_t base, u32 num_pages, const char *desc)
{
	void __iomem *addr;
	size_t size;

	if (check_mul_overflow((size_t)num_pages, (size_t)SZ_4K, &size))
		return NULL;

	addr = ioremap(base, size);
	if (!addr) {
		pr_err("ERDT: Failed to map %s at phys addr %pa (size: %u pages)\n",
		       desc, &base, num_pages);
	}
	return addr;
}

static void erdt_iounmap_domain(struct erdt_domain_info *domain)
{
	for (int i = 0; i < ERDT_MMIO_NUM_TYPES; i++) {
		if (domain->base[i]) {
			iounmap(domain->base[i]);
			domain->base[i] = NULL;
		}
	}
}

static void cleanup_one_domain(struct erdt_domain_info *d)
{
	erdt_iounmap_domain(d);
	free_cpumask_var(d->cpu_mask);
	kfree(d->cmrc);
	kfree(d->mmrc);
	kfree(d);
}

/*
 * Save CACD information for this RMDD:
 * convert the X2APIC to CPU and save them in a mask.
 */
static __init int cacd_init(struct acpi_subtbl_hdr_16 *subtbl,
			    struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cacd *cacd = (struct acpi_erdt_cacd *)subtbl;
	int num_ids, cpu;

	if (cacd->header.length < struct_size(cacd, X2APICIDS, 1)) {
		pr_warn(FW_BUG "Invalid x2apicid CACD table\n");
		return -EIO;
	}

	num_ids = (cacd->header.length - sizeof(*cacd)) / sizeof(cacd->X2APICIDS[0]);

	for (int i = 0; i < num_ids; i++) {
		cpu = topo_lookup_cpuid(cacd->X2APICIDS[i]);
		if (cpu < 0) {
			pr_warn(FW_BUG "Unknown x2apicid 0x%x\n", cacd->X2APICIDS[i]);
			return -EIO;
		}

		cpumask_set_cpu(cpu, domain_info->cpu_mask);
	}

	return 0;
}

static __init int cmrc_init(struct acpi_subtbl_hdr_16 *subtbl,
			    struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cmrc *cmrc = (struct acpi_erdt_cmrc *)subtbl;

	if (subtbl->length < sizeof(*cmrc)) {
		pr_warn(FW_BUG "Truncated CMRC subtable\n");
		return -EIO;
	}

	if (cmrc->index_fn != CMRC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported CMRC index function %d\n", cmrc->index_fn);
		return -EIO;
	}

	if (!cmrc->clump_size) {
		pr_warn(FW_BUG "CMRC clump_size is zero\n");
		return -EIO;
	}

	domain_info->base[ERDT_MMIO_CMRC_BASE] =
		erdt_ioremap(cmrc->cmt_reg_base, cmrc->cmt_reg_size, "CMRC base");
	if (!domain_info->base[ERDT_MMIO_CMRC_BASE])
		return -EIO;

	domain_info->cmrc = kmemdup(cmrc, subtbl->length, GFP_KERNEL);
	if (!domain_info->cmrc) {
		iounmap(domain_info->base[ERDT_MMIO_CMRC_BASE]);
		domain_info->base[ERDT_MMIO_CMRC_BASE] = NULL;
		return -ENOMEM;
	}

	return 0;
}

static __init int mmrc_init(struct acpi_subtbl_hdr_16 *subtbl,
			    struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_mmrc *mmrc = (struct acpi_erdt_mmrc *)subtbl;

	if (subtbl->length < sizeof(*mmrc)) {
		pr_warn(FW_BUG "Truncated MMRC subtable\n");
		return -EIO;
	}

	if (mmrc->index_fn != MMRC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported MMRC index function %d\n", mmrc->index_fn);
		return -EIO;
	}

	domain_info->base[ERDT_MMIO_MMRC_BASE] =
		erdt_ioremap(mmrc->reg_base, mmrc->reg_size, "MMRC base");
	if (!domain_info->base[ERDT_MMIO_MMRC_BASE])
		return -EIO;

	domain_info->mmrc = kmemdup(mmrc, subtbl->length, GFP_KERNEL);
	if (!domain_info->mmrc) {
		iounmap(domain_info->base[ERDT_MMIO_MMRC_BASE]);
		domain_info->base[ERDT_MMIO_MMRC_BASE] = NULL;
		return -ENOMEM;
	}

	return 0;
}

static inline struct acpi_subtbl_hdr_16 *rmdd_subtbl(struct acpi_erdt_rmdd *rmdd)
{
	return (void *)rmdd + sizeof(*rmdd);
}

static inline struct acpi_subtbl_hdr_16 *next_subtbl(struct acpi_subtbl_hdr_16 *subtbl)
{
	return (void *)subtbl + subtbl->length;
}

static inline bool subtbl_valid(void *end, struct acpi_subtbl_hdr_16 *subtbl)
{
	/* Ensure the header is within bounds before dereferencing it. */
	if ((void *)subtbl + sizeof(*subtbl) > end)
		return false;

	/* A sub-table must be at least as large as its header. */
	if (subtbl->length < sizeof(*subtbl))
		return false;

	/* The entire sub-table (including body) must fit within the parent. */
	if ((void *)subtbl + subtbl->length > end)
		return false;

	return true;
}

static __init bool parse_rmdd_entry(struct acpi_subtbl_hdr_16 *rmdd_hdr)
{
	struct erdt_domain_info *domain_info;
	struct acpi_subtbl_hdr_16 *subtbl;
	struct acpi_erdt_rmdd *rmdd;
	u32 subtbl_mask = 0;

	if (rmdd_hdr->length < sizeof(*rmdd)) {
		pr_warn(FW_BUG "Invalid RMDD length %u\n", rmdd_hdr->length);
		return false;
	}

	rmdd = (struct acpi_erdt_rmdd *)rmdd_hdr;

	/* Quietly ignore non-CPU-based L3 domains */
	if (!(rmdd->flags & RMDD_FLAG_CPU_L3_DOMAIN))
		return true;

	domain_info = kzalloc_obj(*domain_info, GFP_KERNEL);
	if (!domain_info)
		return false;

	if (!zalloc_cpumask_var(&domain_info->cpu_mask, GFP_KERNEL))
		goto cleanup;

	domain_info->base[ERDT_MMIO_RMDD_CREG] =
		erdt_ioremap(rmdd->creg_base, rmdd->creg_size, "RMDD ctrl base");
	if (!domain_info->base[ERDT_MMIO_RMDD_CREG])
		goto cleanup;

	for (subtbl = rmdd_subtbl(rmdd);
	     subtbl_valid((void *)rmdd + rmdd->header.length, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		switch (subtbl->type) {
		case ACPI_ERDT_TYPE_CACD:
			if (cacd_init(subtbl, domain_info))
				goto cleanup;

			subtbl_mask |= BIT(ACPI_ERDT_TYPE_CACD);
			break;
		case ACPI_ERDT_TYPE_CMRC:
			/* TBD: Only 1 CMRR per domain is allowed? */
			if (!(subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC)) &&
			    !cmrc_init(subtbl, domain_info))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_CMRC);

			break;
		case ACPI_ERDT_TYPE_MMRC:
			if (!(subtbl_mask & BIT(ACPI_ERDT_TYPE_MMRC)) &&
			    !mmrc_init(subtbl, domain_info))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_MMRC);

			break;
		default:
			break;
		}
	}

	if (!subtbl_mask)
		goto cleanup;

	/*
	 * Require all RMDDs to support same set of sub-tables
	 */
	if (!valid_subtbl_mask) {
		valid_subtbl_mask = subtbl_mask;
	} else if (subtbl_mask != valid_subtbl_mask) {
		pr_warn(FW_BUG "RMDD sub-table set does not match the first RMDD\n");
		goto cleanup;
	}

	if (!rmdd->max_rmid || rmdd->max_rmid > INT_MAX) {
		pr_warn(FW_BUG "Unreasonable RMDD max_rmid %u\n", rmdd->max_rmid);
		goto cleanup;
	}
	domain_info->max_rmid = rmdd->max_rmid;

	list_add(&domain_info->list, &domain_info_list);

	return true;

cleanup:
	cleanup_one_domain(domain_info);
	return false;
}

/*
 * Associate ERDT table information with this domain.
 */
int erdt_l3_mon_domain_setup(int cpu, struct rdt_domain_hdr *hdr)
{
	struct rdt_hw_l3_mon_domain *hw_dom;
	struct erdt_domain_info *d;
	struct list_head *pos;

	if (!__erdt_enabled)
		return 0;

	/*
	 * Find the erdt_domain_info that contains this CPU,
	 * compare erdt_domain_info's cpumask with the cpumask
	 * exposed by hw_dom (derived from CPUID leaf 4).
	 * If yes, assign the erdt_domain_info in the hw_dom,
	 * otherwise this CPU should be isolated from resctrl.
	 * For example, the hw_dom reports CPU{0,1} are in
	 * l3 domain0, CPU{2,3} belongs to domain1. Meanwhile
	 * erdt_domain_info reports that CPU{0,2} are in domain0,
	 * CPU{1,3} are in domain1. So when it comes to CPU1,
	 * a mismatch is detected, we should remove CPU1 from
	 * resctrl.
	 */
	list_for_each(pos, &domain_info_list) {
		d = container_of(pos, struct erdt_domain_info, list);

		if (cpumask_test_cpu(cpu, d->cpu_mask)) {
			if (!cpumask_subset(&hdr->cpu_mask, d->cpu_mask)) {
				pr_warn(FW_BUG "Mismatch detected, CPU%d in L3 domain(%*pbl) and CACD domain(%*pbl)\n",
					cpu, cpumask_pr_args(&hdr->cpu_mask), cpumask_pr_args(d->cpu_mask));

				return -EIO;
			}

			hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));
			/* No mismatch, assign the ERDT information to hw_dom */
			if (!hw_dom->d_info)
				hw_dom->d_info = d;

			return 0;
		}
	}

	pr_warn(FW_BUG "Cannot find CACD domain for CPU%d\n", cpu);
	return -ENOENT;
}

void erdt_exit(void)
{
	struct erdt_domain_info *d;
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, &domain_info_list) {
		d = container_of(pos, struct erdt_domain_info, list);
		list_del(pos);
		cleanup_one_domain(d);
	}
	__erdt_enabled = false;
	valid_subtbl_mask = 0;
}

static __init int enumerate_erdt_table(struct acpi_table_header *table_hdr)
{
	struct acpi_table_erdt *erdt = (struct acpi_table_erdt *)table_hdr;
	struct acpi_subtbl_hdr_16 *subtbl;
	void *table_end;

	if (erdt->header.revision != ERDT_VALID_VERSION) {
		pr_info("Unsupported ERDT table revision %d\n", erdt->header.revision);
		return -EINVAL;
	}

	if (erdt->header.length < sizeof(*erdt)) {
		pr_warn(FW_BUG "ERDT: Invalid table length %u bytes\n", erdt->header.length);
		return -EINVAL;
	}

	subtbl = (void *)erdt + sizeof(struct acpi_table_erdt);
	table_end = (void *)erdt + erdt->header.length;

	while (subtbl_valid(table_end, subtbl)) {
		if (subtbl->type == ACPI_ERDT_TYPE_RMDD &&
		    !parse_rmdd_entry(subtbl))
			goto cleanup;

		subtbl = next_subtbl(subtbl);
	}

	if (list_empty(&domain_info_list))
		goto cleanup;

	__erdt_enabled = true;

	return 0;

cleanup:
	erdt_exit();
	return -EINVAL;
}

int __init erdt_init(void)
{
	return acpi_table_parse(ACPI_SIG_ERDT, enumerate_erdt_table);
}
