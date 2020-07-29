/*
 * Copyright (c) 2008 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define DSB .byte 0x4f, 0xf0, 0x7f, 0xf5
#define ISB .byte 0x6f, 0xf0, 0x7f, 0xf5

.section ".text.boot"
.globl _start

/* NOR FLASH 启动入口 */ 
/*
 初始化cpu,设置 irq, fiq, abort, undefined,system/user堆栈, 设置管理模式。
然后通过"bl  kmain"跳转到main.c中执行 
*/
_start:
	/* 从0开始位置是一个中断向量表(8x4连续字节) */
	/* arm处理器从0位置开始执行.需要将协处理CP15中的c1控制寄存器的中的V位配为0 */
	b	reset            /* 复位 */
 	b	arm_undefined    /* 未定义指令中断 */
	b	arm_syscall      /* 软件中断(SWI) */
 	b	arm_prefetch_abort  /* 预取指令中止 */
	b	arm_data_abort   /* 数据访问终止 */
	b	arm_reserved     /* 保留 */
	b	arm_irq          /* 外部中断请求(IRQ) */
	b	arm_fiq          /* 快速中断请求(FIQ) */

reset:

#ifdef ENABLE_TRUSTZONE 
	/*Add reference to TZ symbol so linker includes it in final image */
	ldr r7, =_binary_tzbsp_tzbsp_bin_start
#endif
	/* do some cpu setup */
#if ARM_WITH_CP15
        /* Read SCTLR */
	mrc		p15, 0, r0, c1, c0, 0 /* MCR/MRC{cond} P15,opcode_1,Rd,CRn,CRm,opcode_2 读取cp15协处理器的寄存器1（Control register）的值到通用寄存器r0中 */
		/* XXX this is currently for arm926, revist with armv6 cores */
		/* new thumb behavior, low exception vectors, i/d cache disable, mmu disabled */
	bic		r0, r0, #(1<<15| 1<<13 | 1<<12)  /* 位清除指令，Low addresses = 0x00000000, ICache disabled */
	bic		r0, r0, #(1<<2 | 1<<0)  /* DCache disabled, MMU disabled */
		/* enable alignment faults */
	orr		r0, r0, #(1<<1)  /* Data address alignment fault checking disabled */
        /* Write SCTLR */
	mcr		p15, 0, r0, c1, c0, 0  /* 将设置好的r0的值写入到p15的寄存器1中，完成关闭MMU，禁用ICahce和DCache等 */
#ifdef ENABLE_TRUSTZONE
  /*nkazi: not needed ? Setting VBAR to location of new vector table : 0x80000      */
 ldr             r0, =0x00080000
 mcr             p15, 0, r0, c12, c0, 0
#endif
#endif

#if WITH_CPU_EARLY_INIT
	/* call platform/arch/etc specific init code */
#ifndef ENABLE_TRUSTZONE
	/* Not needed when TrustZone is the first bootloader that runs.*/
	bl __cpu_early_init
#endif
	/* declare return address as global to avoid using stack */
.globl _cpu_early_init_complete
	_cpu_early_init_complete:

#endif

#if (!ENABLE_NANDWRITE)
#if WITH_CPU_WARM_BOOT
	ldr 	r0, warm_boot_tag
	cmp 	r0, #1

	/* if set, warm boot */
	ldreq 	pc, =BASE_ADDR

	mov 	r0, #1
	str	r0, warm_boot_tag
#endif
#endif

	/* see if we need to relocate */
	/* 判断是否需要代码重定位 */
	mov		r0, pc
	sub		r0, r0, #(.Laddr - _start) /* 计算_start所在内存地址 */
.Laddr:
	ldr		r1, =_start  /* 编译时候_start希望加载的地址 */
	cmp		r0, r1
	beq		.Lstack_setup

	/* we need to relocate ourselves to the proper spot */
	/* 与需要加载的地址不相同，那么将代码拷贝到需要加载的地址，并跳转过去继续执行之后的.Lstack_setup */
	ldr		r2, =__data_end	 /* 同目录下的 system-onesegment.ld */

