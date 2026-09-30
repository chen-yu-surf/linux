// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enhanced Resource Director Technology (ERDT)
 *
 * Copyright (C) 2026 Intel Corporation
 *
 */

#define pr_fmt(fmt)     "resctrl: ERDT: " fmt

#include <linux/acpi.h>
#include <linux/overflow.h>
#include <linux/resctrl.h>
#include <linux/sizes.h>

#include <asm/apic.h>
#include <asm/resctrl.h>

#include "internal.h"

static LIST_HEAD(domain_info_list);

/* True when the ERDT ACPI table describes at least one domain with at least one CPU. */
static bool erdt_enabled;
static u32 erdt_max_clos;

#define ERDT_VALID_REVISION		1
#define CMRC_SUPPORTED_INDEX_FN		1
#define MMRC_SUPPORTED_INDEX_FN		1
#define MARC_SUPPORTED_INDEX_FN		1
#define RMDD_FLAG_CPU_L3_DOMAIN		BIT(0)
#define ERDT_DOMAIN_ID_UNSET		-1

#define RDT_CTRL_LEGACY_MODE		BIT(2)

#define MARC_FLAG_OPT			BIT(0)
#define MARC_FLAG_MIN			BIT(1)
#define MARC_FLAG_MAX			BIT(2)

#define CMRC_UNAVAILABLE_COUNTER	BIT_ULL(63)
#define CMRC_FLAG_UNAVAILABLE_BIT	BIT(0)
#define MMRC_UNAVAILABLE_COUNTER	BIT_ULL(63)
#define MMRC_FLAG_UNAVAILABLE_BIT	BIT(0)
#define FIXPOINT_LOW_BITS		16

/* Bitmask of valid sub-tables found in the first RMDD, used to ensure all RMDDs match. */
static u32 valid_subtbl_mask;

/* Domain ID of the first RMDD that established @valid_subtbl_mask, for diagnostics. */
static u16 first_rmdd_domain_id;

/* Largest RMID that is valid in every ERDT domain. */
static unsigned int erdt_max_rmid;

/*
 * Used only to round the global resctrl_rmid_realloc_threshold for the limbo
 * handler. resctrl_rmid_realloc_threshold is exposed to user space as a single
 * global value, so ERDT uses one scale derived from the per-domain
 * cmrc->up_scale values. Keep this single threshold conservative across
 * domains because the rounding is a floor: a larger scale yields a slightly
 * lower threshold, i.e. an RMID has to drop to a slightly lower occupancy
 * before it is reused.
 */
static unsigned int erdt_cmt_scale;

bool erdt_support(int flag)
{
	if (!erdt_enabled)
		return false;

	switch (flag) {
	case X86_FEATURE_CQM_OCCUP_LLC:
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC);
	case X86_FEATURE_CQM_MBM_TOTAL:
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_MMRC);
	case X86_FEATURE_MBA:
		return valid_subtbl_mask & BIT(ACPI_ERDT_TYPE_MARC);
	default:
		return false;
	}
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

/**
 * erdt_get_cmt_max_rmid() - Get the RMID limit of CMT via ERDT
 *
 * Return the ERDT limit only when LLC occupancy is read via ERDT, rather than
 * whenever ERDT is enabled. This prevents, for example, booting with
 * "rdt=!cmt" or ERDT without CMRC from restricting the RMIDs of the MSR-based
 * events to the ERDT range even though no event is read via ERDT.
 *
 * Return: The largest RMID valid in every ERDT domain if LLC occupancy is read
 * via ERDT, 0 otherwise.
 */
unsigned int erdt_get_cmt_max_rmid(void)
{
	if (!erdt_cpu_has(X86_FEATURE_CQM_OCCUP_LLC))
		return 0;

	return erdt_max_rmid;
}

unsigned int erdt_get_cmt_scale(void)
{
	if (!erdt_cpu_has(X86_FEATURE_CQM_OCCUP_LLC))
		return 0;

	return erdt_cmt_scale;
}

/*
 * Compute the CMRC MMIO offset for CMRC_SUPPORTED_INDEX_FN using the
 * MMIO_offset_for_RMID# = (RMID / ClumpSize) * Stride + (RMID % ClumpSize) * 8
 * formula from the Intel RDT Architecture Specification.
 */
static u32 cmrc_index_function_1(struct acpi_erdt_cmrc *cmrc, u32 rmid)
{
	return (rmid / cmrc->clump_size) * cmrc->clump_stride +
	       (rmid % cmrc->clump_size) * sizeof(u64);
}

