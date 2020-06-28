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
	// 初始化进程（lk 中的简单进程）相关结构体 
	thread_init_early();

	// 做一些如 关闭 cache，使能 mmu 的 arm 相关工作 
	arch_early_init();   // 根据不同的平台 bootable/lk/arch/arm(x86)/arch.c 

	// 相关平台的早期初始化( 根据不同的平台初始化platform.c ) 
	platform_early_init();

	// 现在就一个函数跳转，初始化UART（板子相关） 
	target_early_init();

	dprintf(INFO, "welcome to lk\n\n");
	
	// 构造函数相关初始化 
	dprintf(SPEW, "calling constructors\n");
	call_constructors();

	// lk系统相关的堆栈初始化 
	dprintf(SPEW, "initializing heap\n");
	heap_init();

	// 简短的初始化定时器对象 
	dprintf(SPEW, "initializing threads\n");
	thread_init();

	// lk系统控制器初始化（相关事件初始化） 
	dprintf(SPEW, "initializing dpc\n");
	dpc_init();

	// 初始化lk中的定时器 
	dprintf(SPEW, "initializing timers\n");
	timer_init();

#if (!ENABLE_NANDWRITE)
	// 新建线程入口函数 bootstrap2 用于boot 工作（重点） 
	dprintf(SPEW, "creating bootstrap completion thread\n");
	thread_resume(thread_create("bootstrap2", &bootstrap2, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE));

	// enable interrupts
	exit_critical_section();

	// become the idle thread
	thread_become_idle();
#else
        bootstrap_nandwrite();
#endif
}

int main(void);

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

	// initialize the rest of the platform
	dprintf(SPEW, "initializing platform\n");
	platform_init();
	
	// initialize the target
	dprintf(SPEW, "initializing target\n");
	target_init();

	dprintf(SPEW, "calling apps_init()\n");
	apps_init();

	return 0;
}

#if (ENABLE_NANDWRITE)
void bootstrap_nandwrite(void)
{
	dprintf(SPEW, "top of bootstrap2()\n");

	arch_init();

	// initialize the rest of the platform
	dprintf(SPEW, "initializing platform\n");
	platform_init();

	// initialize the target
	dprintf(SPEW, "initializing target\n");
	target_init();

	dprintf(SPEW, "calling nandwrite_init()\n");
	nandwrite_init();

	return 0;
}
#endif
