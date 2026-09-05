/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __LINUX_ARM_RMI_CMDS_H_
#define __LINUX_ARM_RMI_CMDS_H_

#include <linux/arm-smccc-rmi.h>
#include <linux/bug.h>
#include <linux/types.h>

#define RMI_MAX_ADDR_LIST	256

struct rmi_sro_state {
	struct arm_smccc_1_2_regs regs;
	unsigned long addr_count;
	unsigned long addr_list[RMI_MAX_ADDR_LIST];
};

unsigned long rmi_feat_reg(unsigned long id);

int rmi_delegate_range(phys_addr_t phys, unsigned long size,
		       phys_addr_t *out_phys);
int rmi_undelegate_range(phys_addr_t phys, unsigned long size);
int free_delegated_page(phys_addr_t phys);

static inline int rmi_delegate_page(phys_addr_t phys)
{
	return rmi_delegate_range(phys, PAGE_SIZE, NULL);
}

static inline int rmi_undelegate_page(phys_addr_t phys)
{
	return rmi_undelegate_range(phys, PAGE_SIZE);
}

bool is_rmi_available(void);

long rmi_sro_memxfer_execute(struct rmi_sro_state *sro, gfp_t gfp);
void rmi_sro_free(struct rmi_sro_state *sro);
long rmi_sro_execute(struct arm_smccc_1_2_regs *regs);

#define rmi_sro_memxfer_cmd(sro, gfp, ...) ({				\
	struct rmi_sro_state *__sro = (sro);				\
	*__sro = (struct rmi_sro_state){ .regs = {__VA_ARGS__} };	\
	long __ret = rmi_sro_memxfer_execute(__sro, gfp);		\
	rmi_sro_free(__sro);						\
	__ret;								\
})

/**
 * rmi_rmm_config_set() - Configure the RMM
 * @cfg_ptr: PA of a struct rmm_config
 *
 * Sets configuration options on the RMM.
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_config_set(unsigned long cfg_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RMM_CONFIG_SET, cfg_ptr, &res);

	return res.a0;
}

/**
 * rmi_rmm_deactivate() - Deactivate the RMM and reclaim any memory donated at
 * rmi_rmm_activate()
 *
 * @sro: Preallocated SRO context to be used
 *
 * Return: 0 on success, positive RMI result code or negative Linux error code
 */
static inline long rmi_rmm_deactivate(struct rmi_sro_state *sro)
{
	return rmi_sro_memxfer_cmd(sro, GFP_KERNEL, SMC_RMI_RMM_DEACTIVATE);
}

/**
 * rmi_rmm_activate() - Activate the RMM
 * @sro: Preallocated SRO context to be used
 *
 * Return: 0 on success, positive RMI result code or negative Linux error code
 */
static inline long rmi_rmm_activate(struct rmi_sro_state *sro)
{
	return rmi_sro_memxfer_cmd(sro, GFP_KERNEL, SMC_RMI_RMM_ACTIVATE);
}

/**
 * rmi_granule_tracking_get() - Get configuration of a Granule tracking region
 * @start: Base PA of the tracking region
 * @end: End of the PA region
 * @out_category: Memory category
 * @out_state: Tracking region state
 * @out_top: Top of the memory region
 *
 * Return: RMI return code
 */
static inline int rmi_granule_tracking_get(unsigned long start,
					   unsigned long end,
					   unsigned long *out_category,
					   unsigned long *out_state,
					   unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_TRACKING_GET, start, end, &res);

	if (res.a0 == RMI_SUCCESS) {
		if (out_category)
			*out_category = res.a1;
		if (out_state)
			*out_state = res.a2;
		if (out_top)
			*out_top = res.a3;
	}

	return res.a0;
}

/*
 * rmi_gpt_info - Query the GPT info for the given PAR.
 * @base: Base of the physical address region
 * @top: Top of the physical address region
 * @out_top: Top of the phyiscal address region for which
 *		the GPT @out_gpt_par_state is valid
 * @out_gpt_par_state: State of the GPT covered by [base, out_top)
 */
static inline long rmi_gpt_info(unsigned long base, unsigned long end,
			       unsigned long *out_top,
			       unsigned long *out_gpt_par_state)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_GPT_INFO, base, end,
	};

	long ret = rmi_sro_execute(&regs);

	if (ret == RMI_SUCCESS) {
		if (out_top)
			*out_top = regs.a1;
		if (out_gpt_par_state)
			*out_gpt_par_state = regs.a2;
	}

	return ret;
}

/**
 * rmi_features() - Read feature register
 * @index: Feature register index
 * @out: Feature register value is written to this pointer
 *
 * Return: RMI return code
 */
static inline int rmi_features(unsigned long index, unsigned long *out)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_FEATURES, index, &res);

	if (res.a0 == RMI_SUCCESS && out)
		*out = res.a1;

	return res.a0;
}

/**
 * rmi_granule_range_delegate() - Delegate granules
 * @base: PA of the first granule of the range
 * @top: PA of the first granule after the range
 * @out_top: PA of the first granule not delegated
 *
 * Delegate a range of granule for use by the realm world. If the entire range
 * was delegated then @out_top == @top, otherwise the function should be called
 * again with @base == @out_top.
 *
 * Return: 0 on success, positive RMI result code or negative Linux error code
 */
static inline long rmi_granule_range_delegate(unsigned long base,
					      unsigned long top,
					      unsigned long *out_top)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_GRANULE_RANGE_DELEGATE, base, top
	};
	long ret = rmi_sro_execute(&regs);

	if (ret == RMI_SUCCESS && out_top)
		*out_top = regs.a1;

	return ret;
}

/**
 * rmi_granule_range_undelegate() - Undelegate a range of granules
 * @base: Base PA of the target range
 * @top: Top PA of the target range
 * @out_top: Returns the top PA of range whose state is undelegated
 *
 * Undelegate a range of granules to allow use by the normal world. Will fail if
 * the granules are in use.
 *
 * Return: 0 on success, positive RMI result code or negative Linux error code
 */
static inline long rmi_granule_range_undelegate(unsigned long base,
						unsigned long top,
						unsigned long *out_top)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_GRANULE_RANGE_UNDELEGATE, base, top
	};
	long ret = rmi_sro_execute(&regs);

	if (ret == RMI_SUCCESS && out_top)
		*out_top = regs.a1;

	return ret;
}

#endif