static int erdt_read_l3_occupancy(const struct erdt_domain_info *d, u32 rmid, u64 *val)
{
	struct acpi_erdt_cmrc *cmrc;
	u64 l3_cmt_count;
	u32 offset;

	cmrc = d->cmrc;
	if (!cmrc)
		return -EIO;

	offset = cmrc_index_function_1(cmrc, rmid);
	if (offset + sizeof(u64) > (u64)cmrc->cmt_reg_size * SZ_4K)
		return -EINVAL;

	l3_cmt_count = readq(d->base[ERDT_MMIO_CMRC_BASE] + offset);
	if ((cmrc->flags & CMRC_FLAG_UNAVAILABLE_BIT) &&
	    (l3_cmt_count & CMRC_UNAVAILABLE_COUNTER))
		return -EINVAL;

	*val = l3_cmt_count * cmrc->up_scale;

	return 0;
}

static u64 mmrc_index_function_1(u32 rmid, u32 region_idx)
{
	u64 blk_rmid, blk_offset;

	/*
	 * Block_to_locate_RMID# = floor((RMID# % 32) / 8) x 4 x 4096B;
	 * Offset_within_this_Block = (floor(((RMID#/32)x8)+RMID#%8) x
	 * 8B)+(Region# x 2048B);
	 * MMIO_ADDRESS_for_RMID#_Region# =
	 * MBM Register Block Base Address + Block_to_locate_RMID# +
	 * Offset_within_this_Block;
	 */
	blk_rmid = ((rmid % 32) / 8) * 4 * SZ_4K;
	blk_offset = ((rmid / 32) * 8 + (rmid % 8)) * 8 + region_idx * 2048;

	return blk_rmid + blk_offset;
}

static u64 apply_correction_factor(u64 val, u32 factor)
{
	if (!factor)
		return val;

	return ((val * factor) >> FIXPOINT_LOW_BITS);
}

static int erdt_read_region_mbm(struct rdt_domain_hdr *hdr,
				const struct erdt_domain_info *d, int rmid,
				int eventid, u64 *val, bool first)
{
	u32 region_idx = RMBM_STATE_IDX(eventid);
	int corr_factor_len, corr_factor = 0;
	struct rdt_hw_l3_mon_domain *hw_dom;
	u64 mbm_rmid_count = 0, chunks = 0;
	struct acpi_erdt_mmrc *mmrc = NULL;
	struct rdt_l3_mon_domain *mon_dom;
	struct arch_mbm_state *am;
	u64 offset;

	mmrc = d->mmrc;
	if (!mmrc)
		return -EIO;

	if (!domain_header_is_valid(hdr, RESCTRL_MON_DOMAIN, RDT_RESOURCE_L3))
		return -EIO;

	mon_dom = container_of(hdr, struct rdt_l3_mon_domain, hdr);
	hw_dom = resctrl_to_arch_mon_dom(mon_dom);

	offset = mmrc_index_function_1(rmid, region_idx);
	if (offset + sizeof(u64) > (u64)mmrc->reg_size * SZ_4K)
		return -EINVAL;

	mbm_rmid_count = readq(d->base[ERDT_MMIO_MMRC_BASE] + offset);

	/*
	 * Bit 63 only reports whether the data is unavailable on domains that
	 * advertise support for it. Where it is not supported the bit is part
	 * of the counter and must not be interpreted as a status flag.
	 */
	if ((mmrc->flags & MMRC_FLAG_UNAVAILABLE_BIT) &&
	    (mbm_rmid_count & MMRC_UNAVAILABLE_COUNTER))
		return -EINVAL;

	am = get_arch_mbm_state(hw_dom, rmid, eventid);

	/*
	 * The first read only records the initial, non-zero count value so
	 * that later reads can compute a delta from it. Nothing is reported
	 * to the caller, and the software state must not accumulate the
	 * absolute counter value as if it had been consumed.
	 */
	if (first) {
		if (am)
			am->prev_mon_val = mbm_rmid_count;

		*val = 0;

		return 0;
	}

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
		else if (rmid < corr_factor_len)
			corr_factor = mmrc->corr_factor_list[rmid];
		else
			return -EINVAL;
	}

	if (am) {
		am->chunks += mbm_overflow_count(am->prev_mon_val, mbm_rmid_count,
						mmrc->counter_width);

		chunks = apply_correction_factor(am->chunks, corr_factor);
		am->prev_mon_val = mbm_rmid_count;
	}

	*val = chunks * mmrc->up_scale;

	return 0;
}

