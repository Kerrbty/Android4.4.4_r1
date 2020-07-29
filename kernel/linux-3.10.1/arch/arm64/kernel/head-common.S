/*
 *  linux/arch/arm/kernel/head-common.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define ATAG_CORE 0x54410001
#define ATAG_CORE_SIZE ((2*4 + 3*4) >> 2)
#define ATAG_CORE_SIZE_EMPTY ((2*4) >> 2)

#ifdef CONFIG_CPU_BIG_ENDIAN
#define OF_DT_MAGIC 0xd00dfeed
#else
#define OF_DT_MAGIC 0xedfe0dd0 /* 0xd00dfeed in big-endian */
#endif

/*
 * 异常处理.一些我们无法处理的错误. 
 * 我们应当告诉用户(这些错误信息)，但因为我们甚至无法保证是在正确的架构上运行， 
 * 所以我们什么都不做（死循环）。 
 *
 * 如果 CONFIG_DEBUG_LL 被设置，我们试图打印出错误信息， 
 * 并希望这可以对我们有帮助 (例如这对bootloader没有提供适当的处理器ID 
 * 是有帮助的). 
 */
	__HEAD

/* 确定r2（内核启动参数）指针的有效性。 The heuristic 要求 
 * 是4Byte对齐的、在物理内存的头16K中，且以ATAG_CORE标记开头。 
 * 如果选择了CONFIG_OF_FLATTREE，dtb指针也是可以接受的. 
 * 
 * 在这个函数的未来版本中 可能会对物理地址的要求更为宽松， 
 * 且如果有必要的话，可能可以移动ATAGS数据块.
 *
 * 返回值:
 *  r2 可能是有效的 atags 指针, 有效的 dtb 指针，或者0 
 *  r5, r6 被篡改 
 */
