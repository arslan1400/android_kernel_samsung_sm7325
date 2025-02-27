// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <linux/arm-smccc.h>
#include <linux/preempt.h>
#include <linux/kvm_host.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <asm/cputype.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_host.h>

#include <kvm/arm_psci.h>

/*
 * This is an implementation of the Power State Coordination Interface
 * as described in ARM document number ARM DEN 0022A.
 */

#define AFFINITY_MASK(level)	~((0x1UL << ((level) * MPIDR_LEVEL_BITS)) - 1)

static u32 smccc_get_function(struct kvm_vcpu *vcpu)
{
	return vcpu_get_reg(vcpu, 0);
}

static unsigned long smccc_get_arg1(struct kvm_vcpu *vcpu)
{
	return vcpu_get_reg(vcpu, 1);
}

static unsigned long smccc_get_arg2(struct kvm_vcpu *vcpu)
{
	return vcpu_get_reg(vcpu, 2);
}

static unsigned long smccc_get_arg3(struct kvm_vcpu *vcpu)
{
	return vcpu_get_reg(vcpu, 3);
}

static void smccc_set_retval(struct kvm_vcpu *vcpu,
			     unsigned long a0,
			     unsigned long a1,
			     unsigned long a2,
			     unsigned long a3)
{
	vcpu_set_reg(vcpu, 0, a0);
	vcpu_set_reg(vcpu, 1, a1);
	vcpu_set_reg(vcpu, 2, a2);
	vcpu_set_reg(vcpu, 3, a3);
}

static unsigned long psci_affinity_mask(unsigned long affinity_level)
{
	if (affinity_level <= 3)
		return MPIDR_HWID_BITMASK & AFFINITY_MASK(affinity_level);

	return 0;
}

static unsigned long kvm_psci_vcpu_suspend(struct kvm_vcpu *vcpu)
{
	/*
	 * NOTE: For simplicity, we make VCPU suspend emulation to be
	 * same-as WFI (Wait-for-interrupt) emulation.
	 *
	 * This means for KVM the wakeup events are interrupts and
	 * this is consistent with intended use of StateID as described
	 * in section 5.4.1 of PSCI v0.2 specification (ARM DEN 0022A).
	 *
	 * Further, we also treat power-down request to be same as
	 * stand-by request as-per section 5.4.2 clause 3 of PSCI v0.2
	 * specification (ARM DEN 0022A). This means all suspend states
	 * for KVM will preserve the register state.
	 */
	kvm_vcpu_block(vcpu);
	kvm_clear_request(KVM_REQ_UNHALT, vcpu);

	return PSCI_RET_SUCCESS;
}

static void kvm_psci_vcpu_off(struct kvm_vcpu *vcpu)
{
	vcpu->arch.power_off = true;
	kvm_make_request(KVM_REQ_SLEEP, vcpu);
	kvm_vcpu_kick(vcpu);
}

static unsigned long kvm_psci_vcpu_on(struct kvm_vcpu *source_vcpu)
{
	struct vcpu_reset_state *reset_state;
	struct kvm *kvm = source_vcpu->kvm;
	struct kvm_vcpu *vcpu = NULL;
	unsigned long cpu_id;

	cpu_id = smccc_get_arg1(source_vcpu) & MPIDR_HWID_BITMASK;
	if (vcpu_mode_is_32bit(source_vcpu))
		cpu_id &= ~((u32) 0);

	vcpu = kvm_mpidr_to_vcpu(kvm, cpu_id);

	/*
	 * Make sure the caller requested a valid CPU and that the CPU is
	 * turned off.
	 */
	if (!vcpu)
		return PSCI_RET_INVALID_PARAMS;
	if (!vcpu->arch.power_off) {
		if (kvm_psci_version(source_vcpu, kvm) != KVM_ARM_PSCI_0_1)
			return PSCI_RET_ALREADY_ON;
		else
			return PSCI_RET_INVALID_PARAMS;
	}

	reset_state = &vcpu->arch.reset_state;

	reset_state->pc = smccc_get_arg2(source_vcpu);

	/* Propagate caller endianness */
	reset_state->be = kvm_vcpu_is_be(source_vcpu);

	/*
	 * NOTE: We always update r0 (or x0) because for PSCI v0.1
	 * the general puspose registers are undefined upon CPU_ON.
	 */
	reset_state->r0 = smccc_get_arg3(source_vcpu);

	WRITE_ONCE(reset_state->reset, true);
	kvm_make_request(KVM_REQ_VCPU_RESET, vcpu);

	/*
	 * Make sure the reset request is observed if the change to
	 * power_state is observed.
	 */
	smp_wmb();

	vcpu->arch.power_off = false;
	kvm_vcpu_wake_up(vcpu);

	return PSCI_RET_SUCCESS;
}