int erdt_mon_read(struct rdt_domain_hdr *hdr, enum resctrl_event_id evtid, u32 rmid,
		  u64 *val, bool first)
{
	struct rdt_hw_l3_mon_domain *hw_dom;
	const struct erdt_domain_info *d;

	hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));
	d = hw_dom->d_info;
	if (WARN_ON_ONCE(!d))
		return -EIO;

	if (evtid == QOS_L3_OCCUP_EVENT_ID)
		return erdt_read_l3_occupancy(d, rmid, val);

	if (rmbm_event(evtid))
		return erdt_read_region_mbm(hdr, d, rmid, evtid, val, first);

	return -EIO;
}

union marc_bw_ctrl {
	u64	reg;
	struct {
		u16	val : 9;
		u16	rsvd : 7;
	} regions[4];
};

void erdt_marc_hw_update(struct hw_param *m)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(m->dom);
	struct resctrl_hw_ctrl *hw_ctrl = resctrl_to_arch_ctrl(m->ctrl);
	enum resctrl_ctrl_name name = hw_ctrl->r_ctrl.name;
	struct erdt_domain_info *d = hw_dom->d_info;
	unsigned int offset, region, type;
	enum erdt_mmio_type mmio_type;
	union marc_bw_ctrl mmio_ctrl;
	void __iomem *addr;
	int closid_idx;
	unsigned int i;

	if (!d || !d->marc)
		return;

	offset = name - RESCTRL_CTRL_NAME_REGION0_OPT;
	region = offset / RESCTRL_CTRL_REGION_NR_CTRLS;
	type = offset % RESCTRL_CTRL_REGION_NR_CTRLS;
	mmio_type = ERDT_MMIO_MARC_OPT + type;

	if (d->marc_buf_type != mmio_type) {
		memset(d->marc_buf, 0,
		       d->marc->mba_reg_size * 512 * sizeof(u64));
		d->marc_buf_type = mmio_type;
	}

	for (i = m->low; i < m->high; i++) {
		closid_idx = (region / 4) * 64 + i;
		if (WARN_ON_ONCE((u64)closid_idx * sizeof(u64) + sizeof(u64) >
				 (u64)d->marc->mba_reg_size * SZ_4K))
			return;

		addr = d->base[mmio_type] + closid_idx * 8;

		/* The cached value retains the reserved bits to be preserved. */
		mmio_ctrl.reg = d->marc_buf[closid_idx];
		if (!mmio_ctrl.reg)
			mmio_ctrl.reg = readq(addr);

		if (WARN_ON_ONCE(!mmio_ctrl.reg))
			return;

		mmio_ctrl.regions[region % 4].val = hw_dom->ctrl_val[i];
		d->marc_buf[closid_idx] = mmio_ctrl.reg;
		writeq(mmio_ctrl.reg, addr);
	}
}

static __init struct resctrl_ctrl *erdt_legacy_mba_ctrl(struct rdt_resource *r)
{
	struct resctrl_ctrl *ctrl;

	for_each_resource_ctrl(ctrl, r)
		if (ctrl->name == RESCTRL_CTRL_NAME_DEF)
			return ctrl;

	return NULL;
}

