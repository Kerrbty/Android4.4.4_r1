/*
 * Copyright (c) 2008 Travis Geiselbrecht
 *
 * Copyright (c) 2009, Code Aurora Forum. All rights reserved.
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
#include <compiler.h>
#include <debug.h>
#include <string.h>
#include <app.h>
#include <arch.h>
#include <platform.h>
#include <target.h>
#include <lib/heap.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/dpc.h>

extern void *__ctor_list;
extern void *__ctor_end;
extern int __bss_start;
extern int _end;

static int bootstrap2(void *arg);

#if (ENABLE_NANDWRITE)
void bootstrap_nandwrite(void);
#endif

static void call_constructors(void)
{
	void **ctor;
   
	ctor = &__ctor_list;
	while(ctor != &__ctor_end) {
		void (*func)(void);

		func = (void (*)())*ctor;

		func();
		ctor++;
	}
}

/* called from crt0.S */
void kmain(void) __NO_RETURN __EXTERNALLY_VISIBLE;
void kmain(void)
{
	// 早期初始化线程池的上下文，包括运行队列、线程链表的建立等， lk架构支持多线程，但是此阶段只有一个cpu处于online，所以也只有一条代码执行路径 
	thread_init_early();

	// 架构初始化，包括DRAM,MMU初始化使能，使能协处理器， preloader运行在ISRAM，属于物理地址，
	// 而lk运行在DRAM，可以选择开启MMU或者关闭，开启MMU可以加速lk的加载过程  
	arch_early_init();   // 根据不同的平台 bootable/lk/arch/arm(x86)/arch.c 

	// 相关平台的早期初始化: 包括irq、timer，wdt，uart，led，pmic，i2c，gpio等, 初始化平台硬件，建立lk基本运行环境 ( 根据不同的平台初始化platform.c ) 
	platform_early_init();

	// 现在就一个函数跳转，初始化UART（板子相关） 
	target_early_init();

	dprintf(INFO, "welcome to lk\n\n");
	
	// 初始化构造函数  
	dprintf(SPEW, "calling constructors\n");
	call_constructors();

	// 内核堆链表上下文初始化等 
	dprintf(SPEW, "initializing heap\n");
	heap_init();

	// 线程池初始化,前提是PLATFORM_HAS_DYNAMIC_TIMER需要支持  
	dprintf(SPEW, "initializing threads\n");
	thread_init();

	// 初始化 dpc 系统 
	dprintf(SPEW, "initializing dpc\n");
	dpc_init();

	// 初始化lk中的定时器 
	dprintf(SPEW, "initializing timers\n");
	timer_init();

#if (!ENABLE_NANDWRITE)
	// 创建系统初始化工作线程,执行app初始化，lk把业务部分当成一个app.（重点） 
	dprintf(SPEW, "creating bootstrap completion thread\n");
	thread_resume(thread_create("bootstrap2", &bootstrap2, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE));

	// enable interrupts
	exit_critical_section();

	// become the idle thread
	// 初始化完成就退化成Idle线程,只有系统空闲时才运行 
	thread_become_idle();
#else
        bootstrap_nandwrite();
#endif
}

int main(void);

/*
平台相关初始化，包括nand/emmc，显现相关驱动，启动模式选择，加载logo资源检测是否DA模式，检测分区中是否有KE信息，
如果就KE信息，就从分区load 到DRAM，点亮背光，显示logo，禁止I/D-cache和MMU,跳转到DA,配置二级cache的size 获取bat电压，
判断是否低电量是否显示充电logo 
*/
static int bootstrap2(void *arg)
{
	dprintf(SPEW, "top of bootstrap2()\n");

	arch_init();

	// XXX put this somewhere else
#if WITH_LIB_BIO
	bio_init();
#endif
#if WITH_LIB_FS
	fs_init();
#endif

	//  在 acpu_clock_init 对 arm11 进行系统时钟设置，超频 
	dprintf(SPEW, "initializing platform\n");
	platform_init();
	
	// 针对硬件平台进行设置。主要对 arm9 和 arm11 的分区表进行整合，初始化flash和读取FLASH信息 
	dprintf(SPEW, "initializing target\n");
	target_init();

    // 对 LK 中所谓 app 初始化并运行起来，而 aboot_init 就将在这里开始被运行，android linux 内核的加载工作就在 aboot_init 中完成的  
	dprintf(SPEW, "calling apps_init()\n");
	apps_init(); // 调用位于 /lk/app/app.c 

	return 0;
}

#if (ENABLE_NANDWRITE)
void bootstrap_nandwrite(void)
{
	dprintf(SPEW, "top of bootstrap2()\n");

	arch_init();

	// 在 acpu_clock_init 对 arm11 进行系统时钟设置，超频 
	dprintf(SPEW, "initializing platform\n");
	platform_init();

	// 针对硬件平台进行设置。主要对 arm9 和 arm11 的分区表进行整合，初始化flash和读取FLASH信息 
	dprintf(SPEW, "initializing target\n");
	target_init(); /* lk/target/mdm9615/init.c */

	dprintf(SPEW, "calling nandwrite_init()\n");
	nandwrite_init();

	return 0;
}
#endif
