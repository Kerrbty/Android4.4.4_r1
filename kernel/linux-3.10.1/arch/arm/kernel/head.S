/*
 *  linux/arch/arm/kernel/head.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Kernel startup code for all 32-bit CPUs
 *  所有32-bit CPU的内核启动代码
 */
#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/pgtable.h>

#if defined(CONFIG_DEBUG_LL) && !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif

/*
 * swapper_pg_dir 是初始页表的虚拟地址. 
 * 我们将页表放在KERNEL_RAM_VADDR以下16K的空间中. 因此我们必须保证 
 * KERNEL_RAM_VADDR已经被正常设置.当前, 我们期望的是 
 * 这个地址的最后16 bits为0x8000, 但我们或许可以放宽这项限制到 
 * KERNEL_RAM_VADDR >= PAGE_OFFSET + 0x4000. 
 */
#define KERNEL_RAM_VADDR	(PAGE_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif

#ifdef CONFIG_ARM_LPAE
	/* LPAE requires an additional page for the PGD */
#define PG_DIR_SIZE	0x5000
#define PMD_ORDER	3
#else
#define PG_DIR_SIZE	0x4000
#define PMD_ORDER	2
#endif

	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE

/*
 * TEXT_OFFSET 是内核代码（解压后）相对于RAM起始的偏移. 
 * 而#TEXT_OFFSET - 0x4000就是页表相对于RAM起始的偏移.  
 * 这个宏的作用是将phys（RAM的启示地址）加上页表的偏移， 
 * 而得到页表的起始物理地址 
 */
	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET - PG_DIR_SIZE
	.endm

/*
 * 内核启动入口点. 
 * ---------------------------
 *
 * 这个入口正常情况下是在解压完成后被调用的. 
 * 调用条件: MMU = off, D-cache = off, I-cache = dont care, r0 = 0, 
 * r1 = machine nr, r2 = atags or dtb pointer.
 * 这些条件在解压完成后会被逐一满足，然后才跳转过来。 
 *
 * 这些代码大多数是位置无关的, 如果你的内核入口地址在连接时确定为 
 * 0xc0008000, 你调用此函数的物理地址就是 __pa(0xc0008000).  
 *
 * 完整的machineID列表，请参见 linux/arch/arm/tools/mach-types 
 *
 * 我们尽量让代码简洁; 不在此处添加任何设备特定的代码 
 * - 这些特定的初始化代码是boot loader的工作(或在极端情况下， 
 * 有充分理由的情况下, 可以由zImage完成)。 
 */
	.arm

	__HEAD
ENTRY(stext)

 THUMB(	adr	r9, BSYM(1f)	)	@ Kernel is always entered in ARM.
 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
 THUMB(	.thumb			)	@ switch to Thumb now.
 THUMB(1:			)

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install
#endif
	@ 进入svc模式并且关闭中断 
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0		@ 获取处理器id-->r9 
	bl	__lookup_processor_type		@ 返回r5=procinfo r9=cpuid 
	movs	r10, r5				@ r10=r5，并可以检测r5=0?注意当前r10的值 
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p			@ yes, error 'p' 如果r5=0，则内核处理器不匹配，出错～死循环 

#ifdef CONFIG_ARM_LPAE
	mrc	p15, 0, r3, c0, c1, 4		@ read ID_MMFR0
	and	r3, r3, #0xf			@ extract VMSA support
	cmp	r3, #5				@ long-descriptor translation table format?
 THUMB( it	lo )				@ force fixup-able long branch encoding
	blo	__error_p			@ only classic page table format
#endif

/* 
 * 获取RAM的起始物理地址，并保存于 r8 = phys_offset 
 * XIP内核与普通在RAM中运行的内核不同 
 * （1）CONFIG_XIP_KERNEL 
 *        通过运行时计算？？？？ 
 * （2）正常RAM中运行的内核 
 *         通过编译时确定（PLAT_PHYS_OFFSET 一般在arch/arm/mach-xxx/include/mach/memory.h定义）
 */
#ifndef CONFIG_XIP_KERNEL
	adr	r3, 2f
	ldmia	r3, {r4, r8}
	sub	r4, r3, r4			@ (PHYS_OFFSET - PAGE_OFFSET)
	add	r8, r8, r4			@ PHYS_OFFSET
