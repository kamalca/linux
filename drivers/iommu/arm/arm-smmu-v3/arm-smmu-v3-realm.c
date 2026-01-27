// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/arm-smccc-rmi.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/arm-rmi-cmds.h>

#include <asm/kvm_emulate.h>

#include "arm-smmu-v3.h"

#define	RMI_PSMMU_IRQ_GERROR	BIT(0)
#define	RMI_PSMMU_IRQ_EVENTQ	BIT(1)
#define	RMI_PSMMU_IRQ_PRIQ	BIT(2)
#define	RMI_PSMMU_IRQ_CMDQ	BIT(3)

#define	RMI_PSMMU_IRQ_EVENT_NONE	0
#define	RMI_PSMMU_IRQ_EVENT_ERROR	1
#define	RMI_PSMMU_IRQ_EVENT_PSMMU	2
#define	RMI_PSMMU_IRQ_EVENT_VSMMU	3
#define RMI_PSMMU_IRQ_EVENT_MASK	GENMASK(2, 1)
#define RMI_PSMMU_IRQ_EVENT_SHIFT	1
#define RMI_PSMMU_IRQ_EVENT_PENDING	0x1

static irqreturn_t arm_smmu_realm_notify_thread(int irq, void *dev)
{
	int rmi_psmmu_event;
	unsigned long notify_flags;
	struct arm_smmu_device *smmu = dev;
	struct rmi_psmmu_event_details event;

	if (irq == smmu->realm_evtq_irq)
		notify_flags =  RMI_PSMMU_IRQ_EVENTQ;
	else if (irq == smmu->realm_gerr_irq)
		notify_flags =  RMI_PSMMU_IRQ_GERROR;
	else if (irq == smmu->realm_pri_irq)
		notify_flags =  RMI_PSMMU_IRQ_PRIQ;
	else
		return IRQ_HANDLED;

	do {
		if (rmi_psmmu_irq_notify(smmu->base_phys,
					 notify_flags, &event)) {
			dev_warn(smmu->dev,
				 "failed to notify RMM of a SMMU event\n");
			/* there is nothing much we could do. Mark it handled. */
			return IRQ_HANDLED;
		}
		rmi_psmmu_event = (event.flags & RMI_PSMMU_IRQ_EVENT_MASK) >>
				  RMI_PSMMU_IRQ_EVENT_SHIFT;
		switch (rmi_psmmu_event) {
		case RMI_PSMMU_IRQ_EVENT_NONE:
			break;
		case RMI_PSMMU_IRQ_EVENT_ERROR:
			dev_warn(smmu->dev, "SMMU Error reported\n");
			rmi_psmmu_event_consume(smmu->base_phys, notify_flags);
			break;
		case RMI_PSMMU_IRQ_EVENT_PSMMU:
			dev_warn(smmu->dev,
				 "SMMU event (event num: 0x%llx syndrome 0x%llx "
				 "fetch_addr 0x%llx input_addr 0x%llx) reported\n",
				 event.event_num, event.syndrome,
				 event.fetch_addr, event.input_addr);
			rmi_psmmu_event_consume(smmu->base_phys, notify_flags);
			break;
		case RMI_PSMMU_IRQ_EVENT_VSMMU:
			dev_warn(smmu->dev, "Wrong VSMMU event on stream 0x%llx, ignoring\n",
				 event.stream_id);
			rmi_psmmu_event_consume(smmu->base_phys, notify_flags);
			break;
		}

	} while (event.flags & RMI_PSMMU_IRQ_EVENT_PENDING);

	return IRQ_HANDLED;
}

void arm_smmu_setup_realm_irqs(struct arm_smmu_device *smmu)
{
	int irq, ret;

	irq = smmu->realm_evtq_irq;
	if (irq) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_smmu_realm_notify_thread,
						IRQF_ONESHOT,
						"arm-smmu-v3-realm-evtq",
						smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable realm evtq irq\n");
	} else {
		dev_warn(smmu->dev, "no realm evtq irq - events will not be reported!\n");
	}

	irq = smmu->realm_gerr_irq;
	if (irq) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_smmu_realm_notify_thread,
						IRQF_ONESHOT,
						"arm-smmu-v3-realm-gerror",
						smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable realm gerror irq\n");
	} else {
		dev_warn(smmu->dev, "no realm gerr irq - errors will not be reported!\n");
	}

	if (smmu->features & ARM_SMMU_FEAT_PRI) {
		irq = smmu->realm_pri_irq;
		if (irq) {

			ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
							arm_smmu_realm_notify_thread,
							IRQF_ONESHOT,
							"arm-smmu-v3-realm-priq",
							smmu);
			if (ret < 0)
				dev_warn(smmu->dev,
					 "failed to enable realm priq irq\n");
		} else {
			dev_warn(smmu->dev, "no realm priq irq - PRI will be broken\n");
		}
	}
}

static void arm_realm_smmu_v3_destroy(struct iommufd_viommu *viommu)
{
	/* When we add refcount psmmu deactivate here. */
}