.Lrelocate_loop:
	ldr		r3, [r0], #4
	str		r3, [r1], #4
	cmp		r1, r2
	bne		.Lrelocate_loop

	/* wea are relocated, jump to the right address */
	ldr		r0, =.Lstack_setup /* 跳转到新地址的.Lstack_setup 位置 */
	bx		r0

.ltorg
#if WITH_CPU_WARM_BOOT
warm_boot_tag:
	.word 0
#endif

.Lstack_setup:
	/* set up the stack for irq, fiq, abort, undefined, system/user, and lastly supervisor mode */
	mrs     r0, cpsr
	bic     r0, r0, #0x1f  /*  清除处理器模式位M[4:0] */

	ldr		r2, =abort_stack_top  /* 将abort_stack_top的地址赋给r2 */
	orr     r1, r0, #0x12 // irq 0b10010
	msr     cpsr_c, r1    /* 设置irq模式 */
	ldr		r13, =irq_save_spot		/* save a pointer to a temporary dumping spot used during irq delivery 将全局符号irq_save_spot的地址赋给r13 */
	    
	orr     r1, r0, #0x11 // fiq 0b10001 
	msr     cpsr_c, r1   /* 设置fiq模式 */
	mov		sp, r2 /* 设置fiq模式的堆栈 */
	            
	orr     r1, r0, #0x17 // abort 0b10111 
	msr     cpsr_c, r1    /* 设置abort模式 */
	mov		sp, r2 /* 设置abort模式的堆栈 */
	    
	orr     r1, r0, #0x1b // undefined 0b11011 
	msr     cpsr_c, r1  /*  设置undefined模式 */
	mov		sp, r2 /* 设置undefined模式的堆栈 */
	    
	orr     r1, r0, #0x1f // system 0b11111 
	msr     cpsr_c, r1  /* 设置system模式 */
	mov		sp, r2 /* 设置system模式的堆栈 */

	orr		r1, r0, #0x13 // supervisor 0b10011 
	msr		cpsr_c, r1 /* 设置supervisor模式 */
	mov		sp, r2 /* 设置supervisor模式的堆栈 */

	/* copy the initialized data segment out of rom if necessary */
	ldr		r0, =__data_start_rom   /* 同目录下的 system-onesegment.ld */
	ldr		r1, =__data_start
	ldr		r2, =__data_end

        /* 比较__data_start_rom和__data_start的内存地址是否相等，如果相等则跳转到.L__do_bss处 */
	cmp		r0, r1 
	beq		.L__do_bss 

        /* 移动代码到指定地址 */
.L__copy_loop:
	cmp		r1, r2
	ldrlt	r3, [r0], #4
	strlt	r3, [r1], #4
	blt		.L__copy_loop

        /* 进行bss段清零 */
.L__do_bss:
	/* clear out the bss */
	ldr		r0, =__bss_start
	ldr		r1, =_end
	mov		r2, #0
.L__bss_loop:
	cmp		r0, r1
	strlt	r2, [r0], #4
	blt		.L__bss_loop

#ifdef ARM_CPU_CORTEX_A8
	DSB
	ISB
#endif

	bl		kmain   /* 跳转到C语言函数里面继续执行lk/kernel/main.c */
	b		.   /* 执行到这里就死循环 */

.ltorg

.bss
.align 2
	/* the abort stack is for unrecoverable errors.
	 * also note the initial working stack is set to here.
	 * when the threading system starts up it will switch to a new 
	 * dynamically allocated stack, so we do not need it for very long
	 */
abort_stack: 
	.skip 1024  /* 异常堆栈的大小1024字节 */
abort_stack_top:

.rodata:
.align 2

/* define the heap end as read-only data containing the end defined in the
 * linker script. other archs that use dynamic memory length discovery can make
 * this read-write and update it during init.
 */
.global _heap_end
_heap_end:
	.int _end_of_ram