static unsigned long kvm_psci_vcpu_affinity_info(struct kvm_vcpu *vcpu)
{
	int i, matching_cpus = 0;
	unsigned long mpidr;
	unsigned long target_affinity;
	unsigned long target_affinity_mask;
	unsigned long lowest_affinity_level;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_vcpu *tmp;

	target_affinity = smccc_get_arg1(vcpu);
	lowest_affinity_level = smccc_get_arg2(vcpu);

	/* Determine target affinity mask */
	target_affinity_mask = psci_affinity_mask(lowest_affinity_level);
	if (!target_affinity_mask)
		return PSCI_RET_INVALID_PARAMS;

	/* Ignore other bits of target affinity */
	target_affinity &= target_affinity_mask;

	/*
	 * If one or more VCPU matching target affinity are running
	 * then ON else OFF
	 */
	kvm_for_each_vcpu(i, tmp, kvm) {
		mpidr = kvm_vcpu_get_mpidr_aff(tmp);
		if ((mpidr & target_affinity_mask) == target_affinity) {
			matching_cpus++;
			if (!tmp->arch.power_off)
				return PSCI_0_2_AFFINITY_LEVEL_ON;
		}
	}

	if (!matching_cpus)
		return PSCI_RET_INVALID_PARAMS;

	return PSCI_0_2_AFFINITY_LEVEL_OFF;
}

static void kvm_prepare_system_event(struct kvm_vcpu *vcpu, u32 type)
{
	int i;
	struct kvm_vcpu *tmp;

	/*
	 * The KVM ABI specifies that a system event exit may call KVM_RUN
	 * again and may perform shutdown/reboot at a later time that when the
	 * actual request is made.  Since we are implementing PSCI and a
	 * caller of PSCI reboot and shutdown expects that the system shuts
	 * down or reboots immediately, let's make sure that VCPUs are not run
	 * after this call is handled and before the VCPUs have been
	 * re-initialized.
	 */
	kvm_for_each_vcpu(i, tmp, vcpu->kvm)
		tmp->arch.power_off = true;
	kvm_make_all_cpus_request(vcpu->kvm, KVM_REQ_SLEEP);

	memset(&vcpu->run->system_event, 0, sizeof(vcpu->run->system_event));
	vcpu->run->system_event.type = type;
	vcpu->run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
}

static void kvm_psci_system_off(struct kvm_vcpu *vcpu)
{
	kvm_prepare_system_event(vcpu, KVM_SYSTEM_EVENT_SHUTDOWN);
}

static void kvm_psci_system_reset(struct kvm_vcpu *vcpu)
{
	kvm_prepare_system_event(vcpu, KVM_SYSTEM_EVENT_RESET);
}

