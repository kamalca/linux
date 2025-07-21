// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/tsm.h>
#include "iommufd_private.h"

/**
 * iommufd_vdevice_tsm_op_ioctl - Handle vdevice TSM operations
 * @ucmd: user command data for IOMMU_VDEVICE_TSM_OP
 *
 * Currently only supports TSM bind/unbind operations
 * Resolve @iommu_vdevice_tsm_op::vdevice_id to a vdevice and dispatch the
 * requested bind/unbind operation through the TSM core.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int iommufd_vdevice_tsm_op_ioctl(struct iommufd_ucmd *ucmd)
{
	int ret;
	struct kvm *kvm = NULL;
	struct iommufd_vdevice *vdev;
	struct iommu_vdevice_tsm_op *cmd = ucmd->cmd;

	if (cmd->flags)
		return -EOPNOTSUPP;

	vdev = iommufd_get_vdevice(ucmd->ictx, cmd->vdevice_id);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);

	if (vdev->viommu->kvm_file)
		kvm = vdev->viommu->kvm_file->private_data;

	if (!kvm) {
		ret = -ENODEV;
		goto out_put_vdev;
	}

	/* tsm layer will take care of parallel calls to tsm_bind/unbind */
	switch (cmd->type) {
	case IOMMU_VDEVICE_TSM_BIND:
		ret = tsm_bind(vdev->idev->dev, kvm, vdev->virt_id);
		break;
	case IOMMU_VDEVICE_TSM_UNBIND:
		ret = tsm_unbind(vdev->idev->dev);
		break;
	default:
		ret = -EINVAL;
		goto out_put_vdev;
	}

	if (ret)
		goto out_put_vdev;

	ret = iommufd_ucmd_respond(ucmd, sizeof(*cmd));

out_put_vdev:
	iommufd_put_object(ucmd->ictx, &vdev->obj);
	return ret;
}

static bool iommufd_vdevice_tsm_req_arch_valid(u32 tvm_arch)
{
	switch (tvm_arch) {
	case IOMMU_VDEVICE_TSM_TVM_ARCH_CCA:
	case IOMMU_VDEVICE_TSM_TVM_ARCH_SEV:
	case IOMMU_VDEVICE_TSM_TVM_ARCH_TDX:
		return true;
	default:
		return false;
	}
}

static bool iommufd_vdevice_tsm_req_op_valid(u32 op, u32 tvm_arch)
{
	switch (op) {
	case TSM_REQ_VALIDATE_MMIO:
	case TSM_REQ_SET_TDI_STATE:
		return true;
	case TSM_REQ_SEV_ENABLE_DMA:
	case TSM_REQ_SEV_DISABLE_DMA:
		return tvm_arch == IOMMU_VDEVICE_TSM_TVM_ARCH_SEV;
	default:
		return false;
	}
}

/**
 * iommufd_vdevice_tsm_req_ioctl - Forward TSM requests
 * @ucmd: user command data for IOMMU_VDEVICE_TSM_REQ
 *
 * Resolve @iommu_vdevice_tsm_req::vdevice_id to a vdevice and pass the
 * request/response buffers to the TSM core.
 *
 * Return:
 *  -errno on error.
 *  positive residue if response/request bytes were left unconsumed.
 *    if response buffer is provided, residue indicates the number of bytes
 *    not used in response buffer
 *    if there is no response buffer, residue indicates the number of bytes
 *    not consumed in req buffer
 *  0 otherwise.
 */
int iommufd_vdevice_tsm_req_ioctl(struct iommufd_ucmd *ucmd)
{
	int ret;
	struct iommufd_vdevice *vdev;
	struct iommu_vdevice_tsm_req *cmd = ucmd->cmd;
	struct tsm_guest_req_info info = {
		.op = cmd->op,
		.tvm_arch = cmd->tvm_arch,
		.req   = {
			.user = u64_to_user_ptr(cmd->req_uptr),
			.is_kernel = false,
		},
		.req_len = cmd->req_len,
		.resp    =  {
			.user = u64_to_user_ptr(cmd->resp_uptr),
			.is_kernel = false,
		},
		.resp_len = cmd->resp_len,
	};

	if (!iommufd_vdevice_tsm_req_arch_valid(cmd->tvm_arch))
		return -EINVAL;

	if (!iommufd_vdevice_tsm_req_op_valid(cmd->op, cmd->tvm_arch))
		return -EINVAL;

	vdev = iommufd_get_vdevice(ucmd->ictx, cmd->vdevice_id);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);

	cmd->tsm_code = 0;
	ret = tsm_guest_req(vdev->idev->dev, &info, &cmd->tsm_code);

	/* Always copy the tsm_code as response */
	if (iommufd_ucmd_respond(ucmd, sizeof(*cmd)))
		ret = -EFAULT;

	iommufd_put_object(ucmd->ictx, &vdev->obj);
	return ret;
}
