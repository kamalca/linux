/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __LINUX_ARM_RMI_CMDS_H_
#define __LINUX_ARM_RMI_CMDS_H_

#include <linux/arm-smccc-rmi.h>
#include <linux/bug.h>
#include <linux/types.h>

unsigned long rmi_feat_reg(unsigned long id);

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

#endif