#else
	ldr	r8, =PHYS_OFFSET		@ always constant in this case
#endif

	/*
	 * r1 = machine no, r2 = atags or dtb,
	 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
	 */
	bl	__vet_atags   @ 判断r2（内核启动参数）指针的有效性 
#ifdef CONFIG_SMP_ON_UP
	bl	__fixup_smp   @ ???如果运行SMP内核在单处理器系统中启动，做适当调整 
#endif
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
	bl	__fixup_pv_table  @ ????根据内核在内存中的位置修正物理地址与虚拟地址的转换机制 
#endif
	bl	__create_page_tables  @ 初始化页表！ 

	/*
	 * 以下使用位置无关的方法调用的是CPU特定代码。 
	 * 详情请见arch/arm/mm/proc-*.S 
	 * r10 = xxx_proc_info 结构体的基地址（在上面__lookup_processor_type函数中选中的） 
	 * 返回时, CPU 已经为 MMU 的启动做好了准备, 
	 * 且 r0 保存着CPU控制寄存器的值. 
	 */
	ldr	r13, =__mmap_switched		@ 在MMU启动之后跳入的第一个虚拟地址 
						@ mmu has been enabled
	adr	lr, BSYM(1f)			@ 设置返回的地址(PIC) 
	mov	r8, r4				@ 将swapper_pg_dir的物理地址放入r8， 
							@ 以备__enable_mmu中将其放入TTBR1 
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	)  @ 跳入构架相关的初始化处理器函数(例如A8的是__v7_setup) 
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)  @主要目的只配置CP15（包括缓存配置） 
 THUMB(	mov	pc, r12				)
1:	b	__enable_mmu    @ 启动MMU 
ENDPROC(stext)
	.ltorg
#ifndef CONFIG_XIP_KERNEL
2:	.long	.
	.long	PAGE_OFFSET
#endif

/*
 * 创建初始化页表.我们只创建最基本的页表， 
 * 以满足内核运行的需要， 
 * 这通常意味着仅映射内核代码本身. 
 *
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 *
 * 返回: 
 *  r0, r3, r5-r7 corrupted
 *  r4 = 页表物理地址 
 */
__create_page_tables:
	pgtbl	r4, r8				@ 现在r4 = 页表的起始物理地址 

	/*
	 * 清零16K的一级初始页表区 
	 * 这些页表在内核自解压时被设置过 
	 * （此时MMU已关闭） 
	 */
	mov	r0, r4
	mov	r3, #0
	add	r6, r0, #PG_DIR_SIZE
1:	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	teq	r0, r6
	bne	1b

#ifdef CONFIG_ARM_LPAE
	/*
	 * Build the PGD table (first level) to point to the PMD table. A PGD
	 * entry is 64-bit wide.
	 */
	mov	r0, r4
	add	r3, r4, #0x1000			@ first PMD table address
	orr	r3, r3, #3			@ PGD block type
	mov	r6, #4				@ PTRS_PER_PGD
	mov	r7, #1 << (55 - 32)		@ L_PGD_SWAPPER
1:
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4			@ set top PGD entry bits
	str	r3, [r0], #4			@ set bottom PGD entry bits
#else
	str	r3, [r0], #4			@ set bottom PGD entry bits
	str	r7, [r0], #4			@ set top PGD entry bits
#endif
	add	r3, r3, #0x1000			@ next PMD table
	subs	r6, r6, #1
	bne	1b

	add	r4, r4, #0x1000			@ point to the PMD tables
#ifdef CONFIG_CPU_ENDIAN_BE8
	add	r4, r4, #4			@ we only write the bottom word
