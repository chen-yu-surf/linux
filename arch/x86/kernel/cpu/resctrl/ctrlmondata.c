// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Cache Allocation code.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Authors:
 *    Fenghua Yu <fenghua.yu@intel.com>
 *    Tony Luck <tony.luck@intel.com>
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual June 2016, volume 3, section 17.17.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>

#include "internal.h"

/*
 * Program the control's hardware for the config range described by @m. A
 * control flagged RESCTRL_CTRL_FLAG_ANY_CPU (e.g. MMIO based) can be updated
 * from the current CPU; otherwise the update must run on a CPU that belongs
 * to the target domain, so it is issued via an IPI.
 */
static void ctrl_hw_update(struct resctrl_ctrl *ctrl, struct rdt_ctrl_domain *d,
			   struct hw_param *m)
{
	if (ctrl->flags & RESCTRL_CTRL_FLAG_ANY_CPU)
		rdt_ctrl_update(m);
	else
		smp_call_function_any(&d->hdr.cpu_mask, rdt_ctrl_update, m, 1);
}

int resctrl_arch_update_one(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type t, u32 cfg_val)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	struct resctrl_hw_ctrl *hw_ctrl = resctrl_to_arch_ctrl(ctrl);
	u32 idx = resctrl_get_config_index(closid, t);
	struct hw_param hw_param;

	if (!cpumask_test_cpu(smp_processor_id(), &d->hdr.cpu_mask))
		return -EINVAL;

	hw_dom->ctrl_val[idx] = cfg_val;

	hw_param.res = r;
	hw_param.ctrl = ctrl;
	hw_param.dom = d;
	hw_param.low = idx;
	hw_param.high = idx + 1;
	hw_ctrl->hw_update(&hw_param);

	return 0;
}

static void _resctrl_arch_update_domains(struct rdt_resource *r,
					 struct resctrl_ctrl *ctrl, u32 closid)
{
	struct resctrl_staged_config *cfg;
	struct rdt_hw_ctrl_domain *hw_dom;
	struct hw_param hw_param;
	struct rdt_ctrl_domain *d;
	enum resctrl_conf_type t;
	u32 idx;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	hw_param.ctrl = ctrl;
	list_for_each_entry(d, &ctrl->domains, hdr.list) {
		hw_dom = resctrl_to_arch_ctrl_dom(d);
		hw_param.res = NULL;
		for (t = 0; t < CDP_NUM_TYPES; t++) {
			cfg = &hw_dom->d_resctrl.staged_config[t];
			if (!cfg->have_new_ctrl)
				continue;

			idx = resctrl_get_config_index(closid, t);
			if (cfg->new_ctrl == hw_dom->ctrl_val[idx])
				continue;
			hw_dom->ctrl_val[idx] = cfg->new_ctrl;

			if (!hw_param.res) {
				hw_param.low = idx;
				hw_param.high = hw_param.low + 1;
				hw_param.res = r;
				hw_param.dom = d;
			} else {
				hw_param.low = min(hw_param.low, idx);
				hw_param.high = max(hw_param.high, idx + 1);
			}
		}
		if (hw_param.res)
			ctrl_hw_update(ctrl, d, &hw_param);
	}
}


int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid)
{
	struct resctrl_ctrl *ctrl;

	for_each_resource_ctrl(ctrl, r)
		_resctrl_arch_update_domains(r, ctrl, closid);

	return 0;
}

u32 resctrl_arch_get_config(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid, enum
			    resctrl_conf_type type)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	u32 idx = resctrl_get_config_index(closid, type);

	return hw_dom->ctrl_val[idx];
}

bool resctrl_arch_get_io_alloc_enabled(struct rdt_resource *r)
{
	return resctrl_to_arch_res(r)->sdciae_enabled;
}

static void resctrl_sdciae_set_one_amd(void *arg)
{
	bool *enable = arg;

	if (*enable)
		msr_set_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
	else
		msr_clear_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
}

static void _resctrl_sdciae_enable(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				   bool enable)
{
	struct rdt_ctrl_domain *d;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	/* Update MSR_IA32_L3_QOS_EXT_CFG MSR on all the CPUs in all domains */
	list_for_each_entry(d, &ctrl->domains, hdr.list)
		on_each_cpu_mask(&d->hdr.cpu_mask, resctrl_sdciae_set_one_amd, &enable, 1);
}

int resctrl_arch_io_alloc_enable(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				 bool enable)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	if (hw_res->r_resctrl.cache_io_alloc_capable &&
	    hw_res->sdciae_enabled != enable) {
		_resctrl_sdciae_enable(r, ctrl, enable);
		hw_res->sdciae_enabled = enable;
	}

	return 0;
}
