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
#include <linux/compiler_types.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/intel_pmt_features.h>
#include <linux/intel_vsec.h>
#include <linux/overflow.h>
#include <linux/resctrl.h>
#include <linux/resctrl_types.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "internal.h"

/**
 * struct pmt_event - Telemetry event.
 * @id:		Resctrl event id.
 * @idx:	Counter index within each per-RMID block of counters.
 * @bin_bits:	Zero for integer valued events, else number bits in fraction
 *		part of fixed-point.
 */
struct pmt_event {
	enum resctrl_event_id	id;
	unsigned int		idx;
	unsigned int		bin_bits;
};

#define EVT(_id, _idx, _bits) { .id = _id, .idx = _idx, .bin_bits = _bits }

/**
 * struct event_group - All information about a group of telemetry events.
 * @feature:		Argument to intel_pmt_get_regions_by_feature() to
 *			discover if this event_group is supported.
 * @pfg:		Points to the aggregated telemetry space information
 *			returned by the intel_pmt_get_regions_by_feature()
 *			call to the INTEL_PMT_TELEMETRY driver that contains
 *			data for all telemetry regions of a specific type.
 *			Valid if the system supports the event group.
 *			NULL otherwise.
 * @guid:		Unique number per XML description file.
 * @mmio_size:		Number of bytes of MMIO registers for this group.
 * @num_events:		Number of events in this group.
 * @evts:		Array of event descriptors.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	enum pmt_feature_id		feature;
	struct pmt_feature_group	*pfg;

	/* Remaining fields initialized from XML file. */
	u32				guid;
	size_t				mmio_size;
	unsigned int			num_events;
	struct pmt_event		evts[] __counted_by(num_events);
};

#define XML_MMIO_SIZE(num_rmids, num_events, num_extra_status) \
		      (((num_rmids) * (num_events) + (num_extra_status)) * sizeof(u64))

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-ENERGY/cwf_aggregator.xml
 */
static struct event_group energy_0x26696143 = {
	.feature	= FEATURE_PER_RMID_ENERGY_TELEM,
	.guid		= 0x26696143,
	.mmio_size	= XML_MMIO_SIZE(576, 2, 3),
	.num_events	= 2,
	.evts		= {
		EVT(PMT_EVENT_ENERGY, 0, 18),
		EVT(PMT_EVENT_ACTIVITY, 1, 18),
	}
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-PERF/cwf_aggregator.xml
 */
static struct event_group perf_0x26557651 = {
	.feature	= FEATURE_PER_RMID_PERF_TELEM,
	.guid		= 0x26557651,
	.mmio_size	= XML_MMIO_SIZE(576, 7, 3),
	.num_events	= 7,
	.evts		= {
		EVT(PMT_EVENT_STALLS_LLC_HIT, 0, 0),
		EVT(PMT_EVENT_C1_RES, 1, 0),
		EVT(PMT_EVENT_UNHALTED_CORE_CYCLES, 2, 0),
		EVT(PMT_EVENT_STALLS_LLC_MISS, 3, 0),
		EVT(PMT_EVENT_AUTO_C6_RES, 4, 0),
		EVT(PMT_EVENT_UNHALTED_REF_CYCLES, 5, 0),
		EVT(PMT_EVENT_UOPS_RETIRED, 6, 0),
	}
};

static struct event_group *known_event_groups[] = {
	&energy_0x26696143,
	&perf_0x26557651,
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
 * Try to use every telemetry aggregator with a known guid.
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