#endif
#endif
	/*
	 * 获取节描述符的默认配置（除节基址外的其他配置） 
	 * 这个数据依构架而不同，数据是用汇编文件配置的： 
	 * arch/arm/mm/proc-xxx.S 
	 * （此时MMU已关闭） 
	 */
	ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS] @ 获取mm_mmuflags(节描述符默认配置)，保存于r7 

	/*
	 * 创建特定映射，以满足__enable_mmu的需求。 
	 * 此特定映射将被paging_init()删除。 
	 * 
	 * 其实这个特定的映射就是仅映射__enable_mmu功能函数区的页表 
	 * 以保证在启用mmu时代码的正确执行--1:1映射（物理地址=虚拟地址） 
	 */
	adr	r0, __turn_mmu_on_loc
	ldmia	r0, {r3, r5, r6}
	sub	r0, r0, r3			@ 获取编译时确定的虚拟地址到当前物理地址的偏移 
	add	r5, r5, r0			@ __turn_mmu_on 的当前物理地址
	add	r6, r6, r0			@ __turn_mmu_on_end 的当前物理地址 
	mov	r5, r5, lsr #SECTION_SHIFT  @ __turn_mmu_on 的节基址 
	mov	r6, r6, lsr #SECTION_SHIFT  @ __enable_mmu_end 的节基址 

1:	orr	r3, r7, r5, lsl #SECTION_SHIFT	@ 生成节描述符：flags + 节基址 
	str	r3, [r4, r5, lsl #PMD_ORDER]	@ 设置节描述符,1:1映射（物理地址=虚拟地址） 
	cmp	r5, r6				@ 完成映射？（理论上一次就够了，这个函数应该不会大于1M吧～） 
	addlo	r5, r5, #1			@ r5 = 下一节的基址 
	blo	1b

	/*
	 * Map our RAM from the start to the end of the kernel .bss section.
	 */
	add	r0, r4, #PAGE_OFFSET >> (SECTION_SHIFT - PMD_ORDER)
	ldr	r6, =(_end - 1)
	orr	r3, r8, r7
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
1:	str	r3, [r0], #1 << PMD_ORDER
	add	r3, r3, #1 << SECTION_SHIFT
	cmp	r0, r6
	bls	1b

#ifdef CONFIG_XIP_KERNEL
	/*
	 * Map the kernel image separately as it is not located in RAM.
	 */
	 
	/*
	 * 现在创建内核的逻辑映射区页表（节映射）
	 * 创建范围：KERNEL_START---KERNEL_END
	 * KERNEL_START：内核最终运行的虚拟地址
	 * KERNEL_END：内核代码结束的虚拟地址（bss段之后，但XIP不是）
	 */
#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
	mov	r3, pc			@ 获取当前物理地址  
	mov	r3, r3, lsr #SECTION_SHIFT  	@ r3 = 当前物理地址的节基址 
	orr	r3, r7, r3, lsl #SECTION_SHIFT		@ r3 为当前物理地址的节描述符 
	
	
	/*
	 * 下面是为了确定页表项的入口地址
	 * 其实页表入口项的偏移就反应了对应的虚拟地址的高位
	 *
	 * 由于ARM指令集的8bit位图问题，只能分两次得到
	 * KERNEL_START：内核最终运行的虚拟地址
	 */
	add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ORDER)	
	str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ORDER]!
	ldr	r6, =(_edata_loc - 1)
	add	r0, r0, #1 << PMD_ORDER
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)	@ r6 = 内核逻辑映射结束的节基址 
1:	cmp	r0, r6
	add	r3, r3, #1 << SECTION_SHIFT		@ 生成节描述符（只需做基址递增） 
	strls	r3, [r0], #1 << PMD_ORDER		@ 设置节描述符 
	bls	1b
#endif

	/*
	 * Then map boot params address in r2 if specified.
	 * We map 2 sections in case the ATAGs/DTB crosses a section boundary.
	 * 
	 * 然后映射启动参数区（现在r2中的atags物理地址） 
	 * 或者 
	 * 如果启动参数区的虚拟地址没有确定（或者无效），则会映射RAM的头1MB. 
	 */
	mov	r0, r2, lsr #SECTION_SHIFT
	movs	r0, r0, lsl #SECTION_SHIFT
	subne	r3, r0, r8		@ 如果atags指针无效，则r0 = r8（映射RAM的头1MB） 
	addne	r3, r3, #PAGE_OFFSET	@ 转换为虚拟地址 
	addne	r3, r4, r3, lsr #(SECTION_SHIFT - PMD_ORDER)	@ 确定页表项（节描述符）入口地址 
	orrne	r6, r7, r0		@ 生成节描述符 
	strne	r6, [r3], #1 << PMD_ORDER	@ 设置节描述符 
	addne	r6, r6, #1 << SECTION_SHIFT
	strne	r6, [r3]