static int kvm_psci_0_2_call(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	u32 psci_fn = smccc_get_function(vcpu);
	unsigned long val;
	int ret = 1;

	switch (psci_fn) {
	case PSCI_0_2_FN_PSCI_VERSION:
		/*
		 * Bits[31:16] = Major Version = 0
		 * Bits[15:0] = Minor Version = 2
		 */
		val = KVM_ARM_PSCI_0_2;
		break;
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
		val = kvm_psci_vcpu_suspend(vcpu);
		break;
	case PSCI_0_2_FN_CPU_OFF:
		kvm_psci_vcpu_off(vcpu);
		val = PSCI_RET_SUCCESS;
		break;
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN64_CPU_ON:
		mutex_lock(&kvm->lock);
		val = kvm_psci_vcpu_on(vcpu);
		mutex_unlock(&kvm->lock);
		break;
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN64_AFFINITY_INFO:
		val = kvm_psci_vcpu_affinity_info(vcpu);
		break;
	case PSCI_0_2_FN_MIGRATE_INFO_TYPE:
		/*
		 * Trusted OS is MP hence does not require migration
	         * or
		 * Trusted OS is not present
		 */
		val = PSCI_0_2_TOS_MP;
		break;
	case PSCI_0_2_FN_SYSTEM_OFF:
		kvm_psci_system_off(vcpu);
		/*
		 * We should'nt be going back to guest VCPU after
		 * receiving SYSTEM_OFF request.
		 *
		 * If user space accidently/deliberately resumes
		 * guest VCPU after SYSTEM_OFF request then guest
		 * VCPU should see internal failure from PSCI return
		 * value. To achieve this, we preload r0 (or x0) with
		 * PSCI return value INTERNAL_FAILURE.
		 */
		val = PSCI_RET_INTERNAL_FAILURE;
		ret = 0;
		break;
	case PSCI_0_2_FN_SYSTEM_RESET:
		kvm_psci_system_reset(vcpu);
		/*
		 * Same reason as SYSTEM_OFF for preloading r0 (or x0)
		 * with PSCI return value INTERNAL_FAILURE.
		 */
		val = PSCI_RET_INTERNAL_FAILURE;
		ret = 0;
		break;
	default:
		val = PSCI_RET_NOT_SUPPORTED;
		break;
	}

	smccc_set_retval(vcpu, val, 0, 0, 0);
	return ret;
}

static int kvm_psci_1_0_call(struct kvm_vcpu *vcpu)
{
	u32 psci_fn = smccc_get_function(vcpu);
	u32 feature;
	unsigned long val;
	int ret = 1;

	switch(psci_fn) {
	case PSCI_0_2_FN_PSCI_VERSION:
		val = KVM_ARM_PSCI_1_0;
		break;
	case PSCI_1_0_FN_PSCI_FEATURES:
		feature = smccc_get_arg1(vcpu);
		switch(feature) {
		case PSCI_0_2_FN_PSCI_VERSION:
		case PSCI_0_2_FN_CPU_SUSPEND:
		case PSCI_0_2_FN64_CPU_SUSPEND:
		case PSCI_0_2_FN_CPU_OFF:
		case PSCI_0_2_FN_CPU_ON:
		case PSCI_0_2_FN64_CPU_ON:
		case PSCI_0_2_FN_AFFINITY_INFO:
		case PSCI_0_2_FN64_AFFINITY_INFO:
		case PSCI_0_2_FN_MIGRATE_INFO_TYPE:
		case PSCI_0_2_FN_SYSTEM_OFF:
		case PSCI_0_2_FN_SYSTEM_RESET:
		case PSCI_1_0_FN_PSCI_FEATURES:
		case ARM_SMCCC_VERSION_FUNC_ID:
			val = 0;
			break;
		default:
			val = PSCI_RET_NOT_SUPPORTED;
			break;
		}
		break;
	default:
		return kvm_psci_0_2_call(vcpu);
	}

	smccc_set_retval(vcpu, val, 0, 0, 0);
	return ret;
}

static int kvm_psci_0_1_call(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	u32 psci_fn = smccc_get_function(vcpu);
	unsigned long val;

	switch (psci_fn) {
	case KVM_PSCI_FN_CPU_OFF:
		kvm_psci_vcpu_off(vcpu);
		val = PSCI_RET_SUCCESS;
		break;
	case KVM_PSCI_FN_CPU_ON:
		mutex_lock(&kvm->lock);
		val = kvm_psci_vcpu_on(vcpu);
		mutex_unlock(&kvm->lock);
		break;
	default:
		val = PSCI_RET_NOT_SUPPORTED;
		break;
	}

	smccc_set_retval(vcpu, val, 0, 0, 0);
	return 1;
}

/**
 * kvm_psci_call - handle PSCI call if r0 value is in range
 * @vcpu: Pointer to the VCPU struct
 *
 * Handle PSCI calls from guests through traps from HVC instructions.
 * The calling convention is similar to SMC calls to the secure world
 * where the function number is placed in r0.
 *
 * This function returns: > 0 (success), 0 (success but exit to user
 * space), and < 0 (errors)
 *
 * Errors:
 * -EINVAL: Unrecognized PSCI function
 */