__init bool erdt_get_mem_config(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	struct resctrl_ctrl *legacy_ctrl;
	struct resctrl_hw_ctrl *hw_ctrl;
	struct erdt_domain_info *d;
	struct acpi_erdt_marc *marc;
	int max_regions, region;
	unsigned int type;

	if (!erdt_cpu_has(X86_FEATURE_MBA))
		return false;

	/* Use the first domain's MARC to discover control properties. */
	d = list_first_entry_or_null(&domain_info_list, struct erdt_domain_info, entry);
	if (!d || !d->marc)
		return false;

	marc = d->marc;
	max_regions = acpi_mrrm_max_mem_region();

	legacy_ctrl = erdt_legacy_mba_ctrl(r);
	if (!legacy_ctrl)
		return false;

	/*
	 * The legacy MB control (MSR, CPUID enumerated CLOS) and the region
	 * controls (MMIO, ERDT Max CLOS) share num_closid. Use the smaller
	 * of the two so that every CLOS is valid for both interfaces.
	 */
	hw_res->num_closid = min(hw_res->num_closid, erdt_max_clos + 1);

	for (region = 0; region < max_regions; region++) {
		for (type = RESCTRL_CTRL_REGION_TYPE_OPT; type < RESCTRL_CTRL_REGION_NR_CTRLS; type++) {
			if (type == RESCTRL_CTRL_REGION_TYPE_OPT && !(marc->flags & MARC_FLAG_OPT))
				continue;
			if (type == RESCTRL_CTRL_REGION_TYPE_MIN && !(marc->flags & MARC_FLAG_MIN))
				continue;
			if (type == RESCTRL_CTRL_REGION_TYPE_MAX && !(marc->flags & MARC_FLAG_MAX))
				continue;

			hw_ctrl = kzalloc_obj(*hw_ctrl);
			if (!hw_ctrl)
				return false;

			hw_ctrl->r_ctrl.scope = RESCTRL_L3_CACHE;
			hw_ctrl->r_ctrl.type = RESCTRL_CTRL_SCALAR;
			hw_ctrl->r_ctrl.name = resctrl_ctrl_name_region(region, type);
			INIT_LIST_HEAD(&hw_ctrl->r_ctrl.domains);
			INIT_LIST_HEAD(&hw_ctrl->r_ctrl.emulated_by);

			hw_ctrl->r_ctrl.scalar.max = marc->mba_ctrl_range;
			hw_ctrl->r_ctrl.scalar.min = 1;
			hw_ctrl->r_ctrl.scalar.gran = 1;
			/*
			 * The default hardware state has Min == Optimal == Max BW
			 * Cap, all at the maximum (unthrottled) value, so every
			 * region control defaults to the full range.
			 */
			hw_ctrl->r_ctrl.scalar.reset_val = hw_ctrl->r_ctrl.scalar.max;
			hw_ctrl->r_ctrl.scalar.resolution = 1;
			hw_ctrl->r_ctrl.scalar.tolerance = 0;
			hw_ctrl->r_ctrl.scalar.scale = 1;
			hw_ctrl->r_ctrl.scalar.unit = RESCTRL_CTRL_UNIT_ALL;

			/*
			 * Boot in legacy mode: the MMIO interface is installed by
			 * resctrl_arch_control_mode_set() when switching to native mode.
			 */
			hw_ctrl->hw_update = NULL;
			/* MARC controls live in MMIO, program from any CPU. */
			hw_ctrl->any_cpu = true;
			list_add_tail(&hw_ctrl->r_ctrl.entry,
				      &legacy_ctrl->emulated_by);
		}
	}

	r->alloc_capable = true;

	return true;
}

