// SPDX-License-Identifier: GPL-2.0
/*
 *
 * (C) COPYRIGHT 2014-2018, 2021-2022 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#include <linux/bitops.h>

#include <mali_kbase.h>
#include <mali_kbase_mem.h>
#include <mali_kbase_mmu_hw.h>
#include <mali_kbase_tlstream.h>
#include <backend/gpu/mali_kbase_device_internal.h>
#include <mali_kbase_as_fault_debugfs.h>

/**
 * lock_region() - Generate lockaddr to lock memory region in MMU
 * @pfn:       Starting page frame number of the region to lock
 * @num_pages: Number of pages to lock. It must be greater than 0.
 * @lockaddr:  Address and size of memory region to lock
 *
 * The lockaddr value is a combination of the starting address and
 * the size of the region that encompasses all the memory pages to lock.
 *
 * Bits 5:0 are used to represent the size, which must be a power of 2.
 * The smallest amount of memory to be locked corresponds to 32 kB,
 * i.e. 8 memory pages, because a MMU cache line is made of 64 bytes
 * and every page table entry is 8 bytes. Therefore it is not possible
 * to lock less than 8 memory pages at a time.
 *
 * The size is expressed as a logarithm minus one:
 * - A value of 14 is thus interpreted as log(32 kB) = 15, where 32 kB
 *   is the smallest possible size.
 * - Likewise, a value of 47 is interpreted as log(256 TB) = 48, where 256 TB
 *   is the largest possible size (implementation defined value according
 *   to the HW spec).
 *
 * Bits 11:6 are reserved.
 *
 * Bits 63:12 are used to represent the base address of the region to lock.
 * Only the upper bits of the address are used; lowest bits are cleared
 * to avoid confusion.
 *
 * The address is aligned to a multiple of the region size. This has profound
 * implications on the region size itself: often the MMU will lock a region
 * larger than the given number of pages, because the lock region cannot start
 * from any arbitrary address.
 *
 * Return: 0 if success, or an error code on failure.
 */
static int lock_region(u64 pfn, u32 num_pages, u64 *lockaddr)
{
	const u64 lockaddr_base = pfn << PAGE_SHIFT;
	const u64 lockaddr_end = ((pfn + num_pages) << PAGE_SHIFT) - 1;
	u64 lockaddr_size_log2;

	if (num_pages == 0)
		return -EINVAL;

	/* The MMU lock region is a self-aligned region whose size
	 * is a power of 2 and that contains both start and end
	 * of the address range determined by pfn and num_pages.
	 * The size of the MMU lock region can be defined as the
	 * largest divisor that yields the same result when both
	 * start and end addresses are divided by it.
	 *
	 * For instance: pfn=0x4F000 num_pages=2 describe the
	 * address range between 0x4F000 and 0x50FFF. It is only
	 * 2 memory pages. However there isn't a single lock region
	 * of 8 kB that encompasses both addresses because 0x4F000
	 * would fall into the [0x4E000, 0x4FFFF] region while
	 * 0x50000 would fall into the [0x50000, 0x51FFF] region.
	 * The minimum lock region size that includes the entire
	 * address range is 128 kB, and the region would be
	 * [0x40000, 0x5FFFF].
	 *
	 * The region size can be found by comparing the desired
	 * start and end addresses and finding the highest bit
	 * that differs. The smallest naturally aligned region
	 * must include this bit change, hence the desired region
	 * starts with this bit (and subsequent bits) set to 0
	 * and ends with the bit (and subsequent bits) set to 1.
	 *
	 * In the example above: 0x4F000 ^ 0x50FFF = 0x1FFFF
	 * therefore the highest bit that differs is bit #16
	 * and the region size (as a logarithm) is 16 + 1 = 17, i.e. 128 kB.
	 */
	lockaddr_size_log2 = fls64(lockaddr_base ^ lockaddr_end);

	/* Cap the size against minimum and maximum values allowed. */
	if (lockaddr_size_log2 > KBASE_LOCK_REGION_MAX_SIZE_LOG2)
		return -EINVAL;

	lockaddr_size_log2 =
		MAX(lockaddr_size_log2, KBASE_LOCK_REGION_MIN_SIZE_LOG2);

	/* Represent the result in a way that is compatible with HW spec.
	 *
	 * Upper bits are used for the base address, whose lower bits
	 * are cleared to avoid confusion because they are going to be ignored
	 * by the MMU anyway, since lock regions shall be aligned with
	 * a multiple of their size and cannot start from any address.
	 *
	 * Lower bits are used for the size, which is represented as
	 * logarithm minus one of the actual size.
	 */
	*lockaddr = lockaddr_base & ~((1ull << lockaddr_size_log2) - 1);
	*lockaddr |= lockaddr_size_log2 - 1;

	return 0;
}

