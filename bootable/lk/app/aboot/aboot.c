/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <app.h>
#include <debug.h>
#include <arch/arm.h>
#include <dev/udc.h>
#include <string.h>
#include <kernel/thread.h>
#include <arch/ops.h>

#include <dev/flash.h>
#include <lib/ptable.h>
#include <dev/keys.h>
#include <dev/fbcon.h>
#include <baseband.h>
#include <target.h>
#include <mmc.h>
#include <partition_parser.h>
#include <platform.h>
#include <crypto_hash.h>
#include <smem.h> //ML
#include "image_verify.h"
#include "recovery.h"
#include "bootimg.h"
#include "fastboot.h"
#include "sparse_format.h"
#include "mmc.h"
#include "devinfo.h"

#include "scm.h"

#define EXPAND(NAME) #NAME
#define TARGET(NAME) EXPAND(NAME)
#define DEFAULT_CMDLINE "mem=100M console=null";

#ifdef MEMBASE
#define EMMC_BOOT_IMG_HEADER_ADDR (0xFF000+(MEMBASE))
#else
#define EMMC_BOOT_IMG_HEADER_ADDR 0xFF000
#endif

#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500

static const char *emmc_cmdline = " androidboot.emmc=true";
static const char *usb_sn_cmdline = " androidboot.serialno=";
static const char *battchg_pause = " androidboot.mode=charger";
static const char *auth_kernel = " androidboot.authorized_kernel=true";
static const char *boot_up_mode_normal   = " androidboot.bootupmode=normal";
static const char *boot_up_mode_recovery   = " androidboot.bootupmode=recovery";
static const char *reboot_mode_normal = " reboot=h";
static const char *reboot_mode_recovery = " reboot=i";

static const char *baseband_apq     = " androidboot.baseband=apq";
static const char *baseband_msm     = " androidboot.baseband=msm";
static const char *baseband_csfb    = " androidboot.baseband=csfb";
static const char *baseband_svlte2a = " androidboot.baseband=svlte2a";
static const char *baseband_mdm     = " androidboot.baseband=mdm";
static const char *baseband_sglte   = " androidboot.baseband=sglte";

/* Assuming unauthorized kernel image by default */
static int auth_kernel_img = 0;

static device_info device = {DEVICE_MAGIC, 0, 0};

static struct udc_device surf_udc_device = {
	.vendor_id	= 0x18d1,
	.product_id	= 0xD00D,
	.version_id	= 0x0100,
	.manufacturer	= "Google",
	.product	= "Android",
};

struct atag_ptbl_entry
{
	char name[16];
	unsigned offset;
	unsigned size;
	unsigned flags;
};

//ML add support change fastboot usb info
#define USB_SERIAL_NUMBER_LEN 13
#define USB_ID_MAGIC_NUM    0x75736264 //in ascii "usbd"
typedef struct
{
  unsigned int magicNum;
  unsigned short idVendor;
  unsigned short idProduct;
  unsigned serialNumberLen ;
  char   serialNumber[USB_SERIAL_NUMBER_LEN];
} usb_device_info;


char sn_buf[13];

extern int emmc_recovery_init(void);

#if NO_KEYPAD_DRIVER
extern int fastboot_trigger(void);
#endif

static void ptentry_to_tag(unsigned **ptr, struct ptentry *ptn)
{
	struct atag_ptbl_entry atag_ptn;

	memcpy(atag_ptn.name, ptn->name, 16);
	atag_ptn.name[15] = '\0';
	atag_ptn.offset = ptn->start;
	atag_ptn.size = ptn->length;
	atag_ptn.flags = ptn->flags;
	memcpy(*ptr, &atag_ptn, sizeof(struct atag_ptbl_entry));
	*ptr += sizeof(struct atag_ptbl_entry) / sizeof(unsigned);
}