static void arm_realm_smmu_v3_vdevice_destroy(struct iommufd_vdevice *vdev)
{
	struct device *dev = iommufd_vdevice_to_device(vdev);
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	/* FIXME which stream to pick */
	/* At this moment, iommufd only supports PCI device that has one SID */
	struct arm_smmu_stream *stream = &master->streams[0];
	struct arm_smmu_device *smmu = master->smmu;
	unsigned long rmi_ret = 0;
	int ret;

	if (!smmu->realm_initialized)
		return;

	ret = rmi_psmmu_st_l2_destroy(smmu->base_phys,
				    ALIGN_DOWN(stream->id, STRTAB_NUM_L2_STES),
				      &rmi_ret);
	if (ret || rmi_ret) {

		/* Table in use */
		if (RMI_RETURN_STATUS(rmi_ret) == RMI_ERROR_PSMMU_ST &&
		    RMI_RETURN_INDEX(rmi_ret) == 2)
			return;

		dev_warn(dev, "failed to destroy realm stream mapping\n");
	}
}

static int arm_realm_smmu_v3_vdevice_init(struct iommufd_vdevice *vdev)
{
	struct device *dev = iommufd_vdevice_to_device(vdev);
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	// fixme which stream to pick
	/* At this moment, iommufd only supports PCI device that has one SID */
	struct arm_smmu_stream *stream = &master->streams[0];
	struct arm_smmu_device *smmu = master->smmu;
	unsigned long rmi_ret = 0;
	int ret;

	if (!smmu->realm_initialized)
		return -EINVAL;

	ret = rmi_psmmu_st_l2_create(smmu->base_phys,
				     ALIGN_DOWN(stream->id, STRTAB_NUM_L2_STES),
				     &rmi_ret);
	if (ret || rmi_ret) {
		if (!ret)
			return -EIO;
		if (RMI_RETURN_STATUS(rmi_ret) == RMI_ERROR_PSMMU_ST &&
		    RMI_RETURN_INDEX(rmi_ret) == 2) {
			/* table already exist */
			vdev->destroy = arm_realm_smmu_v3_vdevice_destroy;
			return 0;
		}
		dev_warn(dev, "failed to create realm stream mapping\n");
		return -EIO;
	}
	vdev->destroy = arm_realm_smmu_v3_vdevice_destroy;
	return 0;
}

static const struct iommufd_viommu_ops arm_realm_smmu_v3_ops = {
	.destroy = arm_realm_smmu_v3_destroy,
	.alloc_domain_nested = arm_vsmmu_alloc_domain_nested,
	.cache_invalidate = arm_vsmmu_cache_invalidate,
	.vdevice_init = arm_realm_smmu_v3_vdevice_init,
};

static int get_irq_data(int irq, u64 *msi_addr, u64 *msi_data)
{
	struct msi_desc *desc;

	desc = irq_get_msi_desc(irq);
	if (!desc)
		return -EINVAL;

	*msi_addr = (((u64)desc->msg.address_hi) << 32) | desc->msg.address_lo;
	*msi_data = desc->msg.data;
	return 0;
}

int arm_realm_smmu_v3_init(struct iommufd_viommu *viommu,
			   const struct iommu_user_data *user_data)
{
	int ret = 0;
	struct kvm *kvm;
	struct rmi_psmmu_params *params;
	struct arm_smmu_device *smmu =
		container_of(viommu->iommu_dev, struct arm_smmu_device, iommu);
	unsigned long rmi_ret;

	if (!viommu->kvm_file)
		return -EINVAL;

	kvm = viommu->kvm_file->private_data;
	if (!kvm_is_realm(kvm))
		return -EINVAL;

	if (!(smmu->features & ARM_SMMU_FEAT_RME))
		return -EOPNOTSUPP;

	if (smmu->realm_initialized)
		goto psmmu_already_active;

	params = (struct rmi_psmmu_params *)get_zeroed_page(GFP_KERNEL);
	if (!params)
		return -ENOMEM;

	/* No ATS and PRI support */
	if (!(smmu->features & ARM_SMMU_FEAT_MSI))
		goto psmmu_activate;

	params->flags = RMI_PSMMU_FLAG_MSI;
	if (get_irq_data(smmu->realm_gerr_irq,
			 &params->grr_addr, &params->grr_data)) {
		ret = -EINVAL;
		goto out_free;
	}
	if (get_irq_data(smmu->realm_evtq_irq,
			 &params->eventq_addr, &params->eventq_data)) {
		ret = -EINVAL;
		goto out_free;
	}

psmmu_activate:
	ret = rmi_psmmu_activate(smmu->base_phys, virt_to_phys(params),
				 &rmi_ret);
	if (ret || rmi_ret) {
		if (!ret)
			ret = -EIO;
		dev_warn(smmu->dev, "failed to activate realm pSMMU\n");
		ret = -EIO;
	} else {
		smmu->realm_initialized = true;
	}
out_free:
	free_page((unsigned long)params);
psmmu_already_active:
	if (!ret)
		viommu->ops = &arm_realm_smmu_v3_ops;
	return ret;
}