#if defined(CONFIG_ARM_LPAE) && defined(CONFIG_CPU_ENDIAN_BE8)
	sub	r4, r4, #4			@ Fixup page table pointer
						@ for 64-bit descriptors
#endif

/*
 * 下面是调试信息的输出函数区 
 * 这里做了IO内存空间的节映射 
 */
#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
	/*
	 * Map in IO space for serial debugging.
	 * This allows debug messages to be output
	 * via a serial console before paging_init.
	 *
	 * 为串口调试映射IO内存空间（将串口IO内存之上的所有地址都映射了） 
	 * 这允许调试信息（在paging_init之前）从串口控制台输出 
	 */
	addruart r7, r3, r0		@ 宏代码，位于arch/arm/mach-xxx/include/mach/debug-macro.S 
							@ 作用是将串口控制寄存器的基址放入r7(物理地址)和r3(虚拟地址) 
	mov	r3, r3, lsr #SECTION_SHIFT
	mov	r3, r3, lsl #PMD_ORDER

	add	r0, r4, r3		@ r0为串口IO内存映射页表项的入口地址 
	mov	r3, r7, lsr #SECTION_SHIFT	
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
	orr	r3, r7, r3, lsl #SECTION_SHIFT		@ 生成节描述符 
#ifdef CONFIG_ARM_LPAE
	mov	r7, #1 << (54 - 32)		@ XN
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4
	str	r3, [r0], #4
#else
	str	r3, [r0], #4
	str	r7, [r0], #4
#endif
#else
	orr	r3, r3, #PMD_SECT_XN
	str	r3, [r0], #4
#endif

#else /* CONFIG_DEBUG_ICEDCC || CONFIG_DEBUG_SEMIHOSTING */
	/* we don't need any serial debugging mappings */
	/* 我们无需任何串口调试映射 for ICEDCC */ 
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
#endif

#if defined(CONFIG_ARCH_NETWINDER) || defined(CONFIG_ARCH_CATS)
	/*
	 * If we're using the NetWinder or CATS, we also need to map
	 * in the 16550-type serial port for the debug messages
	 * 
	 * 如果我们在使用 NetWinder 或 CATS,我们也需要为调试信息映射 
	 * 16550-type 串口 
	 */
	add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x7c000000
	str	r3, [r0]
#endif
#ifdef CONFIG_ARCH_RPC
	/*
	 * Map in screen at 0x02000000 & SCREEN2_BASE
	 * Similar reasons here - for debug.  This is
	 * only for Acorn RiscPC architectures.
	 */
	add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x02000000
	str	r3, [r0]
	add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0]
#endif
#endif
#ifdef CONFIG_ARM_LPAE
	sub	r4, r4, #0x1000		@ point to the PGD table
#endif
	mov	pc, lr		@页表创建结束，返回 
ENDPROC(__create_page_tables)
	.ltorg
	.align
__turn_mmu_on_loc:
	.long	.
	.long	__turn_mmu_on
	.long	__turn_mmu_on_end

#if defined(CONFIG_SMP)
	__CPUINIT
ENTRY(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 *
	 * Ensure that we're in SVC mode, and IRQs are disabled.  Lookup
	 * the processor type - there is no need to check the machine type
	 * as it has already been validated by the primary processor.
	 */
#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install_secondary
#endif
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type
	movs	r10, r5				@ invalid processor?
	moveq	r0, #'p'			@ yes, error 'p'
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p

	/*
	 * Use the page tables supplied from  __cpu_up.
	 */
	adr	r4, __secondary_data
	ldmia	r4, {r5, r7, r12}		@ address to jump to after
	sub	lr, r4, r5			@ mmu has been enabled
	ldr	r4, [r7, lr]			@ get secondary_data.pgdir
	add	r7, r7, #4
	ldr	r8, [r7, lr]			@ get secondary_data.swapper_pg_dir
	adr	lr, BSYM(__enable_mmu)		@ return address
	mov	r13, r12			@ __secondary_switched address
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	) @ initialise processor
						  @ (return control reg)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	mov	pc, r12				)
