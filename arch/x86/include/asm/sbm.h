/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/apic.h>

static __always_inline u32 arch_sbm_cpu_to_idx(unsigned int cpu)
{
	return cpuid_to_apicid[cpu];
}

static __always_inline u32 arch_sbm_idx_to_cpu(unsigned int idx)
{
	return apicid_to_cpuid[idx];
}