__vet_atags:
	tst	r2, #0x3			@ 是否4Byte对齐? 
	bne	1f				@ 不是则认为指针无效，返回 

	ldr	r5, [r2, #0]	@ 获取r2指向的前4Byte，用于下面测试 
#ifdef CONFIG_OF_FLATTREE
	ldr	r6, =OF_DT_MAGIC		@ is it a DTB?
	cmp	r5, r6
	beq	2f
#endif
	/*
	 * 内核启动参数块的规范是：
	 *   （wait for updata） 
	 */
	cmp	r5, #ATAG_CORE_SIZE		@ 第一个tag是ATAG_CORE吗? 测试的是tag_header中的size 
							@ 如果为ATAG_CORE，那么必为ATAG_CORE_SIZE 
	cmpne	r5, #ATAG_CORE_SIZE_EMPTY		@ 如果第一个tag的tag_header中的size为ATAG_CORE_SIZE_EMPTY 
								@ 说明此处也有atags 
	bne	1f
	ldr	r5, [r2, #4]	@ 第一个tag_header的tag（魔数） 
	ldr	r6, =ATAG_CORE		@ 获取ATAG_CORE的魔数 
	cmp	r5, r6			@ 判断第一个tag是否为ATAG_CORE 
	bne	1f			@ 不是则认为指针无效，返回 

2:	mov	pc, lr				@ atag/dtb 指针有效 

1:	mov	r2, #0
	mov	pc, lr
ENDPROC(__vet_atags)

/*
 * 以下的代码段是在MMU开启的状态下执行的, 
 * 而且使用的是绝对地址; 这不是位置无关代码. 
 *
 *  r0  = cp#15 控制寄存器值 
 *  r1  = machine ID
 *  r2  = atags/dtb pointer
 *  r9  = processor ID
 */
	__INIT
__mmap_switched:
	adr	r3, __mmap_switched_data

	ldmia	r3!, {r4, r5, r6, r7}
	cmp	r4, r5				@ 如果有必要，拷贝数据段。 
							@ 对比__data_loc和_sdata
							@ __data_loc是数据段在内核代码映像中的存储位置
							@ _sdata是数据段的链接位置（在内存中的位置）
							@ 如果是XIP技术的内核，这两个数据肯定不同

1:	cmpne	r5, r6		@ 检测数据是否拷贝完成 
	ldrne	fp, [r4], #4
	strne	fp, [r5], #4
	bne	1b

	mov	fp, #0				@ 清零 BSS 段(and zero fp) 
1:	cmp	r6, r7		@ 检测是否完成 
	strcc	fp, [r6],#4
	bcc	1b

/* 
 * 这里将需要的数据从寄存器中转移到全局变量中， 
 * 因为最后会跳入C代码，寄存器会被使用。 
 */
 ARM(	ldmia	r3, {r4, r5, r6, r7, sp})
 THUMB(	ldmia	r3, {r4, r5, r6, r7}	)
 THUMB(	ldr	sp, [r3, #16]		)
	str	r9, [r4]			@ 保存 processor ID到全局变量processor_id 
	str	r1, [r5]			@ 保存 machine type到全局变量__machine_arch_type 
	str	r2, [r6]			@ 保存 atags指针到全局变量__atags_pointer 
	cmp	r7, #0
	bicne	r4, r0, #CR_A			@ 清除cp15 控制寄存器值的 'A' bit（禁用对齐错误检查） 
	stmneia	r7, {r0, r4}			@ 保存控制寄存器值到全局变量cr_alignment(在arch/arm/kernel/entry-armv.S) 
	b	start_kernel		@ 跳入C代码（init/main.c） 
ENDPROC(__mmap_switched)

	.align	2
	.type	__mmap_switched_data, %object
__mmap_switched_data:
	.long	__data_loc			@ r4
	.long	_sdata				@ r5
	.long	__bss_start			@ r6
	.long	_end				@ r7
	.long	processor_id			@ r4
	.long	__machine_arch_type		@ r5
	.long	__atags_pointer			@ r6
#ifdef CONFIG_CPU_CP15
	.long	cr_alignment			@ r7
#else
	.long	0				@ r7
#endif
	.long	init_thread_union + THREAD_START_SP @ sp
	.size	__mmap_switched_data, . - __mmap_switched_data

/*
 * 这里提供一个 C-API 版本的 __lookup_processor_type 
 */
ENTRY(lookup_processor_type)
	stmfd	sp!, {r4 - r6, r9, lr}
	mov	r9, r0
	bl	__lookup_processor_type
	mov	r0, r5
	ldmfd	sp!, {r4 - r6, r9, pc}
ENDPROC(lookup_processor_type)

/*
 * 读取处理器ID寄存器 (CP#15, CR0), 并且查找编译时确定的处理器 
 *支持列表.注意：我们不能对__proc_info使用绝对地址， 
 * 因为我们还没有重新初始化页表（MMU已关闭，之前是解压时使用的1：1映射）。 
 * (我们不在正确的地址空间：内核是按虚拟地址（0xc00008000）编译的， 
 * 而现在我们运行在MMU关闭的情况下)。  
 * 我们必须计算偏移量。 
 *
 *	r9 = cpuid
 * Returns:
 *	r3, r4, r6 被破坏 
 *	r5 = proc_info 指针（物理地址空间） 
 *	r9 = cpuid (保留) 
 */
	__CPUINIT
__lookup_processor_type:
	adr	r3, __lookup_processor_type_data  @ 获取运行时的地址数据 
	ldmia	r3, {r4 - r6}	@ 获取编译时确定的地址数据（虚拟地址） 
	sub	r3, r3, r4			@ 获取地址偏移 virt&phys(r3) 
	add	r5, r5, r3			@ 将虚拟地址空间转换为物理地址空间 
	add	r6, r6, r3			@ r5=__proc_info_begin r6=__proc_info_end 
1:	ldmia	r5, {r3, r4}			@ 获取proc_info_list结构体中的value, mask 
	and	r4, r4, r9			@ 利用掩码处理从CP15获取的处理器ID 
	teq	r3, r4			@ 对比编译时确定的处理器ID 
	beq	2f				@ 若处理器ID匹配，返回 
	add	r5, r5, #PROC_INFO_SZ		@ 利用sizeof(proc_info_list)跳入下一个处理器ID的匹配 
	cmp	r5, r6			@ 是否已经处理完proc_info_list数据 
	blo	1b				@ 如果还有proc_info_list数据，再次检查匹配 
	mov	r5, #0				@ 否则，编译的内核与此处理器不匹配，r5 = #0 
2:	mov	pc, lr
ENDPROC(__lookup_processor_type)

/*
 * 参见 <asm/procinfo.h> 中关于 __proc_info 结构体的信息. 
 */
	.align	2
	.type	__lookup_processor_type_data, %object
__lookup_processor_type_data:
	.long	.
	.long	__proc_info_begin
	.long	__proc_info_end
	.size	__lookup_processor_type_data, . - __lookup_processor_type_data

/*
 * 处理器ID不匹配时的入口
 * 如果启用了调试信息，会从consol打印提示信息
 * 之后会进入__error的死循环
 */
__error_p:
#ifdef CONFIG_DEBUG_LL
	adr	r0, str_p1
	bl	printascii
	mov	r0, r9
	bl	printhex8
	adr	r0, str_p2
	bl	printascii
	b	__error
str_p1:	.asciz	"\nError: unrecognized/unsupported processor variant (0x"
str_p2:	.asciz	").\n"
	.align
#endif
ENDPROC(__error_p)

/*
 * 出错时的死循环入口 
 */ 
__error:
#ifdef CONFIG_ARCH_RPC
/*
 * 出错时屏幕变红 - RiscPC only. 
 */
	mov	r0, #0x02000000
	mov	r3, #0x11
	orr	r3, r3, r3, lsl #8
	orr	r3, r3, r3, lsl #16
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
#endif
1:	mov	r0, r0
	b	1b
ENDPROC(__error)
