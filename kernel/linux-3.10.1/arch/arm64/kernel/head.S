/*
 * Low-level CPU initialisation
 * Based on arch/arm/kernel/head.S
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/cputype.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/virt.h>

/*
 * swapper_pg_dir is the virtual address of the initial page table. We place
 * the page tables 3 * PAGE_SIZE below KERNEL_RAM_VADDR. The idmap_pg_dir has
 * 2 pages and is placed below swapper_pg_dir.
 */
#define KERNEL_RAM_VADDR	(PAGE_OFFSET + TEXT_OFFSET)

#if (KERNEL_RAM_VADDR & 0xfffff) != 0x80000
#error KERNEL_RAM_VADDR must start at 0xXXX80000
#endif

#define SWAPPER_DIR_SIZE	(3 * PAGE_SIZE)
#define IDMAP_DIR_SIZE		(2 * PAGE_SIZE)

	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - SWAPPER_DIR_SIZE

	.globl	idmap_pg_dir
	.equ	idmap_pg_dir, swapper_pg_dir - IDMAP_DIR_SIZE

	.macro	pgtbl, ttb0, ttb1, phys
	add	\ttb1, \phys, #TEXT_OFFSET - SWAPPER_DIR_SIZE
	sub	\ttb0, \ttb1, #IDMAP_DIR_SIZE
	.endm

#ifdef CONFIG_ARM64_64K_PAGES
#define BLOCK_SHIFT	PAGE_SHIFT
#define BLOCK_SIZE	PAGE_SIZE
#else
#define BLOCK_SHIFT	SECTION_SHIFT
#define BLOCK_SIZE	SECTION_SIZE
#endif

#define KERNEL_START	KERNEL_RAM_VADDR
#define KERNEL_END	_end

/*
 * Initial memory map attributes.
 */
#ifndef CONFIG_SMP
#define PTE_FLAGS	PTE_TYPE_PAGE | PTE_AF
#define PMD_FLAGS	PMD_TYPE_SECT | PMD_SECT_AF
#else
#define PTE_FLAGS	PTE_TYPE_PAGE | PTE_AF | PTE_SHARED
#define PMD_FLAGS	PMD_TYPE_SECT | PMD_SECT_AF | PMD_SECT_S
#endif

#ifdef CONFIG_ARM64_64K_PAGES
#define MM_MMUFLAGS	PTE_ATTRINDX(MT_NORMAL) | PTE_FLAGS
#else
#define MM_MMUFLAGS	PMD_ATTRINDX(MT_NORMAL) | PMD_FLAGS
#endif

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * The requirements are:
 *   MMU = off, D-cache = off, I-cache = on or off,
 *   x0 = physical address to the FDT blob.
 *
 * This code is mostly position independent so you call this at
 * __pa(PAGE_OFFSET + TEXT_OFFSET).
 *
 * Note that the callee-saved registers are used for storing variables
 * that are useful before the MMU is enabled. The allocations are described
 * in the entry routines.
 */
	__HEAD

	/*
	 * DO NOT MODIFY. Image header expected by Linux boot-loaders.
	 */
	b	stext				// branch to kernel start, magic
	.long	0				// reserved
	.quad	TEXT_OFFSET			// Image load offset from start of RAM
	.quad	0				// reserved
	.quad	0				// reserved