ENDPROC(secondary_startup)

	/*
	 * r6  = &secondary_data
	 */
ENTRY(__secondary_switched)
	ldr	sp, [r7, #4]			@ get secondary_data.stack
	mov	fp, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)

	.align

	.type	__secondary_data, %object
__secondary_data:
	.long	.
	.long	secondary_data
	.long	__secondary_switched
#endif /* defined(CONFIG_SMP) */



/*
 * 在最后启动MMU前，设置一些常用位 Essentially 
 * 其实，这里只是加载了页表指针和域访问控制数据寄存器
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r4  = page table pointer
 *  r9  = processor ID
 *  r13 = 最后要跳入的虚拟地址 
 */
__enable_mmu:
#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
	orr	r0, r0, #CR_A
#else
	bic	r0, r0, #CR_A
#endif
#ifdef CONFIG_CPU_DCACHE_DISABLE
	bic	r0, r0, #CR_C
#endif
#ifdef CONFIG_CPU_BPREDICT_DISABLE
	bic	r0, r0, #CR_Z
#endif
#ifdef CONFIG_CPU_ICACHE_DISABLE
	bic	r0, r0, #CR_I
#endif
#ifdef CONFIG_ARM_LPAE
	mov	r5, #0
	mcrr	p15, 0, r4, r5, c2		@ load TTBR0
#else
	mov	r5, #(domain_val(DOMAIN_USER, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_TABLE, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_IO, DOMAIN_CLIENT))		@设置域访问控制数据 
	mcr	p15, 0, r5, c3, c0, 0		@ 载入域访问控制数据到DACR 
	mcr	p15, 0, r4, c2, c0, 0		@ 载入页表基址到TTBR0 
#endif
	b	__turn_mmu_on		@ 开启MMU 
ENDPROC(__enable_mmu)

/*
 * 使能 MMU.这完全改变了可见的内存地址空间结构. 
 * 您将无法通过这里跟踪执行。 
 * 如果你已对此进行探究, *请*在向邮件列表发送另一个新帖之前， 
 * 检查linux-arm-kernel的邮件列表归档 
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r9  = processor ID
 *  r13 = 最后要跳入的*虚拟*地址 
 *
 * 其他寄存器依赖上面的调用函数 
 */
	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(__turn_mmu_on)
	mov	r0, r0
	instr_sync
	mcr	p15, 0, r0, c1, c0, 0		@ 设置cp#15控制寄存器（启用MMU） 
	mrc	p15, 0, r3, c0, c0, 0		@ read id reg
	instr_sync
	mov	r3, r3
	mov	r3, r13		@ r3中装入最后要跳入的*虚拟*地址 
	mov	pc, r3		@ 跳转到__mmap_switched 
__turn_mmu_on_end:
ENDPROC(__turn_mmu_on)
	.popsection


#ifdef CONFIG_SMP_ON_UP
	__INIT
__fixup_smp:
	and	r3, r9, #0x000f0000	@ architecture version
	teq	r3, #0x000f0000		@ CPU ID supported?
	bne	__fixup_smp_on_up	@ no, assume UP

	bic	r3, r9, #0x00ff0000
	bic	r3, r3, #0x0000000f	@ mask 0xff00fff0
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000b000
	orr	r4, r4, #0x00000020	@ val 0x4100b020
	teq	r3, r4			@ ARM 11MPCore?
	moveq	pc, lr			@ yes, assume SMP

	mrc	p15, 0, r0, c0, c0, 5	@ read MPIDR
	and	r0, r0, #0xc0000000	@ multiprocessing extensions and
	teq	r0, #0x80000000		@ not part of a uniprocessor system?
	moveq	pc, lr			@ yes, assume SMP

__fixup_smp_on_up:
	adr	r0, 1f
	ldmia	r0, {r3 - r5}
	sub	r3, r0, r3
	add	r4, r4, r3
	add	r5, r5, r3
	b	__do_fixup_smp_on_up
ENDPROC(__fixup_smp)

	.align
1:	.word	.
	.word	__smpalt_begin
	.word	__smpalt_end

	.pushsection .data
	.globl	smp_on_up
smp_on_up:
	ALT_SMP(.long	1)
	ALT_UP(.long	0)
	.popsection