static int wait_ready(struct kbase_device *kbdev,
		unsigned int as_nr)
{
	unsigned int max_loops = KBASE_AS_INACTIVE_MAX_LOOPS;
	u32 val = kbase_reg_read(kbdev, MMU_AS_REG(as_nr, AS_STATUS));

	/* Wait for the MMU status to indicate there is no active command, in
	 * case one is pending. Do not log remaining register accesses. */
	while (--max_loops && (val & AS_STATUS_AS_ACTIVE))
		val = kbase_reg_read(kbdev, MMU_AS_REG(as_nr, AS_STATUS));

	if (max_loops == 0) {
		dev_err(kbdev->dev, "AS_ACTIVE bit stuck\n");
		return -1;
	}

	/* If waiting in loop was performed, log last read value. */
	if (KBASE_AS_INACTIVE_MAX_LOOPS - 1 > max_loops)
		kbase_reg_read(kbdev, MMU_AS_REG(as_nr, AS_STATUS));

	return 0;
}

static int write_cmd(struct kbase_device *kbdev, int as_nr, u32 cmd)
{
	int status;

	/* write AS_COMMAND when MMU is ready to accept another command */
	status = wait_ready(kbdev, as_nr);
	if (status == 0)
		kbase_reg_write(kbdev, MMU_AS_REG(as_nr, AS_COMMAND), cmd);

	return status;
}

static void validate_protected_page_fault(struct kbase_device *kbdev)
{
	/* GPUs which support (native) protected mode shall not report page
	 * fault addresses unless it has protected debug mode and protected
	 * debug mode is turned on */
	u32 protected_debug_mode = 0;

	if (!kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_PROTECTED_MODE))
		return;

	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_PROTECTED_DEBUG_MODE)) {
		protected_debug_mode = kbase_reg_read(kbdev,
				GPU_CONTROL_REG(GPU_STATUS)) & GPU_DBGEN;
	}

	if (!protected_debug_mode) {
		/* fault_addr should never be reported in protected mode.
		 * However, we just continue by printing an error message */
		dev_err(kbdev->dev, "Fault address reported in protected mode\n");
	}
}