ENTRY(stext)
	mov	x21, x0				// x21=FDT, bootloader传过来的设备树地址
	// 获取物理地址和虚拟地址之差 -> x28, PHYS_OFFSET -> x24 
	bl	__calc_phys_offset		// x24=PHYS_OFFSET, x28=PHYS_OFFSET-PAGE_OFFSET
	bl	el2_setup			// Drop to EL1
	// 读取cpuid -> x22，cpuid是记录在midr_el1寄存器中的 
	mrs	x22, midr_el1			// x22=cpuid
	// lookup_processor_type根据传入的w0(x0)查找体系结构相关的cpu_table，找到后返回地址给x0 
	mov	x0, x22
	bl	lookup_processor_type
	// x23存当前体系结构的cpu_table地址 
	mov	x23, x0				// x23=current cpu_table
	// 如果没找到，直接error 
	cbz	x23, __error_p			// invalid processor (x23=0)?
	bl	__vet_fdt
	/* 运行到这里之前，x0 = cpu_table, x21 = FDT, x24 = PHYS_OFFSET(0x40000000), 这个函数内部做了两张表，idmap_pg_dir 和 swapper_pg_dir。idmap_pg_dir负责恒等映射，__turn_mmu_on函数所在的2MB空间映射到这张表了，swapper_pg_dir负责正常内核的映射，整个kernel映射在这张表上。 */
	bl	__create_page_tables		// x25=TTBR0, x26=TTBR1
	/*
	 * The following calls CPU specific code in a position independent
	 * manner. See arch/arm64/mm/proc.S for details. x23 = base of
	 * cpu_info structure selected by lookup_processor_type above.
	 * On return, the CPU will be ready for the MMU to be turned on and
	 * the TCR will have been set.
	 */
	// 这个是__turn_mmu_on执行后的跳转地址，也是最后调用的函数，这个函数内部调用start_kernel. 
	ldr	x27, __switch_data		// address to jump to after
						// MMU has been enabled
	// __enable_mmu内部跳转到__turn_mmu_on，这是最后一句br x12返回的地址 
	adr	lr, __enable_mmu		// return (PIC) address
	// 找到cpu对应的CPU_INFO_SETUP函数，并调用之 
	ldr	x12, [x23, #CPU_INFO_SETUP]
	// 跳转到体系结构相关的CPU_INFO_SETUP函数 
	add	x12, x12, x28			// __virt_to_phys
	br	x12				// initialise processor
	// 这里的实际调用流程是： CPU_INFO_SETUP -> __enable_mmu -> __turn_mmu_on -> __switch_data.__mmap_switched -> start_kernel 
ENDPROC(stext)

/*
 * If we're fortunate enough to boot at EL2, ensure that the world is
 * sane before dropping to EL1.
 */
ENTRY(el2_setup)
	mrs	x0, CurrentEL
	cmp	x0, #PSR_MODE_EL2t
	ccmp	x0, #PSR_MODE_EL2h, #0x4, ne
	ldr	x0, =__boot_cpu_mode		// Compute __boot_cpu_mode
	add	x0, x0, x28
	b.eq	1f
	str	wzr, [x0]			// Remember we don't have EL2...
	ret

	/* Hyp configuration. */
1:	ldr	w1, =BOOT_CPU_MODE_EL2
	str	w1, [x0, #4]			// This CPU has EL2
	mov	x0, #(1 << 31)			// 64-bit EL1
	msr	hcr_el2, x0

	/* Generic timers. */
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3			// Enable EL1 physical timers
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr		// Clear virtual offset

	/* Populate ID registers. */
	mrs	x0, midr_el1
	mrs	x1, mpidr_el1
	msr	vpidr_el2, x0
	msr	vmpidr_el2, x1

	/* sctlr_el1 */
	mov	x0, #0x0800			// Set/clear RES{1,0} bits
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0

	/* Coprocessor traps. */
	mov	x0, #0x33ff
	msr	cptr_el2, x0			// Disable copro. traps to EL2

#ifdef CONFIG_COMPAT
	msr	hstr_el2, xzr			// Disable CP15 traps to EL2
#endif

	/* Stage-2 translation */
	msr	vttbr_el2, xzr

	/* Hypervisor stub */
	adr	x0, __hyp_stub_vectors
	msr	vbar_el2, x0

	/* spsr */
	mov	x0, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT |\
		      PSR_MODE_EL1h)
	msr	spsr_el2, x0
	msr	elr_el2, lr
	eret
ENDPROC(el2_setup)

/*
 * We need to find out the CPU boot mode long after boot, so we need to
 * store it in a writable variable.
 *
 * This is not in .bss, because we set it sufficiently early that the boot-time
 * zeroing of .bss would clobber it.
 */
	.pushsection	.data
ENTRY(__boot_cpu_mode)
	.long	BOOT_CPU_MODE_EL2
	.long	0
	.popsection

	.align	3
2:	.quad	.
	.quad	PAGE_OFFSET

#ifdef CONFIG_SMP
	.pushsection    .smp.pen.text, "ax"
	.align	3
1:	.quad	.
	.quad	secondary_holding_pen_release

	/*
	 * This provides a "holding pen" for platforms to hold all secondary
	 * cores are held until we're ready for them to initialise.
	 */
ENTRY(secondary_holding_pen)
	bl	__calc_phys_offset		// x24=phys offset
	bl	el2_setup			// Drop to EL1
	mrs	x0, mpidr_el1
	ldr     x1, =MPIDR_HWID_BITMASK
	and	x0, x0, x1
	adr	x1, 1b
	ldp	x2, x3, [x1]
	sub	x1, x1, x2
	add	x3, x3, x1
pen:	ldr	x4, [x3]
	cmp	x4, x0
	b.eq	secondary_startup
	wfe
	b	pen
ENDPROC(secondary_holding_pen)
	.popsection

ENTRY(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 */
	mrs	x22, midr_el1			// x22=cpuid
	mov	x0, x22
	bl	lookup_processor_type
	mov	x23, x0				// x23=current cpu_table
	cbz	x23, __error_p			// invalid processor (x23=0)?

	pgtbl	x25, x26, x24			// x25=TTBR0, x26=TTBR1
	ldr	x12, [x23, #CPU_INFO_SETUP]
	add	x12, x12, x28			// __virt_to_phys
	blr	x12				// initialise processor

	ldr	x21, =secondary_data
	ldr	x27, =__secondary_switched	// address to jump to after enabling the MMU
	b	__enable_mmu
ENDPROC(secondary_startup)

ENTRY(__secondary_switched)
	ldr	x0, [x21]			// get secondary_data.stack
	mov	sp, x0
	mov	x29, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)
#endif	/* CONFIG_SMP */

/*
 * Setup common bits before finally enabling the MMU. Essentially this is just
 * loading the page table pointer and vector base registers.
 *
 * On entry to this code, x0 must contain the SCTLR_EL1 value for turning on
 * the MMU.
 */
__enable_mmu:
	ldr	x5, =vectors
	msr	vbar_el1, x5
	msr	ttbr0_el1, x25			// load TTBR0
	msr	ttbr1_el1, x26			// load TTBR1
	isb
	b	__turn_mmu_on
ENDPROC(__enable_mmu)

/*
 * Enable the MMU. This completely changes the structure of the visible memory
 * space. You will not be able to trace execution through this.
 *
 *  x0  = system control register
 *  x27 = *virtual* address to jump to upon completion
 *
 * other registers depend on the function called upon completion
 */
	.align	6
__turn_mmu_on:
	msr	sctlr_el1, x0
	isb
	br	x27
ENDPROC(__turn_mmu_on)

/*
 * Calculate the start of physical memory.
 */
__calc_phys_offset:
	adr	x0, 1f
	ldp	x1, x2, [x0]
	sub	x28, x0, x1			// x28 = PHYS_OFFSET - PAGE_OFFSET
	add	x24, x2, x28			// x24 = PHYS_OFFSET
	ret
ENDPROC(__calc_phys_offset)

	.align 3
1:	.quad	.
	.quad	PAGE_OFFSET

/*
 * Macro to populate the PGD for the corresponding block entry in the next
 * level (tbl) for the given virtual address.
 *
 * Preserves:	pgd, tbl, virt
 * Corrupts:	tmp1, tmp2
 */
 /* pgd 是要在哪个pgd中创建页表项 tbl 是要写入的内容(最终&3写入) virt 是一个虚拟地址，往哪个页表项中写入，取决于这个虚拟地址 tmp1/tmp2是临时寄存器，没用。 create\_pgd\_entry的作用是修改pgd的页表项，具体哪个页表项根据virt算出来，修改为tbl & 3(代表table)。 */
	.macro	create_pgd_entry, pgd, tbl, virt, tmp1, tmp2
	// 获取pgd的index, tmp1 = virt >> PGDIR_SHIFT 右移30位 
	lsr	\tmp1, \virt, #PGDIR_SHIFT
	// tmp1 = tmp1 & 1FF 
    // 这两句实际上是tmp1 = virt[38,30],取的是pgd的index 
	and	\tmp1, \tmp1, #PTRS_PER_PGD - 1	// PGD index
	// tbl = tbl |3 这个3是代表TABLE, 表示还有下一级分页，pgd默认是table 
	orr	\tmp2, \tbl, #3			// PGD entry table type
	// pgd的一个表项8byte,将tlb & 3 写入pgd[index] 
	str	\tmp2, [\pgd, \tmp1, lsl #3]
	.endm

/*
 * Macro to populate block entries in the page table for the start..end
 * virtual range (inclusive).
 *
 * Preserves:	tbl, flags
 * Corrupts:	phys, start, end, pstate
 */
 /* tbl: 一个二级页表(pmd)页表的地址，这里是向二级页表项写入内容(pud为空) flags: 每个映射页的附加属性 phys: 被映射到的起始物理地址 start: 映射的起始虚拟地址 end: 映射的结束虚拟地址 idmap: 是否为恒等映射 create_block_map是将虚拟地址[start,end]映射到物理地址phys开始的内存，映射大小必须在一个二级页表范围内(1GB)，每个二级页表项附加属性flags,如果是恒等映射(idmap=1)则忽略start，直接映射[phys,end]。 */
	.macro	create_block_map, tbl, flags, phys, start, end, idmap=0
	//phys = phys >> BLOCK_SHIFT(21)
	lsr	\phys, \phys, #BLOCK_SHIFT
	// 这里的目的是获取虚拟地址start 对应的pmd的index，正常情况下应该是(start >> 21) & 0x1ff (只取start[29,21]),如果是恒等映射(idmap = 1)，则start应该=phys，所以直接取phys[29,21]就行 
	.if	\idmap
	// PTRS_PER_PTE = 0x200, start = phys & 0x1ff 
	and	\start, \phys, #PTRS_PER_PTE - 1	// table index
	.else
	// 非恒等映射，取start[29,21] 
	lsr	\start, \start, #BLOCK_SHIFT
	and	\start, \start, #PTRS_PER_PTE - 1	// table index
	.endif
	
	// phys = phys << 21 | flags (这个物理地址 & 属性标志，作为页表项内容) 
	orr	\phys, \flags, \phys, lsl #BLOCK_SHIFT	// table entry
	// 这里实际上是判断start和end是否是同一个寄存器，除非同一个寄存器，否则start都移位了，不可能等于end。 
	.ifnc	\start,\end
	// end = end >> 21 
	lsr	\end, \end, #BLOCK_SHIFT
	// end & = 0x1ff ，就是end = end[29,21]; 
	and	\end, \end, #PTRS_PER_PTE - 1		// table end index
	.endif
	//tbl[start * 8] = phys (这时候的start已经作为index了)
9999:	str	\phys, [\tbl, \start, lsl #3]		// store the entry
	//如果start != end
	.ifnc	\start,\end
	// start ++; (index++) 
	add	\start, \start, #1			// next entry
	// phys += 2MB; 
	add	\phys, \phys, #BLOCK_SIZE		// next block
	cmp	\start, \end
	// 映射下一个pmd 
	b.ls	9999b
	.endif
	.endm

/*
 * Setup the initial page tables. We only setup the barest amount which is
 * required to get the kernel running. The following sections are required:
 *   - identity mapping to enable the MMU (low address, TTBR0)
 *   - first few MB of the kernel linear mapping to jump to once the MMU has
 *     been enabled, including the FDT blob (TTBR1)
 *   - UART mapping if CONFIG_EARLY_PRINTK is enabled (TTBR1)
 */
__create_page_tables:
	/* pgtbl定义如下: .macro pgtbl, ttb0, ttb1, phys add \ttb1, \phys, #TEXT_OFFSET - SWAPPER_DIR_SIZE sub \ttb0, \ttb1, #IDMAP_DIR_SIZE .endm 在内核初始化的时候，kernel是放在PAGE_OFFSET + TEXT_OFFSET的位置,而idmap_pg_dir,swapper_pg_dir这两张表是固定紧挨着kernel，在kernel前面的。这句宏是根据内核载入的物理基地址PHYS_OFFSET(x24)，计算idmap_pg_dir,swapper_pg_dir的物理地址分别 -> x25, x26 (在__create_page_tables返回后，开启mmu之前，x25的值会赋值给ttb0，x26的值会赋值给ttb1)。 */
	pgtbl	x25, x26, x24			// idmap_pg_dir and swapper_pg_dir addresses

	/*
	 * Clear the idmap and swapper page tables.
	 */
	/* 将[idmap_pg_dir, idmap_pg_dir + SWAPPER_DIR_SIZE + IDMAP_DIR_SIZE] 全部清空，这里面不止包含idmap_pg_dir, swapper_pg_dir两张表，具体的结构如下: PA=idmap_pg_dir:------> idmap_pg_dir PA+=0x000001000:------> idmap_pg_dir[x]'s tbl(idm_tbl) PA+=0x000001000:------> swapper_pg_dir PA+=0x000001000:------> swapper_pg_dir[x]'s tbl(swap_tbl) PA+=0x000001000:------> kernel（PA=PHYS_OFFSET + TEXT_OFFSET）: idmap_pg_dir是一张恒等映射表，对应arm的一级页表，idm_tbl是一个二级页表。 swapper_pg_dir是系统正常运行时的一份内核页表，所有进程页表的kernel部分都是从这里复制出来的，swap_tbl是其的一个二级页表 最后空的1MB不知道是干嘛的. arm64的一个页表项为8byte,idmap_pg_dir大小4KB, 4KB/8byte = 512(0x200)个页表项。每个页表代表的地址空间就是PGDIR_SIZE(1GB)，一个idmap_pg_dir理论上能代表512G地址空间(刚好是三级寻址空间大小，上一篇提到过)。 在系统初始化的时候只为idmap_pg_dir预留了一个二级页表，即swap_tbl，可代表1GB空间(其余的idmap_pg_dir作为一级页表项，英应该可以用block的方式填充)。 swap_tbl同理 4KB/8byte=512(0x200)个页表项,每隔页表项代表PMD_SIZE(2MB)的地址空间，512*2MB=1GB空间，由于没有预留pte三级页表，所以在开始阶段swap_tbl想要初始化，估计也只能初始化为block。 swapper_pg_dir与idmap_pg_dir类似。 */
	mov	x0, x25
	// x6 = 内核起始位置 
	add	x6, x26, #SWAPPER_DIR_SIZE
	// 循环将idmap_pg_dir到内核开始位置都清零。 
1:	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	stp	xzr, xzr, [x0], #16
	cmp	x0, x6
	// 如果x0 < x6,则jmp 1 
	b.lo	1b

	// x7 = 0x711 （0,1,2级页描述符的访问属性是通用的,0x711最后一位1表示用的是段映射(SECT/block),没有下一级页表了,初始化的时候这是默认属性。
	ldr	x7, =MM_MMUFLAGS

	/*
	 * Create the identity mapping.
	 */
	/* 以idmap_pg_dir为基址，创建恒等映射表, 在恒等映射表中，虚拟地址就等于物理地址。恒等映射表是为开启mmu的代码准备的，在当前内核中相关的代码有: __turn_mmu_on(), cpu_resume_mmu() and cpu_reset()。以__enable_mmu为例，其代码如下: __turn_mmu_on: msr sctlr_el1, x0 //开启mmu isb //指令同步屏障 br x27 ENDPROC(__turn_mmu_on) 这段代码所在的页，在恒等映射表中必须要有。因为在msr指令开启mmu之后，isb指令打断了指令流水，下一条指令br执行的时候会先根据pc+4查找指令地址，此时的pc是一个物理地址，pc+4是br x27这条指令的物理地址。内核在开启mmu之前会将恒等映射表存入TTBR0, swapper_pg_dir表存入TTBR1，br x27指令最终会到恒等映射表上寻找物理地址，恒等映射表物理地址等于虚拟地址，从而保证这条指令的正常执行。 在内核中__turn_mmu_on(), cpu_resume_mmu() and cpu_reset()的代码一般都在同一个2MB页上，所以这里实际上映射__turn_mmu_on一个函数就够了，如果内核中有其他enable mmu的函数，则也需要加到恒等映射表中。 */
    // x0 = idm_tlb，二级页表地址。
	add	x0, x25, #PAGE_SIZE		// section table address
	// 获取__turn_mmu_on函数的物理地址（adr是基于pc寻址，由于当前没有开启mmu，pc就是物理地址，所以adr寻址后的函数也是物理地址） 
	adr	x3, __turn_mmu_on		// virtual/physical address
	// pgd = x25, tbl = x0, virt = x3。这个宏用来修改pgd的页表项，具体哪个页表项根据virt算出来，修改为tbl & 3(代表table)。这里的x3传入的是__turn_mmu_on的物理地址。 
	create_pgd_entry x25, x0, x3, x5, x6
	/*  idmap=1表示这里做恒等映射，x0=idm_tbl地址，x7为附加属性,x3为映射的起始物理地址(这个不一定是对齐的),x5这里实际上没用上，恒等映射不用start。end和start一个寄存器。这里实际上只是对x3(__turn_mmu_on)所在的2MB做了恒等映射 */
	create_block_map x0, x7, x3, x5, x5, idmap=1

	/*
	 * Map the kernel image (starting with PHYS_OFFSET).
	 */
	/* 对内核做非恒等映射，内核起始虚拟地址是PAGE_OFFSET，映射到物理地址PHYS_OFFSET，大小为整个内核大小，用的pgd为swapper_pg_dir。 */
    // x0为swap_tbl的地址 
	add	x0, x26, #PAGE_SIZE		// section table address
	// 映射的起始虚拟地址 
	mov	x5, #PAGE_OFFSET
	// pgd = x26, tbl = x0, virt = x5，修改swapper_pg_dir中某个pgd页表项的值，为 tbl &3, 哪个pgd页是由virt来决定的。 
	create_pgd_entry x26, x0, x5, x3, x6
	// 映射的结束位置 
	ldr	x6, =KERNEL_END - 1
	// 映射的起始物理地址PHYS_OFFSET 
	mov	x3, x24				// phys offset
	// 循环将内核整个映射到swapper_pg_dir 
	create_block_map x0, x7, x3, x5, x6

	/*
	 * Map the FDT blob (maximum 2MB; must be within 512MB of
	 * PHYS_OFFSET).
	 */
	// 对FDT的映射 
	mov	x3, x21				// FDT phys address
	and	x3, x3, #~((1 << 21) - 1)	// 2MB aligned
	mov	x6, #PAGE_OFFSET
	sub	x5, x3, x24			// subtract PHYS_OFFSET
	tst	x5, #~((1 << 29) - 1)		// within 512MB?
	csel	x21, xzr, x21, ne		// zero the FDT pointer
	b.ne	1f
	add	x5, x5, x6			// __va(FDT blob)
	add	x6, x5, #1 << 21		// 2MB for the FDT blob
	sub	x6, x6, #1			// inclusive range
	create_block_map x0, x7, x3, x5, x6
1:
#ifdef CONFIG_EARLY_PRINTK
	/*
	 * Create the pgd entry for the UART mapping. The full mapping is done
	 * later based earlyprintk kernel parameter.
	 */
	ldr	x5, =EARLYCON_IOBASE		// UART virtual address
	add	x0, x26, #2 * PAGE_SIZE		// section table address
	create_pgd_entry x26, x0, x5, x6, x7
#endif
	ret
ENDPROC(__create_page_tables)
	.ltorg

	.align	3
	.type	__switch_data, %object
__switch_data:
	.quad	__mmap_switched
	.quad	__data_loc			// x4
	.quad	_data				// x5
	.quad	__bss_start			// x6
	.quad	_end				// x7
	.quad	processor_id			// x4
	.quad	__fdt_pointer			// x5
	.quad	memstart_addr			// x6
	.quad	init_thread_union + THREAD_START_SP // sp

/*
 * The following fragment of code is executed with the MMU on in MMU mode, and
 * uses absolute addresses; this is not position independent.
 */
__mmap_switched:
	adr	x3, __switch_data + 8

	ldp	x4, x5, [x3], #16
	ldp	x6, x7, [x3], #16
	cmp	x4, x5				// Copy data segment if needed
1:	ccmp	x5, x6, #4, ne
	b.eq	2f
	ldr	x16, [x4], #8
	str	x16, [x5], #8
	b	1b
2:
1:	cmp	x6, x7
	b.hs	2f
	str	xzr, [x6], #8			// Clear BSS
	b	1b
2:
	ldp	x4, x5, [x3], #16
	ldr	x6, [x3], #8
	ldr	x16, [x3]
	mov	sp, x16
	str	x22, [x4]			// Save processor ID
	str	x21, [x5]			// Save FDT pointer
	str	x24, [x6]			// Save PHYS_OFFSET
	mov	x29, #0
	b	start_kernel
ENDPROC(__mmap_switched)

/*
 * Exception handling. Something went wrong and we can't proceed. We ought to
 * tell the user, but since we don't have any guarantee that we're even
 * running on the right architecture, we do virtually nothing.
 */
__error_p:
ENDPROC(__error_p)

__error:
1:	nop
	b	1b
ENDPROC(__error)

/*
 * This function gets the processor ID in w0 and searches the cpu_table[] for
 * a match. It returns a pointer to the struct cpu_info it found. The
 * cpu_table[] must end with an empty (all zeros) structure.
 *
 * This routine can be called via C code and it needs to work with the MMU
 * both disabled and enabled (the offset is calculated automatically).
 */
ENTRY(lookup_processor_type)
	adr	x1, __lookup_processor_type_data
	ldp	x2, x3, [x1]
	sub	x1, x1, x2			// get offset between VA and PA
	add	x3, x3, x1			// convert VA to PA
1:
	ldp	w5, w6, [x3]			// load cpu_id_val and cpu_id_mask
	cbz	w5, 2f				// end of list?
	and	w6, w6, w0
	cmp	w5, w6
	b.eq	3f
	add	x3, x3, #CPU_INFO_SZ
	b	1b
2:
	mov	x3, #0				// unknown processor
3:
	mov	x0, x3
	ret
ENDPROC(lookup_processor_type)

	.align	3
	.type	__lookup_processor_type_data, %object
__lookup_processor_type_data:
	.quad	.
	.quad	cpu_table
	.size	__lookup_processor_type_data, . - __lookup_processor_type_data

/*
 * Determine validity of the x21 FDT pointer.
 * The dtb must be 8-byte aligned and live in the first 512M of memory.
 */
__vet_fdt:
	tst	x21, #0x7
	b.ne	1f
	cmp	x21, x24
	b.lt	1f
	mov	x0, #(1 << 29)
	add	x0, x0, x24
	cmp	x21, x0
	b.ge	1f
	ret
1:
	mov	x21, #0
	ret
ENDPROC(__vet_fdt)