#endif

	.text
__do_fixup_smp_on_up:
	cmp	r4, r5
	movhs	pc, lr
	ldmia	r4!, {r0, r6}
 ARM(	str	r6, [r0, r3]	)
 THUMB(	add	r0, r0, r3	)
#ifdef __ARMEB__
 THUMB(	mov	r6, r6, ror #16	)	@ Convert word order for big-endian.
#endif
 THUMB(	strh	r6, [r0], #2	)	@ For Thumb-2, store as two halfwords
 THUMB(	mov	r6, r6, lsr #16	)	@ to be robust against misaligned r3.
 THUMB(	strh	r6, [r0]	)
	b	__do_fixup_smp_on_up
ENDPROC(__do_fixup_smp_on_up)

ENTRY(fixup_smp)
	stmfd	sp!, {r4 - r6, lr}
	mov	r4, r0
	add	r5, r0, r1
	mov	r3, #0
	bl	__do_fixup_smp_on_up
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(fixup_smp)

#ifdef CONFIG_ARM_PATCH_PHYS_VIRT

/* __fixup_pv_table - patch the stub instructions with the delta between
 * PHYS_OFFSET and PAGE_OFFSET, which is assumed to be 16MiB aligned and
 * can be expressed by an immediate shifter operand. The stub instruction
 * has a form of '(add|sub) rd, rn, #imm'.
 */
	__HEAD
__fixup_pv_table:
	adr	r0, 1f
	ldmia	r0, {r3-r5, r7}
	sub	r3, r0, r3	@ PHYS_OFFSET - PAGE_OFFSET
	add	r4, r4, r3	@ adjust table start address
	add	r5, r5, r3	@ adjust table end address
	add	r7, r7, r3	@ adjust __pv_phys_offset address
	str	r8, [r7]	@ save computed PHYS_OFFSET to __pv_phys_offset
	mov	r6, r3, lsr #24	@ constant for add/sub instructions
	teq	r3, r6, lsl #24 @ must be 16MiB aligned
THUMB(	it	ne		@ cross section branch )
	bne	__error
	str	r6, [r7, #4]	@ save to __pv_offset
	b	__fixup_a_pv_table
ENDPROC(__fixup_pv_table)

	.align
1:	.long	.
	.long	__pv_table_begin
	.long	__pv_table_end
2:	.long	__pv_phys_offset

	.text
__fixup_a_pv_table:
#ifdef CONFIG_THUMB2_KERNEL
	lsls	r6, #24
	beq	2f
	clz	r7, r6
	lsr	r6, #24
	lsl	r6, r7
	bic	r6, #0x0080
	lsrs	r7, #1
	orrcs	r6, #0x0080
	orr	r6, r6, r7, lsl #12
	orr	r6, #0x4000
	b	2f
1:	add     r7, r3
	ldrh	ip, [r7, #2]
	and	ip, 0x8f00
	orr	ip, r6	@ mask in offset bits 31-24
	strh	ip, [r7, #2]
2:	cmp	r4, r5
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	bx	lr
#else
	b	2f
1:	ldr	ip, [r7, r3]
	bic	ip, ip, #0x000000ff
	orr	ip, ip, r6	@ mask in offset bits 31-24
	str	ip, [r7, r3]
2:	cmp	r4, r5
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	mov	pc, lr
#endif
ENDPROC(__fixup_a_pv_table)

ENTRY(fixup_pv_table)
	stmfd	sp!, {r4 - r7, lr}
	ldr	r2, 2f			@ get address of __pv_phys_offset
	mov	r3, #0			@ no offset
	mov	r4, r0			@ r0 = table start
	add	r5, r0, r1		@ r1 = table size
	ldr	r6, [r2, #4]		@ get __pv_offset
	bl	__fixup_a_pv_table
	ldmfd	sp!, {r4 - r7, pc}
ENDPROC(fixup_pv_table)

	.align
2:	.long	__pv_phys_offset

	.data
	.globl	__pv_phys_offset
	.type	__pv_phys_offset, %object
__pv_phys_offset:
	.long	0
	.size	__pv_phys_offset, . - __pv_phys_offset
__pv_offset:
	.long	0
#endif

#include "head-common.S"