static void __iomem *erdt_ioremap(resource_size_t base, u32 num_pages, const char *desc)
{
	void __iomem *addr;
	unsigned long size;

	if (check_mul_overflow(num_pages, SZ_4K, &size))
		return NULL;

	addr = ioremap(base, size);
	if (!addr) {
		pr_warn(FW_BUG "Failed to map %s at phys addr %pa (size: %u pages)\n",
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

static void region_aware_enable(void __iomem *addr, bool enable)
{
	u64 rdt_ctrl = readq(addr);

	if (enable)
		rdt_ctrl &= ~RDT_CTRL_LEGACY_MODE;
	else
		rdt_ctrl |= RDT_CTRL_LEGACY_MODE;

	writeq(rdt_ctrl, addr);
}

/*
 * Region-aware MBM and MBA must be enabled or disabled together and
 * consistently across all RMDDs, so switch every domain at once.
 *
 * The ERDT domains are enumerated during boot and the list is read-only
 * afterwards, so no additional serialization is needed here. Callers are
 * responsible for serializing the read-modify-write of RDT_CTRL.
 */
void erdt_region_aware_enable_all(bool enable)
{
	struct erdt_domain_info *d;

	if (!erdt_enabled)
		return;

	list_for_each_entry(d, &domain_info_list, entry)
		region_aware_enable(d->base[ERDT_MMIO_RMDD_CREG], enable);
}

static void cleanup_one_domain(struct erdt_domain_info *d)
{
	if (d->base[ERDT_MMIO_RMDD_CREG])
		region_aware_enable(d->base[ERDT_MMIO_RMDD_CREG], false);

	erdt_iounmap_domain(d);
	kfree(d->marc_buf);
	kfree(d->cmrc);
	kfree(d->mmrc);
	kfree(d->marc);
	kfree(d);
}

/*
 * Save CACD information for this RMDD:
 * convert the x2APIC IDs to CPUs and save them in a mask.
 */
static __init bool cacd_init(struct acpi_subtbl_hdr_16 *subtbl,
			     struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cacd *cacd = (struct acpi_erdt_cacd *)subtbl;
	unsigned int num_ids;
	int cpu;

	if (cacd->header.length < struct_size(cacd, X2APICIDS, 1)) {
		pr_warn(FW_BUG "Invalid x2APIC ID CACD table\n");
		return false;
	}

	num_ids = (cacd->header.length - sizeof(*cacd)) / sizeof(cacd->X2APICIDS[0]);

	for (unsigned int i = 0; i < num_ids; i++) {
		cpu = topo_lookup_cpuid(cacd->X2APICIDS[i]);
		/* Skip CPUs not in the possible mask, e.g. excluded by nr_cpus= */
		if (cpu < 0)
			continue;

		cpumask_set_cpu(cpu, &domain_info->cpu_mask);
	}

	return true;
}

static __init bool cmrc_init(struct acpi_subtbl_hdr_16 *subtbl,
			     struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_cmrc *cmrc = (struct acpi_erdt_cmrc *)subtbl;

	if (cmrc->header.length < sizeof(*cmrc)) {
		pr_warn(FW_BUG "Truncated CMRC sub-table\n");
		return false;
	}

	if (cmrc->index_fn != CMRC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported CMRC index function %u\n", cmrc->index_fn);
		return false;
	}

	if (!cmrc->clump_size) {
		pr_warn(FW_BUG "CMRC clump_size is zero\n");
		return false;
	}

	/* resctrl scales monitoring values with an unsigned int. */
	if (cmrc->up_scale > UINT_MAX) {
		pr_warn(FW_BUG "Insane CMRC up_scale value 0x%llx\n", cmrc->up_scale);
		return false;
	}

	domain_info->base[ERDT_MMIO_CMRC_BASE] =
		erdt_ioremap(cmrc->cmt_reg_base, cmrc->cmt_reg_size, "CMRC base");
	if (!domain_info->base[ERDT_MMIO_CMRC_BASE])
		return false;

	domain_info->cmrc = kmemdup(cmrc, cmrc->header.length, GFP_KERNEL);
	if (!domain_info->cmrc) {
		iounmap(domain_info->base[ERDT_MMIO_CMRC_BASE]);
		domain_info->base[ERDT_MMIO_CMRC_BASE] = NULL;
		return false;
	}

	return true;
}

static __init bool mmrc_init(struct acpi_subtbl_hdr_16 *subtbl,
			     struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_mmrc *mmrc = (struct acpi_erdt_mmrc *)subtbl;

	if (mmrc->header.length < sizeof(*mmrc)) {
		pr_warn(FW_BUG "Truncated MMRC sub-table\n");
		return false;
	}

	if (mmrc->index_fn != MMRC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported MMRC index function %u\n", mmrc->index_fn);
		return false;
	}

	/*
	 * The correction factor list is a trailing flexible array, so it is
	 * not covered by the sizeof(*mmrc) check above. Firmware could claim
	 * more entries than the sub-table actually carries, so only trust the
	 * length the sub-table can back.
	 */
	if (mmrc->header.length < struct_size(mmrc, corr_factor_list,
					      mmrc->corr_factor_list_len)) {
		pr_warn(FW_BUG "MMRC correction factor list of %u entries exceeds sub-table\n",
			mmrc->corr_factor_list_len);
		return false;
	}

	domain_info->base[ERDT_MMIO_MMRC_BASE] =
		erdt_ioremap(mmrc->reg_base, mmrc->reg_size, "MMRC base");
	if (!domain_info->base[ERDT_MMIO_MMRC_BASE])
		return false;

	domain_info->mmrc = kmemdup(mmrc, mmrc->header.length, GFP_KERNEL);
	if (!domain_info->mmrc) {
		iounmap(domain_info->base[ERDT_MMIO_MMRC_BASE]);
		domain_info->base[ERDT_MMIO_MMRC_BASE] = NULL;
		return false;
	}

	return true;
}

static __init void marc_iounmap(struct erdt_domain_info *domain_info)
{
	for (int i = ERDT_MMIO_MARC_OPT; i <= ERDT_MMIO_MARC_MAX; i++) {
		if (domain_info->base[i]) {
			iounmap(domain_info->base[i]);
			domain_info->base[i] = NULL;
		}
	}
}

static __init bool marc_init(struct acpi_subtbl_hdr_16 *subtbl,
			     struct erdt_domain_info *domain_info)
{
	struct acpi_erdt_marc *marc = (struct acpi_erdt_marc *)subtbl;

	if (marc->header.length < sizeof(*marc)) {
		pr_warn(FW_BUG "Truncated MARC sub-table\n");
		return false;
	}

	if (marc->index_fn != MARC_SUPPORTED_INDEX_FN) {
		pr_info("Unsupported MARC index function %u\n", marc->index_fn);
		return false;
	}

	if (marc->flags & MARC_FLAG_OPT) {
		domain_info->base[ERDT_MMIO_MARC_OPT] =
			erdt_ioremap(marc->reg_base_opt, marc->mba_reg_size, "MARC OPT base");
		if (!domain_info->base[ERDT_MMIO_MARC_OPT])
			return false;
	}

	if (marc->flags & MARC_FLAG_MIN) {
		domain_info->base[ERDT_MMIO_MARC_MIN] =
			erdt_ioremap(marc->reg_base_min, marc->mba_reg_size, "MARC MIN base");
		if (!domain_info->base[ERDT_MMIO_MARC_MIN])
			goto unmap;
	}

	if (marc->flags & MARC_FLAG_MAX) {
		domain_info->base[ERDT_MMIO_MARC_MAX] =
			erdt_ioremap(marc->reg_base_max, marc->mba_reg_size, "MARC MAX base");
		if (!domain_info->base[ERDT_MMIO_MARC_MAX])
			goto unmap;
	}

	domain_info->marc = kmemdup(marc, marc->header.length, GFP_KERNEL);
	if (!domain_info->marc)
		goto unmap;

	domain_info->marc_buf = kcalloc(marc->mba_reg_size * 512, sizeof(u64),
					GFP_KERNEL);
	if (!domain_info->marc_buf) {
		kfree(domain_info->marc);
		domain_info->marc = NULL;
		goto unmap;
	}

	return true;

unmap:
	marc_iounmap(domain_info);
	return false;
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

static __init bool parse_rmdd_table(struct acpi_subtbl_hdr_16 *rmdd_hdr)
{
	struct acpi_erdt_rmdd *rmdd = (struct acpi_erdt_rmdd *)rmdd_hdr;
	struct erdt_domain_info *domain_info;
	struct acpi_subtbl_hdr_16 *subtbl;
	u32 subtbl_mask = 0;
	bool ret = false;

	if (rmdd->header.length < sizeof(*rmdd)) {
		pr_warn(FW_BUG "Invalid RMDD length %u bytes\n", rmdd->header.length);
		return false;
	}

	/* Quietly ignore non-CPU-based L3 domains */
	if (!(rmdd->flags & RMDD_FLAG_CPU_L3_DOMAIN))
		return true;

	domain_info = kzalloc_obj(*domain_info, GFP_KERNEL);
	if (!domain_info)
		return false;

	domain_info->dom_id = ERDT_DOMAIN_ID_UNSET;

	for (subtbl = (void *)rmdd + sizeof(*rmdd);
	     subtbl_valid((void *)rmdd + rmdd->header.length, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		switch (subtbl->type) {
		/* An RMDD table has one or more CACD sub-table(s) */
		case ACPI_ERDT_TYPE_CACD:
			if (!cacd_init(subtbl, domain_info))
				goto cleanup;

			subtbl_mask |= BIT(ACPI_ERDT_TYPE_CACD);
			break;
		/* An RMDD table supports at most one CMRC sub-table */
		case ACPI_ERDT_TYPE_CMRC:
			if (subtbl_mask & BIT(ACPI_ERDT_TYPE_CMRC))
				break;

			if (!cmrc_init(subtbl, domain_info))
				goto cleanup;

			subtbl_mask |= BIT(ACPI_ERDT_TYPE_CMRC);
			break;
		case ACPI_ERDT_TYPE_MMRC:
			if (!(subtbl_mask & BIT(ACPI_ERDT_TYPE_MMRC)) &&
			    mmrc_init(subtbl, domain_info))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_MMRC);

			break;
		case ACPI_ERDT_TYPE_MARC:
			if (!(subtbl_mask & BIT(ACPI_ERDT_TYPE_MARC)) &&
			    marc_init(subtbl, domain_info))
				subtbl_mask |= BIT(ACPI_ERDT_TYPE_MARC);

			break;
		default:
			break;
		}
	}

	/* A CPU-based L3 RMDD must have at least one CACD sub-table */
	if (!(subtbl_mask & BIT(ACPI_ERDT_TYPE_CACD))) {
		pr_warn(FW_BUG "RMDD %u has no CACD sub-table\n", rmdd->domain_id);
		goto cleanup;
	}

	/* Quietly skip an RMDD without any CPU, e.g. all excluded by nr_cpus= */
	if (cpumask_empty(&domain_info->cpu_mask)) {
		ret = true;
		goto cleanup;
	}

	domain_info->base[ERDT_MMIO_RMDD_CREG] =
		erdt_ioremap(rmdd->creg_base, rmdd->creg_size, "RMDD ctrl base");
	if (!domain_info->base[ERDT_MMIO_RMDD_CREG])
		goto cleanup;

	/*
	 * Require all RMDDs to support same set of sub-tables
	 */
	if (!valid_subtbl_mask) {
		valid_subtbl_mask = subtbl_mask;
		first_rmdd_domain_id = rmdd->domain_id;
	} else if (subtbl_mask != valid_subtbl_mask) {
		pr_warn(FW_BUG "RMDD %u sub-table set does not match the first RMDD %u\n",
			rmdd->domain_id, first_rmdd_domain_id);
		goto cleanup;
	}

	if (!rmdd->max_rmid) {
		pr_warn(FW_BUG "Unreasonable RMDD max_rmid 0\n");
		goto cleanup;
	}

	if (!erdt_max_rmid)
		erdt_max_rmid = rmdd->max_rmid;
	else
		erdt_max_rmid = min(erdt_max_rmid, rmdd->max_rmid);

	if (domain_info->cmrc)
		erdt_cmt_scale = max(erdt_cmt_scale, domain_info->cmrc->up_scale);

	list_add(&domain_info->entry, &domain_info_list);

	return true;

cleanup:
	cleanup_one_domain(domain_info);
	return ret;
}

/**
 * erdt_try_bind_domain() - Validate @cpu's L3 domain against ERDT and bind the
 * ERDT domain containing @cpu to that L3 domain.
 * @cpu: CPU that is coming online
 *
 * Find the erdt_domain_info whose CACD lists @cpu, then bind that ERDT domain
 * to @cpu's L3 id. The binding fails if that ERDT domain is already bound to a
 * different L3 id, or if another ERDT domain is already bound to @cpu's L3 id.
 *
 * For example, the CACD sub-tables report:
 * domain0: CPU0, CPU2, domain1: CPU1, CPU3
 * while CPUID/cacheinfo reports the L3 cache is shared by:
 * id0: CPU0, CPU1, id1: CPU2, CPU3
 * With the CPUs coming online in order:
 * - CPU0 binds domain0 to L3 id0.
 * - CPU1 is in the unbound domain1, but L3 id0 is already bound to domain0,
 *   so binding fails for CPU1.
 * - CPU2 is in domain0, which is bound to L3 id0 rather than CPU2's L3 id1,
 *   so binding fails for CPU2.
 * - CPU3 binds domain1 to L3 id1.
 *
 * Return: true if ERDT is unused, or if the ERDT domain containing @cpu is
 * bound to @cpu's L3 cache ID, whether that binding already existed or was
 * created by this call. false if ERDT and CPUID cannot be reconciled into a
 * one-to-one binding, in which case no binding is modified.
 */
bool erdt_try_bind_domain(int cpu)
{
	struct erdt_domain_info *d, *cpu_dom = NULL;
	int dom_id;

	lockdep_assert_held(&domain_list_lock);

	/* Without ERDT there is no firmware topology to disagree with. */
	if (!erdt_enabled)
		return true;

	dom_id = get_cpu_cacheinfo_id(cpu, RESCTRL_L3_CACHE);
	if (dom_id < 0) {
		pr_warn_once(FW_BUG "Cannot find L3 id for CPU%d\n", cpu);
		return false;
	}

	list_for_each_entry(d, &domain_info_list, entry) {
		if (cpumask_test_cpu(cpu, &d->cpu_mask)) {
			cpu_dom = d;
			break;
		}
	}

	if (!cpu_dom) {
		pr_warn_once(FW_BUG "Cannot find the ERDT domain which has CPU%d\n", cpu);
		return false;
	}

	/* This ERDT domain is already bound to this CPU's L3 domain. */
	if (cpu_dom->dom_id == dom_id)
		return true;

	/*
	 * This ERDT domain is already bound to a different L3 domain. Rebinding it
	 * would leave two L3 domains reading the counters of one ERDT domain, so
	 * fail instead (CPU2 in the example above).
	 */
	if (cpu_dom->dom_id != ERDT_DOMAIN_ID_UNSET) {
		pr_warn_once(FW_BUG "CPU%d's id=%d not equal to CACD domain(%*pbl) id=%d, skip this CPU\n",
			     cpu, dom_id, cpumask_pr_args(&cpu_dom->cpu_mask), cpu_dom->dom_id);

		return false;
	}

	/*
	 * A possible new binding. Check if another ERDT domain shares the same
	 * L3 id. If yes, this is a conflict (CPU1 in the example above).
	 */
	list_for_each_entry(d, &domain_info_list, entry) {
		if (d == cpu_dom)
			continue;

		if (d->dom_id == dom_id) {
			pr_warn_once(FW_BUG "CPU%d's id=%d is already used by CACD domain(%*pbl), skip this CPU\n",
				     cpu, dom_id, cpumask_pr_args(&d->cpu_mask));

			return false;
		}
	}

	/* Eligible new binding, assign the L3 id. */
	cpu_dom->dom_id = dom_id;

	return true;
}

/* Find the ERDT domain bound to L3 cache @dom_id by erdt_try_bind_domain(). */
static struct erdt_domain_info *erdt_find_domain_info(int dom_id)
{
	struct erdt_domain_info *d;

	list_for_each_entry(d, &domain_info_list, entry) {
		if (d->dom_id == dom_id)
			return d;
	}

	return NULL;
}

/**
 * erdt_l3_mon_domain_setup() - Associate ERDT table information with an L3
 * monitoring domain.
 * @hdr: Header of the L3 monitoring domain being created
 *
 * The CPU creating this domain has passed erdt_try_bind_domain(), which bound
 * an ERDT domain to this CPU's L3 cache ID. Without SNC the L3 monitoring
 * domain ID is also the L3 cache ID, so a match is always found and
 * hw_dom->d_info always points to a valid ERDT domain while ERDT is enabled.
 */
void erdt_l3_mon_domain_setup(struct rdt_domain_hdr *hdr)
{
	struct rdt_hw_l3_mon_domain *hw_dom;

	lockdep_assert_held(&domain_list_lock);

	if (!erdt_enabled)
		return;

	hw_dom = resctrl_to_arch_mon_dom(container_of(hdr, struct rdt_l3_mon_domain, hdr));
	hw_dom->d_info = erdt_find_domain_info(hdr->id);
}

void erdt_ctrl_domain_setup(struct resctrl_ctrl *ctrl, struct rdt_domain_hdr *hdr)
{
	struct rdt_hw_ctrl_domain *hw_dom;

	lockdep_assert_held(&domain_list_lock);

	if (!erdt_enabled || ctrl->scope != RESCTRL_L3_CACHE)
		return;

	hw_dom = resctrl_to_arch_ctrl_dom(container_of(hdr, struct rdt_ctrl_domain, hdr));
	hw_dom->d_info = erdt_find_domain_info(hdr->id);
}

void erdt_exit(void)
{
	struct erdt_domain_info *d, *tmp;

	list_for_each_entry_safe(d, tmp, &domain_info_list, entry) {
		list_del(&d->entry);
		cleanup_one_domain(d);
	}
	erdt_enabled = false;
	valid_subtbl_mask = 0;
	first_rmdd_domain_id = 0;
	erdt_max_rmid = 0;
	erdt_cmt_scale = 0;
}

static __init int enumerate_erdt_table(struct acpi_table_header *table_hdr)
{
	struct acpi_table_erdt *erdt = (struct acpi_table_erdt *)table_hdr;
	struct acpi_subtbl_hdr_16 *subtbl;

	/*
	 * ERDT describes monitoring resources per L3 cache domain, while SNC
	 * splits an L3 cache into NUMA node scoped monitoring domains. The two
	 * cannot be matched up, so leave ERDT disabled and fall back to the
	 * MSR based interface.
	 */
	if (snc_nodes_per_l3_cache > 1) {
		pr_info("ERDT not supported with Sub-NUMA Cluster enabled\n");
		return -EINVAL;
	}

	if (erdt->header.revision != ERDT_VALID_REVISION) {
		pr_info("Unsupported table revision %u (expected %u)\n",
			erdt->header.revision, ERDT_VALID_REVISION);
		return -EINVAL;
	}

	if (erdt->header.length < sizeof(*erdt)) {
		pr_warn(FW_BUG "Invalid table length %u bytes\n", erdt->header.length);
		return -EINVAL;
	}

	for (subtbl = (void *)erdt + sizeof(*erdt);
	     subtbl_valid((void *)erdt + erdt->header.length, subtbl);
	     subtbl = next_subtbl(subtbl)) {
		if (subtbl->type == ACPI_ERDT_TYPE_RMDD &&
		    !parse_rmdd_table(subtbl))
			goto cleanup;
	}

	if (list_empty(&domain_info_list))
		goto cleanup;

	erdt_max_clos = erdt->max_clos;
	erdt_enabled = true;

	return 0;

cleanup:
	erdt_exit();
	return -EINVAL;
}

__init void erdt_init(void)
{
	acpi_table_parse(ACPI_SIG_ERDT, enumerate_erdt_table);
}