static int kvm_psci_call(struct kvm_vcpu *vcpu)
{
	switch (kvm_psci_version(vcpu, vcpu->kvm)) {
	case KVM_ARM_PSCI_1_0:
		return kvm_psci_1_0_call(vcpu);
	case KVM_ARM_PSCI_0_2:
		return kvm_psci_0_2_call(vcpu);
	case KVM_ARM_PSCI_0_1:
		return kvm_psci_0_1_call(vcpu);
	default:
		return -EINVAL;
	};
}

int kvm_hvc_call_handler(struct kvm_vcpu *vcpu)
{
	u32 func_id = smccc_get_function(vcpu);
	u32 val = SMCCC_RET_NOT_SUPPORTED;
	u32 feature;

	switch (func_id) {
	case ARM_SMCCC_VERSION_FUNC_ID:
		val = ARM_SMCCC_VERSION_1_1;
		break;
	case ARM_SMCCC_ARCH_FEATURES_FUNC_ID:
		feature = smccc_get_arg1(vcpu);
		switch(feature) {
		case ARM_SMCCC_ARCH_WORKAROUND_1:
			switch (kvm_arm_harden_branch_predictor()) {
			case KVM_BP_HARDEN_UNKNOWN:
				break;
			case KVM_BP_HARDEN_WA_NEEDED:
				val = SMCCC_RET_SUCCESS;
				break;
			case KVM_BP_HARDEN_NOT_REQUIRED:
				val = SMCCC_ARCH_WORKAROUND_RET_UNAFFECTED;
				break;
			}
			break;
		case ARM_SMCCC_ARCH_WORKAROUND_2:
			switch (kvm_arm_have_ssbd()) {
			case KVM_SSBD_FORCE_DISABLE:
			case KVM_SSBD_UNKNOWN:
				break;
			case KVM_SSBD_KERNEL:
				val = SMCCC_RET_SUCCESS;
				break;
			case KVM_SSBD_FORCE_ENABLE:
			case KVM_SSBD_MITIGATED:
				val = SMCCC_RET_NOT_REQUIRED;
				break;
			}
			break;
		case ARM_SMCCC_ARCH_WORKAROUND_3:
			switch (kvm_arm_get_spectre_bhb_state()) {
			case SPECTRE_VULNERABLE:
				break;
			case SPECTRE_MITIGATED:
				val = SMCCC_RET_SUCCESS;
				break;
			case SPECTRE_UNAFFECTED:
				val = SMCCC_ARCH_WORKAROUND_RET_UNAFFECTED;
				break;
			}
			break;
		}
		break;
	default:
		return kvm_psci_call(vcpu);
	}

	smccc_set_retval(vcpu, val, 0, 0, 0);
	return 1;
}

int kvm_arm_get_fw_num_regs(struct kvm_vcpu *vcpu)
{
	return 4;		/* PSCI version and three workaround registers */
}

int kvm_arm_copy_fw_reg_indices(struct kvm_vcpu *vcpu, u64 __user *uindices)
{
	if (put_user(KVM_REG_ARM_PSCI_VERSION, uindices++))
		return -EFAULT;

	if (put_user(KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1, uindices++))
		return -EFAULT;

	if (put_user(KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2, uindices++))
		return -EFAULT;

	if (put_user(KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3, uindices++))
		return -EFAULT;

	return 0;
}

#define KVM_REG_FEATURE_LEVEL_WIDTH	4
#define KVM_REG_FEATURE_LEVEL_MASK	(BIT(KVM_REG_FEATURE_LEVEL_WIDTH) - 1)

/*
 * Convert the workaround level into an easy-to-compare number, where higher
 * values mean better protection.
 */
