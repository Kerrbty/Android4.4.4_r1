/*
 * Copyright (c) 2008, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2012, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in
 *	the documentation and/or other materials provided with the
 *	distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *	may be used to endorse or promote products derived from this
 *	software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <reg.h>
#include <debug.h>
#include <kernel/thread.h>
#include <platform/debug.h>
#include <platform/iomap.h>
#include <platform/irqs.h>
#include <mddi.h>
#include <dev/fbcon.h>
#include <dev/gpio.h>
#include <smem.h>
#include <mmu.h>
#include <arch/arm/mmu.h>

static struct fbcon_config *fb_config;

static uint32_t ticks_per_sec = 0;

void platform_init_interrupts(void);
void platform_init_timer();

void uart3_clock_init(void);
void uart_init(void);

void acpu_clock_init(void);

void mddi_clock_init(unsigned num, unsigned rate);

extern void mipi_dsi_shutdown(void);

unsigned board_msm_id(void);

static int target_uses_qgic;
int debug_timer = 0, gpt_timer = 0, usb_hs_int = 0;
#define MB (1024*1024)

/* LK memory - cacheable, write through */
#define ALL_MEMORY         (MMU_MEMORY_TYPE_STRONGLY_ORDERED | \
				    MMU_MEMORY_AP_READ_WRITE)

#define CS0_MEMORY_PSTART   (0x00000000)
#define CS0_MEMORY_VSTART   (0x00000000)
#define CS0_MEMORY_SIZE     (0x10000000)
#define CS1_MEMORY_PSTART   (0x20000000)
#define CS1_MEMORY_VSTART   (0x10000000)
#define CS1_MEMORY_SIZE	    (0x10000000 - 0x4000000)

mmu_section_t mmu_section_table[] = {
	/*  Physical addr,    Virtual addr,    Size (in MB),    Flags */
    {CS0_MEMORY_PSTART, CS0_MEMORY_VSTART, (CS0_MEMORY_SIZE/MB),    ALL_MEMORY},
    {CS1_MEMORY_PSTART, CS1_MEMORY_VSTART, (CS1_MEMORY_SIZE/MB),    ALL_MEMORY},
};

/* Setup memory for this platform */
void platform_init_mmu_mappings(void)
{
    uint32_t i;
    uint32_t sections;
    uint32_t table_size = ARRAY_SIZE(mmu_section_table);

    for (i = 0; i < table_size; i++)
    {
        sections = mmu_section_table[i].num_of_sections;

        while (sections--)
        {
            arm_mmu_map_section(mmu_section_table[i].paddress + sections*MB,
                                mmu_section_table[i].vaddress + sections*MB,
                                mmu_section_table[i].flags);
        }
    }
}

void platform_early_init(void)
{
#if WITH_DEBUG_UART
	uart1_clock_init();
	uart_init();
#endif
	if(machine_is_8x25()) {
		qgic_init();
		target_uses_qgic = 1;
		debug_timer = (GIC_PPI_START + 2);
		gpt_timer = (GIC_PPI_START + 3);
		usb_hs_int = INT_USB_HS_GIC;
	} else {
		platform_init_interrupts();
		debug_timer = 8;
		gpt_timer = 7;
		usb_hs_int = INT_USB_HS_VIC;
	}
	platform_init_timer();
}

void platform_init(void)
{
	dprintf(INFO, "platform_init()\n");
	acpu_clock_init();
}

void display_init(void)
{
	dprintf(INFO, "display_init()\n");

#if DISPLAY_TYPE_MDDI
	fb_config = mddi_init();
	ASSERT(fb_config);
	fbcon_setup(fb_config);
#endif
#if DISPLAY_TYPE_LCDC
	panel_lcdc_init();
	fb_config = lcdc_init();
	ASSERT(fb_config);
	fbcon_setup(fb_config);
#endif
#if DISPLAY_TYPE_MIPI
	panel_dsi_init();
	fb_config = mipi_init();
	ASSERT(fb_config);
	fbcon_setup(fb_config);
#endif
}

void display_shutdown(void)
{
	dprintf(SPEW, "display_shutdown()\n");

#if DISPLAY_TYPE_MIPI
	mipi_dsi_shutdown();
	panel_dsi_deinit();
#endif
#if DISPLAY_TYPE_LCDC
	/* Turn off LCDC */
	lcdc_shutdown();
	panel_lcdc_deinit();
#endif
}

void platform_uninit(void)
{
#if DISPLAY_SPLASH_SCREEN
	display_shutdown();
#endif

	platform_uninit_timer();
}

/* Initialize DGT timer */
void platform_init_timer(void)
{
	/* disable timer */
	writel(0, DGT_ENABLE);

	ticks_per_sec = 19200000;	/* Uses TCXO (19.2 MHz) */
}

/* Returns timer ticks per sec */
uint32_t platform_tick_rate(void)
{
	return ticks_per_sec;
}

bool machine_is_7x25a(void)
{
	if ((board_msm_id() == MSM7225A) || (board_msm_id() == MSM7625A))
		return 1;
	else
		return 0;
}

/* Toggle RESET pin of the DSI Client before sending
 * panel init commands
 */
void panel_dsi_init(void)
{
	dprintf(SPEW, "panel_dsi_init()\n");

	if (machine_is_7x27a_surf_ffa()) {
		gpio_set(128, 0x1);
		mdelay(5);
		gpio_set(128, 0x0);
		gpio_set(129, 0x1);
		gpio_config(129, GPIO_OUTPUT);
		gpio_set(129, 0x0);
		gpio_set(129, 0x1);
		mdelay(10);
	} else if (machine_is_evb() || machine_is_qrd5()) {
		gpio_tlmm_config(GPIO_CFG(35, 0, 1, 0, 0), 0);
		gpio_tlmm_config(GPIO_CFG(40, 0, 1, 0, 0), 0);
		gpio_tlmm_config(GPIO_CFG(85, 0, 1, 0, 0), 0);
		gpio_tlmm_config(GPIO_CFG(96, 0, 1, 0, 0), 0);

		gpio_config(35, GPIO_OUTPUT);
		gpio_config(40, GPIO_OUTPUT);
		gpio_config(85, GPIO_OUTPUT);
		gpio_config(96, GPIO_OUTPUT);

		//turn on ext power
		gpio_set(35, 0x1);
		gpio_set(40, 0x1);
		mdelay(20);
		//reset
		gpio_set(85, 0x0);
		mdelay(20);
		gpio_set(85, 0x1);
		mdelay(20);
		//turn on backlight
		gpio_set(96, 0x1);
	}

	dprintf(SPEW, "panel_dsi_init() done\n");
}

void panel_dsi_deinit(void)
{
	if (machine_is_7x27a_surf_ffa()) {
		/* Power down DSI bridge chip */
		gpio_set(128, 0x1);
	} else if (machine_is_evb() || machine_is_qrd5()) {
		//turn off backlight
		gpio_set(96, 0x0);
		//pull down reset
		gpio_set(85, 0x0);
		//turn off ext power
		gpio_set(35, 0x0);
		gpio_set(40, 0x0);
	}
}

void panel_lcdc_init(void)
{
	dprintf(SPEW, "panel_lcdc_init()\n");

	if (machine_is_qrd7()) {
		truly_lcdc_on();
	}
}

void panel_lcdc_deinit(void)
{
	dprintf(SPEW, "panel_lcdc_deinit()\n");

	if (machine_is_qrd7()) {
		truly_lcdc_off();
	}
}

int target_supports_qgic()
{
	return target_uses_qgic;
}
