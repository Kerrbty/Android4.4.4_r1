/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org> 
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/tty.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_cgroup.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/kmemcheck.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/file.h>
#include <linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#ifdef CONFIG_X86_LOCAL_APIC
#include <asm/smp.h>
#endif

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(unsigned long);
extern void mca_init(void);
extern void sbus_init(void);
extern void radix_tree_init(void);
#ifndef CONFIG_DEBUG_RODATA
static inline void mark_rodata_ro(void) { }
#endif

#ifdef CONFIG_TC
extern void tc_init(void);
#endif

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);
extern void softirq_init(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situaiton where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char * argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char * envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static int __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	int had_early_param = 0;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = 1;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return 1;
			} else if (p->setup_func(line + n))
				return 1;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);

EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = 10;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = 4;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val, const char *unused)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val, const char *unused)
{
	repair_env_string(param, val, unused);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "Too many boot env vars at `%s'";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "Too many boot init vars at `%s'";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
#ifdef CONFIG_X86_LOCAL_APIC
static void __init smp_init(void)
{
	APIC_init_uniprocessor();
}
#else
#define smp_init()	do { } while (0)
#endif

static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	saved_command_line = alloc_bootmem(strlen (boot_command_line)+1);
	static_command_line = alloc_bootmem(strlen (command_line)+1);
	strcpy (saved_command_line, boot_command_line);
	strcpy (static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __init_refok rest_init(void)
{
	int pid;

	rcu_scheduler_starting();  // 内核RCU锁机制调度启动,因为下面就要用到 
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 * 
	 * 我们必须先创建init内核线程，这样它就可以获得pid为1。
	 * 尽管如此init线程将会挂起来等待创建kthreads线程。
	 * 如果我们在创建kthreadd线程前调度它，就将会出现OOPS。 
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);  // 创建内核初始化线程 
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES); // 创建"内核线程的管理者"线程 
	
	/* 
	 * 1.创建kthreadd内核线程，它的作用是管理和调度其它内核线程。
	 * 2.它循环运行一个叫做kthreadd的函数，该函数的作用是运行kthread_create_list全局链表中维护的内核线程。
	 * 3.调用kthread_create创建一个kthread，它会被加入到kthread_create_list 链表中；
	 * 4.被执行过的kthread会从kthread_create_list链表中删除；
	 * 5.且kthreadd会不断调用scheduler函数让出CPU。此线程不可关闭。
	 * 
	 * 上面两个线程就是我们平时在Linux系统中用ps命令看到：
	 * $ ps -A
	 * PID TTY TIME CMD
	 * 3.1 ? 00:00:00 init
	 * 4.2 ? 00:00:00 kthreadd
	 */
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	complete(&kthreadd_done);

	/* 
	 * 1.获取kthreadd的线程信息，获取完成说明kthreadd已经创建成功。并通过一个
	 * complete变量（kthreadd_done）来通知kernel_init线程。
	 */ 
	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val, const char *unused)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static __initdata int done = 0;
	static __initdata char tmp_cmdline[COMMAND_LINE_SIZE];

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

/*
 *	Activate the first processor.
 */

static void __init boot_cpu_init(void)
{
	int cpu = smp_processor_id();
	/* Mark the boot cpu "present", "online" etc for SMP and UP case */
	set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);
}

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_info_cache_init(void)
{
}
#endif

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_cgroup requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	page_cgroup_init_flatmem();
	mem_init();
	kmem_cache_init();
	percpu_init_late();
	pgtable_cache_init();
	vmalloc_init();
}

// 构架无关的内核C语言代码入口 
// 在这个函数中Linux内核开始真正进入初始化阶段 
// 进行一系列与内核相关的初始化后，调用第一个用户进程－init 进程并等待用户进程的执行，这样整个 Linux 内核便启动完毕。 
/*
 * 函数处理归纳如下： 
 *  1.内核启动参数的获取和处理
 *  2.setup_arch(&command_line);函数
 *  3.内存管理的初始化（从bootmem到slab）
 *  4.各种内核体系的初始化
 *  5.rest_init();函数
 */