/* 内核引导参数处理,并直接跳入kernel执行 */
void boot_linux(void *kernel, unsigned *tags,
		const char *cmdline, unsigned machtype,
		void *ramdisk, unsigned ramdisk_size)
{
	unsigned *ptr = tags;
	unsigned pcount = 0;
	void (*entry)(unsigned,unsigned,unsigned*) = kernel; // 设置hdr->kernel_addr函数指针，最后跳入运行 
	struct ptable *ptable;
	int cmdline_len = 0;
	int have_cmdline = 0;
	int pause_at_bootup = 0;
	unsigned char *cmdline_final = NULL;

	/* CORE */
	*ptr++ = 2;
	*ptr++ = 0x54410001;

	/* 传递 ramdisk 的 size 和 address 给kernel，其中0x54410001是个 magic number. */
	if (ramdisk_size) {
		*ptr++ = 4;
		*ptr++ = 0x54420005;
		*ptr++ = (unsigned)ramdisk;
		*ptr++ = ramdisk_size;
	}

	ptr = target_atag_mem(ptr);

	if (!target_is_emmc_boot()) {
		/* Skip NAND partition ATAGS for eMMC boot */
		if ((ptable = flash_get_ptable()) && (ptable->count != 0)) {
			int i;
			*ptr++ = 2 + (ptable->count * (sizeof(struct atag_ptbl_entry) /
						       sizeof(unsigned)));
			*ptr++ = 0x4d534d70;
			for (i = 0; i < ptable->count; ++i)
				ptentry_to_tag(&ptr, ptable_get(ptable, i));
		}
	}

	/* 传递(cmdline 给kernel，类似uboot的bootargs        */ 
	if (cmdline && cmdline[0]) {
		cmdline_len = strlen(cmdline);
		have_cmdline = 1;
	}
	if (target_is_emmc_boot()) {
		cmdline_len += strlen(emmc_cmdline);
	}

	cmdline_len += strlen(usb_sn_cmdline);
	cmdline_len += strlen(sn_buf);
	if(!boot_into_recovery)
	{
		cmdline_len += strlen(boot_up_mode_normal);
		cmdline_len += strlen(reboot_mode_normal);
	}
	else{
		cmdline_len += strlen(boot_up_mode_recovery);
		cmdline_len += strlen(reboot_mode_recovery);

	}

	if (target_pause_for_battery_charge()) {
		pause_at_bootup = 1;
		cmdline_len += strlen(battchg_pause);
	}

	if(target_use_signed_kernel() && auth_kernel_img) {
		cmdline_len += strlen(auth_kernel);
	}

	/* Determine correct androidboot.baseband to use */
	switch(target_baseband())
	{
		case BASEBAND_APQ:
			cmdline_len += strlen(baseband_apq);
			break;

		case BASEBAND_MSM:
			cmdline_len += strlen(baseband_msm);
			break;

		case BASEBAND_CSFB:
			cmdline_len += strlen(baseband_csfb);
			break;

		case BASEBAND_SVLTE2A:
			cmdline_len += strlen(baseband_svlte2a);
			break;

		case BASEBAND_MDM:
			cmdline_len += strlen(baseband_mdm);
			break;

		case BASEBAND_SGLTE:
			cmdline_len += strlen(baseband_sglte);
			break;
	}

	if (cmdline_len > 0) {
		const char *src;
		char *dst;
		unsigned n;
		/* include terminating 0 and round up to a word multiple */
		n = (cmdline_len + 4) & (~3);
		*ptr++ = (n / 4) + 2;
		*ptr++ = 0x54410009;
		dst = (char *)ptr;
		/* Save start ptr for debug print */
		cmdline_final = (char *)ptr;
		if (have_cmdline) {
			src = cmdline;
			while ((*dst++ = *src++));
		}
		if (target_is_emmc_boot()) {
			src = emmc_cmdline;
			if (have_cmdline) --dst;
			have_cmdline = 1;
			while ((*dst++ = *src++));
		}

		src = usb_sn_cmdline;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));
		src = sn_buf;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));

		if (pause_at_bootup) {
			src = battchg_pause;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		if(target_use_signed_kernel() && auth_kernel_img) {
			src = auth_kernel;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		switch(target_baseband())
		{
			case BASEBAND_APQ:
				src = baseband_apq;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MSM:
				src = baseband_msm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_CSFB:
				src = baseband_csfb;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SVLTE2A:
				src = baseband_svlte2a;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MDM:
				src = baseband_mdm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SGLTE:
				src = baseband_sglte;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;
		}
		if (!boot_into_recovery)
		{
			src = boot_up_mode_normal;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
		else
		{
			src = boot_up_mode_recovery;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
		if (!boot_into_recovery)
		{
			src = reboot_mode_normal;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
		else
		{
			src = reboot_mode_recovery;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		ptr += (n / 4);
	}

	/* END */
	*ptr++ = 0;
	*ptr++ = 0;

	dprintf(INFO, "booting linux @ %p, ramdisk @ %p (%d)\n",
		kernel, ramdisk, ramdisk_size);
	if (cmdline_final)
		dprintf(INFO, "cmdline: %s\n", cmdline_final);

	enter_critical_section();
	/* do any platform specific cleanup before kernel entry */
	platform_uninit();
	/* 关掉cache和mmu */
	arch_disable_cache(UCACHE);
	arch_disable_mmu();
	
	/*
	调到entry开始执行kernel，传递三个参数: 
	  第一个固定为0，
	  第二个machtype被定义成LINUX_MACHTYPE，一般在对应平台的init.c中定义，
	  第三个参数tags，就是传给给kernel的参数.
	*/
	/* 
	一般Android源码工程默认不包含Linux Kernel代码 ，
	编译好的二进制在 /prebuilts/qemu-kernel/[cpu架构]/kernel-qemu 
	*/
	entry(0, machtype, tags); /* 跳入kernel执行,kernel 代码的第一调指令就是 B xxxxx,代码位于 kernel/arch/arm64/kernel/head.S (stext入口)  */
}

unsigned page_size = 0;
unsigned page_mask = 0;

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

static unsigned char buf[4096]; //Equal to max-supported pagesize

int boot_linux_from_mmc(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	struct boot_img_hdr *uhdr;
	unsigned offset = 0;
	unsigned long long ptn = 0;
	unsigned n = 0;
	const char *cmdline;
	int index = INVALID_PTN;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;

	uhdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
	if (!memcmp(uhdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(INFO, "Unified boot method!\n");
		hdr = uhdr;
		goto unified_boot;
	}

	/* 获取指定分区的索引index，并获取分区所在的扇区首地址ptn */
	if (!boot_into_recovery) {
		index = partition_get_index("boot");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No boot partition found\n");
                    return -1;
		}
	}
	else {
		index = partition_get_index("recovery");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No recovery partition found\n");
                    return -1;
		}
	}

	/* 读取分区的首页数据 */
	if (mmc_read(ptn + offset, (unsigned int *) buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
                return -1;
	}

	/* 判断标志位是否是 "ANDROID!"         */ 
	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
                return -1;
	}

	/* 判断分区指定的页大小是否与当前机器匹配                    */
	if (hdr->page_size && (hdr->page_size != page_size)) {
		page_size = hdr->page_size;
		page_mask = page_size - 1;
	}

	/* Authenticate Kernel */
	/* 采用签名的内核,且设备没有未解锁 */
	if(target_use_signed_kernel() && (!device.is_unlocked) && (!device.is_tampered))
	{
		image_addr = (unsigned char *)target_get_scratch_address();
		kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);     /* kernel大小 */
		ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);   /* ramdisk大小 */
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual); 

		offset = 0;

		/* Assuming device rooted at this time */
		device.is_tampered = 1;

		/* Read image without signature */
		/* 从 EMMC 读取除了签名之外的boot/fastboot部分(kernel+ramdisk) */
		if (mmc_read(ptn + offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		offset = imagesize_actual;
		/* Read signature */
		/* 从 EMMC 读取内核的签名信息 */
		if(mmc_read(ptn + offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
		}
		else
		{
			/* 对 boot.img 鉴权 */
			auth_kernel_img = image_verify((unsigned char *)image_addr,
					(unsigned char *)(image_addr + imagesize_actual),
					imagesize_actual,
					CRYPTO_AUTH_ALG_SHA256);

			if(auth_kernel_img)
			{
				/* Authorized kernel */
				device.is_tampered = 0;
			}
		}

		/* Move kernel and ramdisk to correct address */ 
		/* 将 kernel 和 ramdisk 移到正确的地址处(一般加载到 0x00008000 物理地址)  */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);

		/* Make sure everything from scratch address is read before next step!*/
		if(device.is_tampered)
		{
			/* 签名验证不通过 */
		
			write_device_info_mmc(&device); /*将info写入到devinfo分区,则下次开机是,devinfo分区 */
		#ifdef TZ_TAMPER_FUSE
			set_tamper_fuse_cmd();
		#endif
		}
	#if USE_PCOM_SECBOOT
		set_tamper_flag(device.is_tampered);
	#endif
	}
	else
	{
		/* boot.img 不需要验证签名的情况          ,直接读取kernel/ramdisk到指定位置 */
	
		offset += page_size;

		n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		if (mmc_read(ptn + offset, (void *)hdr->kernel_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
					return -1;
		}
		offset += n;

		n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		if(n != 0)
		{
			if (mmc_read(ptn + offset, (void *)hdr->ramdisk_addr, n)) {
				dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
				return -1;
			}
		}
		offset += n;
	}

unified_boot:
	dprintf(INFO, "\nkernel  @ %x (%d bytes)\n", hdr->kernel_addr,
		hdr->kernel_size);
	dprintf(INFO, "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr,
		hdr->ramdisk_size);

	/* 获取boot.img 的启动参数，如果没有则设置一个默认的 */
	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	dprintf(INFO, "\nBooting Linux\n");
	/* 调用 boot_linux() 解压(这份代码没看到解压过程，应该是需要解压才能调用entry的，否则异常)并启动内核 */
	boot_linux((void *)hdr->kernel_addr, (unsigned *) hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

//  主要是内核的加载过程，我们的 boot.img 包含：kernel 头、kernel、ramdisk、second stage（可以没有）。 
int boot_linux_from_flash(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	unsigned n;
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned offset = 0;
	const char *cmdline;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;

	if (target_is_emmc_boot()) {
		hdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
		if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
			return -1;
		}
		goto continue_boot;
	}

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return -1;
	}

	if(!boot_into_recovery)
	{
	        ptn = ptable_find(ptable, "boot");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No boot partition found\n");
		        return -1;
	        }
	}
	else
	{
	        ptn = ptable_find(ptable, "recovery");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No recovery partition found\n");
		        return -1;
	        }
	}

	// 读取boot 头部 
	if (flash_read(ptn, offset, buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
		return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
		return -1;
	}

	if (hdr->page_size != page_size) {
		dprintf(CRITICAL, "ERROR: Invalid boot image pagesize. Device pagesize: %d, Image pagesize: %d\n",page_size,hdr->page_size);
		return -1;
	}

	/* Authenticate Kernel */
	if(target_use_signed_kernel() && (!device.is_unlocked) && (!device.is_tampered))
	{
		image_addr = (unsigned char *)target_get_scratch_address();
		kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual);

		offset = 0;

		/* Assuming device rooted at this time */
		device.is_tampered = 1;

		/* Read image without signature */
		if (flash_read(ptn, offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		offset = imagesize_actual;
		/* Read signature */
		if (flash_read(ptn, offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
		}
		else
		{

			/* Verify signature */
			auth_kernel_img = image_verify((unsigned char *)image_addr,
						(unsigned char *)(image_addr + imagesize_actual),
						imagesize_actual,
						CRYPTO_AUTH_ALG_SHA256);

			if(auth_kernel_img)
			{
				/* Authorized kernel */
				device.is_tampered = 0;
			}
		}

		/* Move kernel and ramdisk to correct address */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);

		/* Make sure everything from scratch address is read before next step!*/
		if(device.is_tampered)
		{
			write_device_info_flash(&device);
		}
#if USE_PCOM_SECBOOT
		set_tamper_flag(device.is_tampered);
#endif
	}
	else
	{
		offset = page_size;

		n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		// 读取 内核 
		if (flash_read(ptn, offset, (void *)hdr->kernel_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
			return -1;
		}
		offset += n;

		n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		// 读取 ramdisk 
		if (flash_read(ptn, offset, (void *)hdr->ramdisk_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
			return -1;
		}
		offset += n;
	}
continue_boot:
	dprintf(INFO, "\nkernel  @ %x (%d bytes)\n", hdr->kernel_addr,
		hdr->kernel_size);
	dprintf(INFO, "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr,
		hdr->ramdisk_size);

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	/* TODO: create/pass atags to kernel */

	dprintf(INFO, "\nBooting Linux\n");
	// 启动内核 
	boot_linux((void *)hdr->kernel_addr, (void *)hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

unsigned char info_buf[4096];
void write_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	memcpy(info, dev, sizeof(device_info));

	if(mmc_write((ptn + size - 512), 512, (void *)info_buf))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
		return;
	}
}

void read_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	if(mmc_read((ptn + size - 512), (void *)info_buf, 512))
	{
		dprintf(CRITICAL, "ERROR: Cannot read device info\n");
		return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;

		write_device_info_mmc(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info_flash(device_info *dev)
{
	struct device_info *info = (void *) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	memcpy(info, dev, sizeof(device_info));

	if (flash_write(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}
}

void read_device_info_flash(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	if (flash_read(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;
		write_device_info_flash(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		write_device_info_mmc(dev);
	}
	else
	{
		write_device_info_flash(dev);
	}
}

void read_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		read_device_info_mmc(dev);
	}
	else
	{
		read_device_info_flash(dev);
	}
}

void reset_device_info()
{
	dprintf(ALWAYS, "reset_device_info called.");
	device.is_tampered = 0;
	write_device_info(&device);
}

void set_device_root()
{
	dprintf(ALWAYS, "set_device_root called.");
	device.is_tampered = 1;
	write_device_info(&device);
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	static struct boot_img_hdr hdr;
	char *ptr = ((char*) data);

	if (sz < sizeof(hdr)) {
		fastboot_fail("invalid bootimage header");
		return;
	}

	memcpy(&hdr, data, sizeof(hdr));

	/* ensure commandline is terminated */
	hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

	if(target_is_emmc_boot() && hdr.page_size) {
		page_size = hdr.page_size;
		page_mask = page_size - 1;
	}

	kernel_actual = ROUND_TO_PAGE(hdr.kernel_size, page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr.ramdisk_size, page_mask);

	/* sz should have atleast raw boot image */
	if (page_size + kernel_actual + ramdisk_actual > sz) {
		fastboot_fail("incomplete bootimage");
		return;
	}

	memmove((void*) hdr.kernel_addr, ptr + page_size, hdr.kernel_size);
	memmove((void*) hdr.ramdisk_addr, ptr + page_size + kernel_actual, hdr.ramdisk_size);

	fastboot_okay("");
	udc_stop();

	boot_linux((void*) hdr.kernel_addr, (void*) hdr.tags_addr,
		   (const char*) hdr.cmdline, board_machtype(),
		   (void*) hdr.ramdisk_addr, hdr.ramdisk_size);
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (flash_erase(ptn)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}


static unsigned int erase_out[4096] = {0};
void cmd_erase_mmc(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);

	if(ptn == 0) {
		fastboot_fail("Partition table doesn't exist\n");
		return;
	}
	/* Simple inefficient version of erase. Just writing
       0 in first block */
	if (mmc_write(ptn , sizeof(erase_out), (unsigned int *)erase_out)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}


void cmd_flash_mmc_img(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	if (!strcmp(arg, "partition"))
	{
		dprintf(INFO, "Attempt to write partition image.\n");
		if (write_partition(sz, (unsigned char *) data)) {
			fastboot_fail("failed to write partition");
			return;
		}
	}
	else
	{
		index = partition_get_index(arg);
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			fastboot_fail("partition table doesn't exist");
			return;
		}

		if (!strcmp(arg, "boot") || !strcmp(arg, "recovery")) {
			if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
				fastboot_fail("image is not a boot image");
				return;
			}
		}

		size = partition_get_size(index);
		if (ROUND_TO_PAGE(sz,511) > size) {
			fastboot_fail("size too large");
			return;
		}
		else if (mmc_write(ptn , sz, (unsigned int *)data)) {
			fastboot_fail("flash write failure");
			return;
		}
	}
	fastboot_okay("");
	return;
}

void cmd_flash_mmc_sparse_img(const char *arg, void *data, unsigned sz)
{
	unsigned int chunk;
	unsigned int chunk_data_sz;
	sparse_header_t *sparse_header;
	chunk_header_t *chunk_header;
	uint32_t total_blocks = 0;
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);
	if(ptn == 0) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	size = partition_get_size(index);
	if (ROUND_TO_PAGE(sz,511) > size) {
		fastboot_fail("size too large");
		return;
	}

	/* Read and skip over sparse image header */
	sparse_header = (sparse_header_t *) data;
	data += sparse_header->file_hdr_sz;
	if(sparse_header->file_hdr_sz > sizeof(sparse_header_t))
	{
		/* Skip the remaining bytes in a header that is longer than
		 * we expected.
		 */
		data += (sparse_header->file_hdr_sz - sizeof(sparse_header_t));
	}

	dprintf (SPEW, "=== Sparse Image Header ===\n");
	dprintf (SPEW, "magic: 0x%x\n", sparse_header->magic);
	dprintf (SPEW, "major_version: 0x%x\n", sparse_header->major_version);
	dprintf (SPEW, "minor_version: 0x%x\n", sparse_header->minor_version);
	dprintf (SPEW, "file_hdr_sz: %d\n", sparse_header->file_hdr_sz);
	dprintf (SPEW, "chunk_hdr_sz: %d\n", sparse_header->chunk_hdr_sz);
	dprintf (SPEW, "blk_sz: %d\n", sparse_header->blk_sz);
	dprintf (SPEW, "total_blks: %d\n", sparse_header->total_blks);
	dprintf (SPEW, "total_chunks: %d\n", sparse_header->total_chunks);

	/* Start processing chunks */
	for (chunk=0; chunk<sparse_header->total_chunks; chunk++)
	{
		/* Read and skip over chunk header */
		chunk_header = (chunk_header_t *) data;
		data += sizeof(chunk_header_t);

		dprintf (SPEW, "=== Chunk Header ===\n");
		dprintf (SPEW, "chunk_type: 0x%x\n", chunk_header->chunk_type);
		dprintf (SPEW, "chunk_data_sz: 0x%x\n", chunk_header->chunk_sz);
		dprintf (SPEW, "total_size: 0x%x\n", chunk_header->total_sz);

		if(sparse_header->chunk_hdr_sz > sizeof(chunk_header_t))
		{
			/* Skip the remaining bytes in a header that is longer than
			 * we expected.
			 */
			data += (sparse_header->chunk_hdr_sz - sizeof(chunk_header_t));
		}

		chunk_data_sz = sparse_header->blk_sz * chunk_header->chunk_sz;
		switch (chunk_header->chunk_type)
		{
			case CHUNK_TYPE_RAW:
			if(chunk_header->total_sz != (sparse_header->chunk_hdr_sz +
											chunk_data_sz))
			{
				fastboot_fail("Bogus chunk size for chunk type Raw");
				return;
			}

			if(mmc_write(ptn + ((uint64_t)total_blocks*sparse_header->blk_sz),
						chunk_data_sz,
						(unsigned int*)data))
			{
				fastboot_fail("flash write failure");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			case CHUNK_TYPE_DONT_CARE:
			total_blocks += chunk_header->chunk_sz;
			break;

			case CHUNK_TYPE_CRC:
			if(chunk_header->total_sz != sparse_header->chunk_hdr_sz)
			{
				fastboot_fail("Bogus chunk size for chunk type Dont Care");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			default:
			fastboot_fail("Unknown chunk type");
			return;
		}
	}

	dprintf(INFO, "Wrote %d blocks, expected to write %d blocks\n",
					total_blocks, sparse_header->total_blks);

	if(total_blocks != sparse_header->total_blks)
	{
		fastboot_fail("sparse image write failure");
	}

	fastboot_okay("");
	return;
}

void cmd_flash_mmc(const char *arg, void *data, unsigned sz)
{
	sparse_header_t *sparse_header;
	/* 8 Byte Magic + 2048 Byte xml + Encrypted Data */
	unsigned int *magic_number = (unsigned int *) data;
	int ret=0;

	if (magic_number[0] == DECRYPT_MAGIC_0 &&
		magic_number[1] == DECRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = decrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Invalid secure image\n");
			return;
		}
	}
	else if (magic_number[0] == ENCRYPT_MAGIC_0 &&
			 magic_number[1] == ENCRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = encrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Encryption Failure\n");
			return;
		}
	}

	sparse_header = (sparse_header_t *) data;
	if (sparse_header->magic != SPARSE_HEADER_MAGIC)
		cmd_flash_mmc_img(arg, data, sz);
	else
		cmd_flash_mmc_sparse_img(arg, data, sz);
	return;
}

void cmd_flash(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned extra = 0;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (!strcmp(ptn->name, "boot") || !strcmp(ptn->name, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(ptn->name, "system")
		|| !strcmp(ptn->name, "userdata")
		|| !strcmp(ptn->name, "persist")
		|| !strcmp(ptn->name, "recoveryfs")) {
		if (flash_ecc_bch_enabled())
			/* Spare data bytes for 8 bit ECC increased by 4 */
			extra = ((page_size >> 9) * 20);
		else
			extra = ((page_size >> 9) * 16);
	} else
		sz = ROUND_TO_PAGE(sz, page_mask);

	dprintf(INFO, "writing %d bytes to '%s'\n", sz, ptn->name);
	if (flash_write(ptn, extra, data, sz)) {
		fastboot_fail("flash write failure");
		return;
	}
	dprintf(INFO, "partition '%s' updated\n", ptn->name);
	fastboot_okay("");
}

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	udc_stop();
	if (target_is_emmc_boot())
	{
		boot_linux_from_mmc();
	}
	else
	{
		boot_linux_from_flash();
	}
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(0);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(FASTBOOT_MODE);
}

void cmd_oem_unlock(const char *arg, void *data, unsigned sz)
{
	if(!device.is_unlocked)
	{
		device.is_unlocked = 1;
		write_device_info(&device);
	}
	fastboot_okay("");
}

void cmd_oem_devinfo(const char *arg, void *data, unsigned sz)
{
	char response[64];
	snprintf(response, 64, "\tDevice tampered: %s", (device.is_tampered ? "true" : "false"));
	fastboot_info(response);
	snprintf(response, 64, "\tDevice unlocked: %s", (device.is_unlocked ? "true" : "false"));
	fastboot_info(response);
	fastboot_okay("");
}

void splash_screen ()
{
	struct ptentry *ptn;
	struct ptable *ptable;
	struct fbcon_config *fb_display = NULL;

	if (!target_is_emmc_boot())
	{
		ptable = flash_get_ptable();
		if (ptable == NULL) {
			dprintf(CRITICAL, "ERROR: Partition table not found\n");
			return;
		}

		ptn = ptable_find(ptable, "splash");
		if (ptn == NULL) {
			dprintf(CRITICAL, "ERROR: No splash partition found\n");
		} else {
			fb_display = fbcon_display();
			if (fb_display) {
				if (flash_read(ptn, 0, fb_display->base,
					(fb_display->width * fb_display->height * fb_display->bpp/8))) {
					fbcon_clear();
					dprintf(CRITICAL, "ERROR: Cannot read splash image\n");
				}
			}
		}
	}
}
//ML add
#ifdef PLATFORM_MSM7X27A
static void set_usb_serial_num()
{
	boot_info_for_apps binfo ;
	usb_device_info *usb_info;
	unsigned smem_status;
	smem_status = smem_read_alloc_entry(SMEM_BOOT_INFO_FOR_APPS,
										&binfo,  sizeof(boot_info_for_apps));
	if (smem_status)
	{
		dprintf(CRITICAL, "ERROR: unable to read shared memory for boot info\n");
		return;
	}
      usb_info  = (usb_device_info*)(binfo.PAD);

	  //check if is valid
	  if(usb_info->magicNum ==USB_ID_MAGIC_NUM ){

		if(usb_info->serialNumberLen>13){
			dprintf(CRITICAL, "ERROR: wrong serial num length \n");
			return;
		}

		memset(sn_buf,0,sizeof(sn_buf));
		memcpy(sn_buf,&usb_info->serialNumber,usb_info->serialNumberLen);
	}
}
#endif

void aboot_init(const struct app_descriptor *app)
{
	unsigned reboot_mode = 0;
	unsigned usb_init = 0;
	unsigned sz = 0;

	/* 设置NAND/ EMMC读取信息页面大小 */
	if (target_is_emmc_boot())
	{
		page_size = 2048;    /* 获取mmc/ufs的页大小，ufs为4096，emmc为2048  */
		page_mask = page_size - 1;  /* 获取mmc/ufs的页掩码 */
	}
	else
	{
		page_size = flash_page_size();
		page_mask = page_size - 1;
	}

	if(target_use_signed_kernel())
	{
		/* 获取device_info信息，开发中需要关注 device_info 信息，它牵扯到fastboot是否被禁掉，boot.img是否需要鉴权等等 */
		read_device_info(&device);
	}

	/* 根据不同的硬件获取emmc/ufs序列号   , 例如: lk/target/msm7627a/init.c */
	/* 如果源码编译，这里可以随便修改硬件序列号 */
	target_serialno((unsigned char *) sn_buf);
	dprintf(SPEW,"serial number: %s\n",sn_buf);
	surf_udc_device.serialno = sn_buf;

	/* 读取按键信息，判断是正常开机，还是进入 fastboot ,还是进入recovery 模式 */
	if (keys_get_state(KEY_HOME) != 0) /* HOME 键被按下 */
		boot_into_recovery = 1;
	if (keys_get_state(KEY_VOLUMEUP) != 0) /* 音量上键被按下 */
		boot_into_recovery = 1;
	if(!boot_into_recovery)
	{
		/* 同时按下返回键或者音量下键，则启动fastboot模式 */
		if (keys_get_state(KEY_BACK) != 0) 
			goto fastboot;
		if (keys_get_state(KEY_VOLUMEDOWN) != 0)
			goto fastboot;
	}

	#if NO_KEYPAD_DRIVER  // 如果没有按键驱动? 
	if (fastboot_trigger())
		goto fastboot;
	#endif

	/* 获取重启原因，设置相应的开机模式标志，之前使用的是共享内存记录重启原因，最新转为使用pmic的pon寄存器记录重启原因 */
	/* 不同的硬件平台实现方式不一样(例:lk/target/mdm9615/init.c) */
	reboot_mode = check_reboot_mode();
	if (reboot_mode == RECOVERY_MODE) {
		boot_into_recovery = 1;
	} else if(reboot_mode == FASTBOOT_MODE) {
		goto fastboot;
	}

normal_boot:
	if (target_is_emmc_boot())  /* 从emmc/ufs启动 */
	{
		if(emmc_recovery_init()) /* recovery模式需要的一些初始化 */
			dprintf(ALWAYS,"error in emmc_recovery_init\n");
		if(target_use_signed_kernel())  /* 判断是否使用了签名的kernel（即boot.img） */
		{
			if((device.is_unlocked) || (device.is_tampered))
			{
			#ifdef TZ_TAMPER_FUSE
				set_tamper_fuse_cmd();  /* 暂不关注fuse */
			#endif
			#if USE_PCOM_SECBOOT
				set_tamper_flag(device.is_tampered); /* 暂不关注secboot */
			#endif
			}
		}

		/* 从emmc/ufs加载boot.img，选择dts，设置cmdline，跳转到kernel */ 
		boot_linux_from_mmc();  
	}
	else
	{
		/* 从nanflash启动，当前已经没有手机厂商在使用nandflash了，可以忽略此代码 */
		recovery_init();
#if USE_PCOM_SECBOOT
	if((device.is_unlocked) || (device.is_tampered))
		set_tamper_flag(device.is_tampered);
#endif
		save_debug_message();
		boot_linux_from_flash();  // 从 nand 中加载 内核 
	}
	dprintf(CRITICAL, "ERROR: Could not do normal boot. Reverting "
		"to fastboot mode.\n");

fastboot:

	target_fastboot_init();

	#ifdef PLATFORM_MSM7X27A
	//change the sn_buf
	   set_usb_serial_num();
	 #endif

	if(!usb_init)
		udc_init(&surf_udc_device);

	/* 注册fastboot命令 */
	fastboot_register("boot", cmd_boot);

	if (target_is_emmc_boot())
	{
		fastboot_register("flash:", cmd_flash_mmc);
		fastboot_register("erase:", cmd_erase_mmc);
	}
	else
	{
		fastboot_register("flash:", cmd_flash);
		fastboot_register("erase:", cmd_erase);
		save_debug_message();
	}

	fastboot_register("continue", cmd_continue);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bootloader);
	fastboot_register("oem unlock", cmd_oem_unlock);
	fastboot_register("oem device-info", cmd_oem_devinfo);
	fastboot_publish("product", TARGET(BOARD));
	fastboot_publish("kernel", "lk");
	fastboot_publish("serialno", sn_buf);
	partition_dump();  /* 打印分区表信息 */
	sz = target_get_max_flash_size();
	fastboot_init(target_get_scratch_address(), sz); /* 初始化并启动fastboot，参数1为fastboot使用的内存buffer的地址，参数2位内存buffer的大小 */
	udc_start();  /* 开始 USB 协议 */ 
}

APP_START(aboot)
	.init = aboot_init,
APP_END

