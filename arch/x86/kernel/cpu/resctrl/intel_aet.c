// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Intel Application Energy Telemetry
 *
 * Copyright (C) 2025 Intel Corporation
 *
 * Author:
 *    Tony Luck <tony.luck@intel.com>
 */

#define pr_fmt(fmt)   "resctrl: " fmt

#include <linux/array_size.h>
#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/intel_pmt_features.h>
#include <linux/intel_vsec.h>
#include <linux/overflow.h>
#include <linux/resctrl.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "internal.h"

/**
 * struct event_group - Events with the same feature type ("energy" or "perf") and guid.
 * @feature:		Type of events, for example FEATURE_PER_RMID_PERF_TELEM or
 *			FEATURE_PER_RMID_ENERGY_TELEM, in this group.
 * @pfg:		Points to the aggregated telemetry space information
 *			returned by the intel_pmt_get_regions_by_feature()
 *			call to the INTEL_PMT_TELEMETRY driver that contains
 *			data for all telemetry regions type @feature.
 *			Valid if the system supports the event group.
 *			NULL otherwise.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	enum pmt_feature_id		feature;
	struct pmt_feature_group	*pfg;
};

static struct event_group *known_event_groups[] = {
};

#define for_each_event_group(_peg)						\
	for (_peg = known_event_groups;						\
	     _peg < &known_event_groups[ARRAY_SIZE(known_event_groups)];	\
	     _peg++)

/* Stub for now */
static bool enable_events(struct event_group *e, struct pmt_feature_group *p)
{
	return false;
}

/*
 * Make a request to the INTEL_PMT_TELEMETRY driver for a copy of the
 * pmt_feature_group for each known feature. If there is one, the returned
 * structure has an array of telemetry_region structures, each element of
 * the array describes one telemetry aggregator.
 * A single pmt_feature_group may include multiple different guids.
 * Save the pmt_feature_group for enabled events.
 */
bool intel_aet_get_events(void)
{
	struct pmt_feature_group *p;
	struct event_group **peg;
	bool ret = false;

	for_each_event_group(peg) {
		p = intel_pmt_get_regions_by_feature((*peg)->feature);
		if (IS_ERR_OR_NULL(p))
			continue;
		if (enable_events(*peg, p)) {
			(*peg)->pfg = p;
			ret = true;
		} else {
			intel_pmt_put_feature_group(p);
		}
	}

	return ret;
}

void __exit intel_aet_exit(void)
{
	struct event_group **peg;

	for_each_event_group(peg) {
		if ((*peg)->pfg) {
			intel_pmt_put_feature_group((*peg)->pfg);
			(*peg)->pfg = NULL;
		}
	}
}