void kbase_mmu_interrupt(struct kbase_device *kbdev, u32 irq_stat)
{
	const int num_as = 16;
	const int busfault_shift = MMU_PAGE_FAULT_FLAGS;
	const int pf_shift = 0;
	const unsigned long as_bit_mask = (1UL << num_as) - 1;
	unsigned long flags;
	u32 new_mask;
	u32 tmp;

	/* bus faults */
	u32 bf_bits = (irq_stat >> busfault_shift) & as_bit_mask;
	/* page faults (note: Ignore ASes with both pf and bf) */
	u32 pf_bits = ((irq_stat >> pf_shift) & as_bit_mask) & ~bf_bits;

	KBASE_DEBUG_ASSERT(NULL != kbdev);

	/* remember current mask */
	spin_lock_irqsave(&kbdev->mmu_mask_change, flags);
	new_mask = kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_MASK));
	/* mask interrupts for now */
	kbase_reg_write(kbdev, MMU_REG(MMU_IRQ_MASK), 0);
	spin_unlock_irqrestore(&kbdev->mmu_mask_change, flags);

	while (bf_bits | pf_bits) {
		struct kbase_as *as;
		int as_no;
		struct kbase_context *kctx;
		struct kbase_fault *fault;

		/*
		 * the while logic ensures we have a bit set, no need to check
		 * for not-found here
		 */
		as_no = ffs(bf_bits | pf_bits) - 1;
		as = &kbdev->as[as_no];

		/* find the fault type */
		as->fault_type = (bf_bits & (1 << as_no)) ?
				KBASE_MMU_FAULT_TYPE_BUS :
				KBASE_MMU_FAULT_TYPE_PAGE;

		if (kbase_as_has_bus_fault(as))
			fault = &as->bf_data;
		else
			fault = &as->pf_data;

		/*
		 * Refcount the kctx ASAP - it shouldn't disappear anyway, since
		 * Bus/Page faults _should_ only occur whilst jobs are running,
		 * and a job causing the Bus/Page fault shouldn't complete until
		 * the MMU is updated
		 */
		kctx = kbasep_js_runpool_lookup_ctx(kbdev, as_no);

		/* find faulting address */
		fault->addr = kbase_reg_read(kbdev, MMU_AS_REG(as_no,
				AS_FAULTADDRESS_HI));
		fault->addr <<= 32;
		fault->addr |= kbase_reg_read(kbdev, MMU_AS_REG(as_no,
				AS_FAULTADDRESS_LO));

		/* Mark the fault protected or not */
		fault->protected_mode = kbdev->protected_mode;

		if (kbdev->protected_mode && fault->addr) {
			/* check if address reporting is allowed */
			validate_protected_page_fault(kbdev);
		}

		/* report the fault to debugfs */
		kbase_as_fault_debugfs_new(kbdev, as_no);

		/* record the fault status */
		fault->status = kbase_reg_read(kbdev, MMU_AS_REG(as_no,
				AS_FAULTSTATUS));

		if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_AARCH64_MMU)) {
			fault->extra_addr = kbase_reg_read(kbdev,
					MMU_AS_REG(as_no, AS_FAULTEXTRA_HI));
			fault->extra_addr <<= 32;
			fault->extra_addr |= kbase_reg_read(kbdev,
					MMU_AS_REG(as_no, AS_FAULTEXTRA_LO));
		}

		if (kbase_as_has_bus_fault(as)) {
			/* Mark bus fault as handled.
			 * Note that a bus fault is processed first in case
			 * where both a bus fault and page fault occur.
			 */
			bf_bits &= ~(1UL << as_no);

			/* remove the queued BF (and PF) from the mask */
			new_mask &= ~(MMU_BUS_ERROR(as_no) |
					MMU_PAGE_FAULT(as_no));
		} else {
			/* Mark page fault as handled */
			pf_bits &= ~(1UL << as_no);

			/* remove the queued PF from the mask */
			new_mask &= ~MMU_PAGE_FAULT(as_no);
		}

		/* Process the interrupt for this address space */
		spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
		kbase_mmu_interrupt_process(kbdev, kctx, as, fault);
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	}

	/* reenable interrupts */
	spin_lock_irqsave(&kbdev->mmu_mask_change, flags);
	tmp = kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_MASK));
	new_mask |= tmp;
	kbase_reg_write(kbdev, MMU_REG(MMU_IRQ_MASK), new_mask);
	spin_unlock_irqrestore(&kbdev->mmu_mask_change, flags);
}

void kbase_mmu_hw_configure(struct kbase_device *kbdev, struct kbase_as *as)
{
	struct kbase_mmu_setup *current_setup = &as->current_setup;
	u64 transcfg = 0;

	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_AARCH64_MMU)) {
		transcfg = current_setup->transcfg;

		/* Set flag AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK */
		/* Clear PTW_MEMATTR bits */
		transcfg &= ~AS_TRANSCFG_PTW_MEMATTR_MASK;
		/* Enable correct PTW_MEMATTR bits */
		transcfg |= AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK;
		/* Ensure page-tables reads use read-allocate cache-policy in
		 * the L2
		 */
		transcfg |= AS_TRANSCFG_R_ALLOCATE;

		if (kbdev->system_coherency == COHERENCY_ACE) {
			/* Set flag AS_TRANSCFG_PTW_SH_OS (outer shareable) */
			/* Clear PTW_SH bits */
			transcfg = (transcfg & ~AS_TRANSCFG_PTW_SH_MASK);
			/* Enable correct PTW_SH bits */
			transcfg = (transcfg | AS_TRANSCFG_PTW_SH_OS);
		}

		kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_TRANSCFG_LO),
				transcfg);
		kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_TRANSCFG_HI),
				(transcfg >> 32) & 0xFFFFFFFFUL);
	} else {
		if (kbdev->system_coherency == COHERENCY_ACE)
			current_setup->transtab |= AS_TRANSTAB_LPAE_SHARE_OUTER;
	}

	kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_TRANSTAB_LO),
			current_setup->transtab & 0xFFFFFFFFUL);
	kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_TRANSTAB_HI),
			(current_setup->transtab >> 32) & 0xFFFFFFFFUL);

	kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_MEMATTR_LO),
			current_setup->memattr & 0xFFFFFFFFUL);
	kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_MEMATTR_HI),
			(current_setup->memattr >> 32) & 0xFFFFFFFFUL);

	KBASE_TLSTREAM_TL_ATTRIB_AS_CONFIG(as,
			current_setup->transtab,
			current_setup->memattr,
			transcfg);

	write_cmd(kbdev, as->number, AS_COMMAND_UPDATE);
}