asmlinkage void __init start_kernel(void)
{
	char * command_line;
	extern const struct kernel_param __start___param[], __stop___param[];
	/*这两个变量为地址指针，指向内核启动参数处理相关结构体段在内存中的位置（虚拟地址）。
	声明传入参数的外部参数对于ARM平台，位于 include\asm-generic\vmlinux.lds.h*/

	/*
	 * Need to run as early as possible, to initialize the
	 * lockdep hash:
	 * 
	 * lockdep是一个内核调试模块，用来检查内核互斥机制（尤其是自旋锁）潜在的死锁问题。
	 */
	lockdep_init();  // 初始化内核依赖的关系表，初始化hash表 
	smp_setup_processor_id();	// 获取当前CPU,单处理器为空 
	debug_objects_early_init();	// 对调试对象进行早期的初始化,其实就是HASH锁和静态对象池进行初始化 

	/*
	 * Set up the the initial canary ASAP:
	 * 
	 * 初始化栈canary值
	 * canary值的是用于防止栈溢出攻击的堆栈的保护字 。 
	 */
	boot_init_stack_canary();

	/* 1.cgroup: 它的全称为control group.即一组进程的行为控制.  
	 * 2.该函数主要是做数据结构和其中链表的初始化  
	 * 3.参考资料： Linux cgroup机制分析之框架分析 
	 */
	cgroup_init_early();

	local_irq_disable(); //关闭系统总中断（底层调用汇编指令） 
	early_boot_irqs_disabled = true;

/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
	boot_cpu_init(); // 1.激活当前CPU（在内核全局变量中将当前CPU的状态设为激活状态） 
	page_address_init();	// 高端内存相关，未定义高端内存的话为空函数 
	pr_notice("%s", linux_banner);
	
	/*
	 * 与体系结构相关的第一个初始化工作
	 *
	 * 对不同的体系结构来说该函数有不同的定义。
	 * 对于 ARM 平台而言，该函数定义在arch/arm/kernel/Setup.c。
	 * 它首先通过检测出来的处理器类型进行处理器内核的初始化，
	 * 然后通过 bootmem_init()函数根据系统定义的 meminfo 结构进行内存结构的初始化，
	 * 最后调用paging_init()开启 MMU，创建内核页表，映射所有的物理内存和 IO空间。
	 */
	 /*
	  * 1.内核构架相关初始化函数,可以说是非常重要的一个初始化步骤。 
	  * 其中包含了处理器相关参数的初始化、内核启动参数（tagged list）的获取和前期处理、 
	  * 内存子系统的早期的初始化（bootmem分配器）。 主要完成了4个方面的工作，一个就是取得MACHINE和PROCESSOR的信息然或将他们赋值 
	  * 给kernel相应的全局变量，然后呢是对boot_command_line和tags接行解析，再然后呢就是 
	  * memory、cach的初始化，最后是为kernel的后续运行请求资源″ 
	  */
	setup_arch(&command_line);  // kernel/linux-3.10.1/arch/arm64(arm)/kernel/setup.c 内实现 
	
	/*
	 * 1.初始化代表内核本身内
	 *   存使用的管理结构体init_mm。 
	 * 2.ps：每一个任务都有一个mm_struct结构以管理内存空间，init_mm是内核的mm_struct，其中： 
	 * 3.设置成员变量* mmap指向自己，意味着内核只有一个内存管理结构; 
	 * 4.设置* pgd=swapper_pg_dir，swapper_pg_dir是内核的页目录(在arm体系结构有16k， 所以init_mm定义了整个kernel的内存空间)。 
	 * 5.这些内容涉及到内存管理子系统
	 */
	mm_init_owner(&init_mm, &init_task);
	mm_init_cpumask(&init_mm);	// 初始化CPU屏蔽字 
	
	// 1.对cmdline进行备份和保存：保存未改变的comand_line到字符数组static_command_line［］ 中。保存  boot_command_line到字符数组saved_command_line［］中 
	setup_command_line(command_line); 
	
	/* 
	 * 如果没有定义CONFIG_SMP宏，则这个函数为空函数。
	 * 如果定义了CONFIG_SMP宏，则这个setup_per_cpu_areas()函数给每个CPU分配内存，
	 * 并拷贝.data.percpu段的数据。为系统中的每个CPU的per_cpu变量申请空间。 
	 */
	/* 
	 * 下面三段 
	 * 1.针对SMP处理器的内存初始化函数，如果不是SMP系统则都为空函数。 (arm为空) 
	 * 2.他们的目的是给每个CPU分配内存，并拷贝.data.percpu段的数据。为系统中的每个CPU的per_cpu变量申请空间并为boot CPU设置一些数据。 
	 * 3.在SMP系统中，在引导过程中使用的CPU称为boot CPU
	 */
	setup_nr_cpu_ids();
	setup_per_cpu_areas();
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */

	build_all_zonelists(NULL, NULL); 	// 建立系统内存页区(zone)链表 
	page_alloc_init();		// 内存页初始化 

	pr_notice("Kernel command line: %s\n", boot_command_line);
	parse_early_param();	//	解析早期格式的内核参数 
	/* 
	 * 函数对Linux启动命令行参数进行在分析和处理, 
	 * 当不能够识别前面的命令时，所调用的函数。 
	 */
	parse_args("Booting kernel", static_command_line, __start___param,
		   __stop___param - __start___param,
		   -1, -1, &unknown_bootoption);

	jump_label_init();

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	setup_log_buf(0);
	
	// 初始化hash表，以便于从进程的PID获得对应的进程描述指针，按照开发办上的物理内存初始化pid hash表
	pidhash_init();
	vfs_caches_init_early(); // 建立节点哈希表和数据缓冲哈希表 
	sort_main_extable(); // 对异常处理函数进行排序 
	trap_init(); // 初始化硬件中断 
	mm_init(); // Set up kernel memory allocators 建立了内核的内存分配器 

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	preempt_disable(); // 禁止调度 
	
	// 先检查中断是否已经打开，若打开，输出信息后则关闭中断 
	if (WARN(!irqs_disabled(), "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	idr_init_cache(); // 创建idr缓冲区 
	perf_event_init();
	rcu_init();	// 互斥访问机制 
	tick_nohz_init();
	radix_tree_init(); 
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	init_IRQ();  // 中断向量初始化 
	tick_init();	// 1.初始化内核时钟系统 
	init_timers(); // 定时器初始化 
	hrtimers_init(); // 高精度时钟初始化 
	softirq_init(); // 软中断初始化 
	timekeeping_init(); // 初始化资源和普通计时器 
	time_init();
	profile_init();  // 对内核的一个性能测试工具profile进行初始化 
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");
	early_boot_irqs_disabled = false;
	local_irq_enable();	// 使能中断 

	kmem_cache_init_late(); // kmem_cache_init_late的目的就在于完善slab分配器的缓存机制. 

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	console_init(); // 初始化控制台以显示printk的内容 
	if (panic_later)
		panic(panic_later, panic_param);

	lockdep_info(); // 如果定义了CONFIG_LOCKDEP宏，那么就打印锁依赖信息，否则什么也不做 

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	page_cgroup_init();
	debug_objects_mem_init();
	kmemleak_init();
	setup_per_cpu_pageset();
	numa_policy_init();
	if (late_time_init)
		late_time_init();
	sched_clock_init();
	calibrate_delay(); // 校准延时函数的精确度 
	pidmap_init(); // 进程号位图初始化，一般用一个錺age来表示所有进程的pid占用情况 
	anon_vma_init(); //	匿名虚拟内存域（ anonymous VMA）初始化 
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
	thread_info_cache_init(); // 获取thread_info缓存空间，大部分构架为空函数（包括ARM 
	cred_init();	// 任务信用系统初始化。详见：Documentation/credentials.txt 
	fork_init(totalram_pages); // 进程创建机制初始化。为内核"task_struct"分配空间，计算最大任务数。 
	proc_caches_init(); // 初始化进程创建机制所需的其他数据结构，为其申请空间。 
	buffer_init(); // 缓存系统初始化，创建缓存头空间，并检查其大小限时。 
	key_init();  // 内核密钥管理系统初始化 
	security_init(); // 内核安全框架初始? 
	dbg_late_init();
	vfs_caches_init(totalram_pages); // 虚拟文件系统（VFS）缓存初始化 
	signals_init(); // 信号管理系统初始化 
	/* rootfs populating might need page-writeback */
	page_writeback_init(); // 页写回机制初始化 
#ifdef CONFIG_PROC_FS
	proc_root_init(); // proc文件系统初始化 
#endif
	cgroup_init(); // control group正式初始化 
	cpuset_init(); // CPUSET初始化。 参考资料：《多核心計算環境—NUMA與CPUSET簡介》 
	taskstats_init_early(); // 任务状态早期初始化函数：为结构体获取高速缓存，并初始化互斥机制。 
	delayacct_init(); // 任务延迟初始化 

	check_bugs(); // 检查CPU BUG的函数，通过软件规避BUG 

	/* before LAPIC and SMP initACPI早期初始化函数。 ACPI - Advanced Configuration and Power Interface高级配置及电源接口 */
	acpi_early_init(); /* before LAPIC and SMP init */
	sfi_init_late(); // 功能跟踪调试机制初始化，ftrace 是 function trace 的简称 

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_late_init();
		efi_free_boot_services();
	}

	ftrace_init();

	/* Do the rest non-__init'ed, we're now alive */
	rest_init(); // 虽然从名字上来说是剩余的初始化。但是这个函数中的初始化包含了很多的内容 
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

static char msgbuf[64];

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	ktime_t calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	pr_debug("calling  %pF @ %i\n", fn, task_pid_nr(current));
	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	pr_debug("initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;

	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count() = count;
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	extern const struct kernel_param __start___param[], __stop___param[];
	initcall_t *fn;

	strcpy(static_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   static_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   &repair_env_string);

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(*fn);
}

static void __init do_initcalls(void)
{
	int level;

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level); // 依次调用不同等级的初始化函数 
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
/*
 * 好了, 设备现在已经初始化完成。 但是还没有一个设备被初始化过，
 * 但是 CPU 的子系统已经启动并运行，
 * 且内存和处理器管理系统已经在工作了。
 * 现在我们终于可以开始做一些实际的工作了..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp(); //针对SMP系统，初始化内核control group的cpuset子系统。如果非SMP，此函数为空。 
	
	/* 
	 * 创建一个单线程工作队列khelper。
	 * 运行的系统中只有一个，主要作用是指定用户空间的程序路径和环境变量, 
	 * 最终运行指定的user space的程序，属于关键线程，不能关闭
	 */
	usermodehelper_init();
	shmem_init();
	driver_init(); // 初始化驱动模型中的各子系统，可见的现象是在/sys中出现的目录和文件 (实现位于linux-3.10.1/drivers/base/init.c ) 
	init_irq_proc(); // 在proc文件系统中创建irq目录，并在其中初始化系统中所有中断对应的目录。 
	do_ctors(); // 调用链接到内核中的所有构造函数，也就是链接进.ctors段中的所有函数。 
	usermodehelper_enable();
	do_initcalls(); // 调用所有编译内核的驱动模块中的初始化函数。 
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(*fn);
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(init_filename,
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static noinline void __init kernel_init_freeable(void);

static int __ref kernel_init(void *unused)
{
	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	/* 在释放内存前，必须完成所有的异步 __init 代码 */
	async_synchronize_full();
	free_initmem(); // 释放所有init.* 段中的内存。 
	mark_rodata_ro(); // 通过修改页表，保证只读数据段为只读属性。大部分构架为空函数 
	system_state = SYSTEM_RUNNING; // 设置系统状态为运行状态 
	numa_default_policy(); // 设定NUMA系统的内存访问策略为默认 

	flush_delayed_fput();

	// 如果ramdisk_execute_command有指定的init程序，就执行它 
	if (ramdisk_execute_command) {
		if (!run_init_process(ramdisk_execute_command))
			return 0;
		pr_err("Failed to execute %s\n", ramdisk_execute_command);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine. 
	 * 
	 * 我们尝试以下的每个函数，直到函数成功执行.
     * 如果我们试图修复一个真正有问题的设备，
	 * Bourne shell 可以替代init进程。
	 */
	if (execute_command) {
		if (!run_init_process(execute_command))
			return 0;
		pr_err("Failed to execute %s.  Attempting defaults...\n",
			execute_command);
	}
	if (!run_init_process("/sbin/init") ||
	    !run_init_process("/etc/init") ||
	    !run_init_process("/bin/init") ||
	    !run_init_process("/bin/sh"))
		return 0;

	panic("No init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);
	/*
	 * init can run on any cpu.
	 */
	set_cpus_allowed_ptr(current, cpu_all_mask);

	cad_pid = task_pid(current);

	smp_prepare_cpus(setup_max_cpus);

	do_pre_smp_initcalls();
	lockup_detector_init();

	smp_init();
	sched_init_smp();
	// 以上代码是在SMP系统做准备，激活所有CPU，并开始SMP系统的调度 

	/* 
	 * do_basic_setup函数主要是初始化设备驱动，完成其他驱动程序（直接编译进内核的模块）的初始化。
	 * 内核中大部分的启动数据输出（都是各设备的驱动模块输出）都是这里产生的 
	 */
	do_basic_setup();  // <---- 驱动工程师重点关注此函数内部逻辑 

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * 复制两次标准输入（0）的文件描述符（它是上面打开的/dev/console，也就是系统控制台）：
	 * 一个作为标准输出（1）
	 * 一个作为标准出错（2）
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */
	
	/* 检查是否有早期用户空间的init程序。如果有，让其执行 */ 
	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 */

	/* rootfs is available now, try loading default modules */
	load_default_modules();
}