static int get_kernel_wa_level(u64 regid)
{
	switch (regid) {
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1:
		switch (kvm_arm_harden_branch_predictor()) {
		case KVM_BP_HARDEN_UNKNOWN:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1_NOT_AVAIL;
		case KVM_BP_HARDEN_WA_NEEDED:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1_AVAIL;
		case KVM_BP_HARDEN_NOT_REQUIRED:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1_NOT_REQUIRED;
		}
		return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1_NOT_AVAIL;
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2:
		switch (kvm_arm_have_ssbd()) {
		case KVM_SSBD_FORCE_DISABLE:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_NOT_AVAIL;
		case KVM_SSBD_KERNEL:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_AVAIL;
		case KVM_SSBD_FORCE_ENABLE:
		case KVM_SSBD_MITIGATED:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_NOT_REQUIRED;
		case KVM_SSBD_UNKNOWN:
		default:
			break;
		}
	return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_UNKNOWN;
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3:
		switch (kvm_arm_get_spectre_bhb_state()) {
		case SPECTRE_VULNERABLE:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3_NOT_AVAIL;
		case SPECTRE_MITIGATED:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3_AVAIL;
		case SPECTRE_UNAFFECTED:
			return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3_NOT_REQUIRED;
		}
		return KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3_NOT_AVAIL;
        }

	return -EINVAL;
}

int kvm_arm_get_fw_reg(struct kvm_vcpu *vcpu, const struct kvm_one_reg *reg)
{
	void __user *uaddr = (void __user *)(long)reg->addr;
	u64 val;

	switch (reg->id) {
	case KVM_REG_ARM_PSCI_VERSION:
		val = kvm_psci_version(vcpu, vcpu->kvm);
		break;
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1:
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3:
		val = get_kernel_wa_level(reg->id) & KVM_REG_FEATURE_LEVEL_MASK;
		break;
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2:
		val = get_kernel_wa_level(reg->id) & KVM_REG_FEATURE_LEVEL_MASK;

		if (val == KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_AVAIL &&
		    kvm_arm_get_vcpu_workaround_2_flag(vcpu))
			val |= KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_ENABLED;
		break;
	default:
		return -ENOENT;
	}

	if (copy_to_user(uaddr, &val, KVM_REG_SIZE(reg->id)))
		return -EFAULT;

	return 0;
}

int kvm_arm_set_fw_reg(struct kvm_vcpu *vcpu, const struct kvm_one_reg *reg)
{
	void __user *uaddr = (void __user *)(long)reg->addr;
	u64 val;
	int wa_level;

	if (copy_from_user(&val, uaddr, KVM_REG_SIZE(reg->id)))
		return -EFAULT;

	switch (reg->id) {
	case KVM_REG_ARM_PSCI_VERSION:
	{
		bool wants_02;

		wants_02 = test_bit(KVM_ARM_VCPU_PSCI_0_2, vcpu->arch.features);

		switch (val) {
		case KVM_ARM_PSCI_0_1:
			if (wants_02)
				return -EINVAL;
			vcpu->kvm->arch.psci_version = val;
			return 0;
		case KVM_ARM_PSCI_0_2:
		case KVM_ARM_PSCI_1_0:
			if (!wants_02)
				return -EINVAL;
			vcpu->kvm->arch.psci_version = val;
			return 0;
		}
		break;
	}

	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_1:
	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_3:
		if (val & ~KVM_REG_FEATURE_LEVEL_MASK)
			return -EINVAL;

		if (get_kernel_wa_level(reg->id) < val)
			return -EINVAL;

		return 0;

	case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2:
		if (val & ~(KVM_REG_FEATURE_LEVEL_MASK |
			    KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_ENABLED))
			return -EINVAL;

		wa_level = val & KVM_REG_FEATURE_LEVEL_MASK;

		if (get_kernel_wa_level(reg->id) < wa_level)
			return -EINVAL;

		/* The enabled bit must not be set unless the level is AVAIL. */
		if (wa_level != KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_AVAIL &&
		    wa_level != val)
			return -EINVAL;

		/* Are we finished or do we need to check the enable bit ? */
		if (kvm_arm_have_ssbd() != KVM_SSBD_KERNEL)
			return 0;

		/*
		 * If this kernel supports the workaround to be switched on
		 * or off, make sure it matches the requested setting.
		 */
		switch (wa_level) {
		case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_AVAIL:
			kvm_arm_set_vcpu_workaround_2_flag(vcpu,
			    val & KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_ENABLED);
			break;
		case KVM_REG_ARM_SMCCC_ARCH_WORKAROUND_2_NOT_REQUIRED:
			kvm_arm_set_vcpu_workaround_2_flag(vcpu, true);
			break;
		}

		return 0;
	default:
		return -ENOENT;
	}

	return -EINVAL;
}