int kbase_mmu_hw_do_operation(struct kbase_device *kbdev, struct kbase_as *as,
		u64 vpfn, u32 nr, u32 op,
		unsigned int handling_irq)
{
	int ret;

	lockdep_assert_held(&kbdev->mmu_hw_mutex);

	if (op == AS_COMMAND_UNLOCK) {
		/* Unlock doesn't require a lock first */
		ret = write_cmd(kbdev, as->number, AS_COMMAND_UNLOCK);
	} else {
		u64 lock_addr;

		ret = lock_region(vpfn, nr, &lock_addr);
		if (!ret) {
			/* Lock the region that needs to be updated */
			kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_LOCKADDR_LO),
					lock_addr & 0xFFFFFFFFUL);
			kbase_reg_write(kbdev, MMU_AS_REG(as->number, AS_LOCKADDR_HI),
					(lock_addr >> 32) & 0xFFFFFFFFUL);
			write_cmd(kbdev, as->number, AS_COMMAND_LOCK);

			/* Run the MMU operation */
			write_cmd(kbdev, as->number, op);

			/* Wait for the flush to complete */
			ret = wait_ready(kbdev, as->number);

			if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_9630)) {
				/* Issue an UNLOCK command to ensure that valid page
				   tables are re-read by the GPU after an update.
				   Note that, the FLUSH command should perform all the
				   actions necessary, however the bus logs show that if
				   multiple page faults occur within an 8 page region
				   the MMU does not always re-read the updated page
				   table entries for later faults or is only partially
				   read, it subsequently raises the page fault IRQ for
				   the same addresses, the unlock ensures that the MMU
				   cache is flushed, so updates can be re-read.  As the
				   region is now unlocked we need to issue 2 UNLOCK
				   commands in order to flush the MMU/uTLB,
				   see PRLAM-8812.
				*/
				write_cmd(kbdev, as->number, AS_COMMAND_UNLOCK);
				write_cmd(kbdev, as->number, AS_COMMAND_UNLOCK);
			}
		}
	}

	return ret;
}

void kbase_mmu_hw_clear_fault(struct kbase_device *kbdev, struct kbase_as *as,
		enum kbase_mmu_fault_type type)
{
	unsigned long flags;
	u32 pf_bf_mask;

	spin_lock_irqsave(&kbdev->mmu_mask_change, flags);

	/*
	 * A reset is in-flight and we're flushing the IRQ + bottom half
	 * so don't update anything as it could race with the reset code.
	 */
	if (kbdev->irq_reset_flush)
		goto unlock;

	/* Clear the page (and bus fault IRQ as well in case one occurred) */
	pf_bf_mask = MMU_PAGE_FAULT(as->number);
	if (type == KBASE_MMU_FAULT_TYPE_BUS ||
			type == KBASE_MMU_FAULT_TYPE_BUS_UNEXPECTED)
		pf_bf_mask |= MMU_BUS_ERROR(as->number);

	kbase_reg_write(kbdev, MMU_REG(MMU_IRQ_CLEAR), pf_bf_mask);

unlock:
	spin_unlock_irqrestore(&kbdev->mmu_mask_change, flags);
}

void kbase_mmu_hw_enable_fault(struct kbase_device *kbdev, struct kbase_as *as,
		enum kbase_mmu_fault_type type)
{
	unsigned long flags;
	u32 irq_mask;

	/* Enable the page fault IRQ (and bus fault IRQ as well in case one
	 * occurred) */
	spin_lock_irqsave(&kbdev->mmu_mask_change, flags);

	/*
	 * A reset is in-flight and we're flushing the IRQ + bottom half
	 * so don't update anything as it could race with the reset code.
	 */
	if (kbdev->irq_reset_flush)
		goto unlock;

	irq_mask = kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_MASK)) |
			MMU_PAGE_FAULT(as->number);

	if (type == KBASE_MMU_FAULT_TYPE_BUS ||
			type == KBASE_MMU_FAULT_TYPE_BUS_UNEXPECTED)
		irq_mask |= MMU_BUS_ERROR(as->number);

	kbase_reg_write(kbdev, MMU_REG(MMU_IRQ_MASK), irq_mask);

unlock:
	spin_unlock_irqrestore(&kbdev->mmu_mask_change, flags);
}
