/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <reg.h>
#include <endian.h>
#include <mipi_dsi.h>
#include <dev/fbcon.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>
#include <target/display.h>
#include <platform/iomap.h>
#include <platform/clock.h>
#include <platform/timer.h>

extern void mdp_disable(void);
extern int mipi_dsi_cmd_config(struct fbcon_config *mipi_fb_cfg,
			       unsigned short num_of_lanes);
extern void mdp_shutdown(void);
extern void mdp_start_dma(void);
extern void dsb(void);

/* panel specific info */
#if DISPLAY_MIPI_PANEL_TOSHIBA
static struct fbcon_config mipi_fb_cfg_toshiba = {
	.height = TSH_MIPI_FB_HEIGHT,
	.width = TSH_MIPI_FB_WIDTH,
	.stride = TSH_MIPI_FB_WIDTH,
	.format = FB_FORMAT_RGB888,
	.bpp = 24,
	.update_start = NULL,
	.update_done = NULL,
};

struct mipi_dsi_panel_config toshiba_panel_info = {
	.mode = MIPI_VIDEO_MODE,
	.num_of_lanes = 1,
	.dsi_phy_config = &mipi_dsi_toshiba_panel_phy_ctrl,
	.panel_cmds = toshiba_panel_video_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(toshiba_panel_video_mode_cmds),
};
#elif DISPLAY_MIPI_PANEL_NOVATEK_BLUE
static struct fbcon_config mipi_fb_cfg_novatek = {
	.height = NOV_MIPI_FB_HEIGHT,
	.width = NOV_MIPI_FB_WIDTH,
	.stride = NOV_MIPI_FB_WIDTH,
	.format = FB_FORMAT_RGB888,
	.bpp = 24,
	.update_start = NULL,
	.update_done = NULL,
};

struct mipi_dsi_panel_config novatek_panel_info = {
	.mode = MIPI_CMD_MODE,
	.num_of_lanes = 2,
	.dsi_phy_config = &mipi_dsi_novatek_panel_phy_ctrl,
	.panel_cmds = novatek_panel_cmd_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(novatek_panel_cmd_mode_cmds),
};
#elif DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
static struct fbcon_config mipi_fb_cfg_toshiba_mdt61 = {
	.height = TSH_MDT61_MIPI_FB_HEIGHT,
	.width = TSH_MDT61_MIPI_FB_WIDTH,
	.stride = TSH_MDT61_MIPI_FB_WIDTH,
	.format = FB_FORMAT_RGB888,
	.bpp = 24,
	.update_start = NULL,
	.update_done = NULL,
};

struct mipi_dsi_panel_config toshiba_mdt61_panel_info = {
	.mode = MIPI_VIDEO_MODE,
	.num_of_lanes = 3,
	.dsi_phy_config = &mipi_dsi_toshiba_mdt61_panel_phy_ctrl,
	.panel_cmds = toshiba_mdt61_video_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(toshiba_mdt61_video_mode_cmds),
};
#endif

static struct fbcon_config mipi_fb_cfg_renesas = {
	.height = REN_MIPI_FB_HEIGHT,
	.width = REN_MIPI_FB_WIDTH,
	.stride = REN_MIPI_FB_WIDTH,
	.format = FB_FORMAT_RGB888,
	.bpp = 24,
	.update_start = NULL,
	.update_done = NULL,
};

struct mipi_dsi_panel_config renesas_panel_info = {
	.mode = MIPI_VIDEO_MODE,
	.num_of_lanes = 2,
	.dsi_phy_config = &mipi_dsi_renesas_panel_phy_ctrl,
	.panel_cmds = renesas_panel_video_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(renesas_panel_video_mode_cmds),
	.lane_swap = 1,
};

static struct fbcon_config mipi_fb_cfg_nt35510 = {
	.height = NT35510_MIPI_FB_HEIGHT,
	.width = NT35510_MIPI_FB_WIDTH,
	.stride = NT35510_MIPI_FB_WIDTH,
	.format = FB_FORMAT_RGB888,
	.bpp = 24,
	.update_start = NULL,
	.update_done = NULL,
};

struct mipi_dsi_panel_config nt35510_video_panel_info = {
	.mode = MIPI_VIDEO_MODE,
	.num_of_lanes = 2,
	.dsi_phy_config = &mipi_dsi_nt35510_panel_phy_ctrl,
	.panel_cmds = nt35510_panel_video_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(nt35510_panel_video_mode_cmds),
	.lane_swap = 1,
};

struct mipi_dsi_panel_config nt35510_cmd_panel_info = {
	.mode = MIPI_CMD_MODE,
	.num_of_lanes = 2,
	.dsi_phy_config = &mipi_dsi_nt35510_panel_phy_ctrl,
	.panel_cmds = nt35510_panel_cmd_mode_cmds,
	.num_of_panel_cmds = ARRAY_SIZE(nt35510_panel_cmd_mode_cmds),
	.lane_swap = 1,
};

static struct fbcon_config mipi_fb_cfg_default = {
	.height = 0,
	.width = 0,
	.stride = 0,
	.format = 0,
	.bpp = 0,
	.update_start = NULL,
	.update_done = NULL,
};

static struct fbcon_config * get_mipi_fbcon_config(void)
{
	struct fbcon_config *mipi_fb_cfg = &mipi_fb_cfg_default;

	dprintf(SPEW, "get_mipi_fbcon_config() done\n");

#if DISPLAY_MIPI_PANEL_TOSHIBA
	mipi_fb_cfg = &mipi_fb_cfg_toshiba;
#elif DISPLAY_MIPI_PANEL_NOVATEK_BLUE
	mipi_fb_cfg = &mipi_fb_cfg_novatek;
#elif DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
	mipi_fb_cfg = &mipi_fb_cfg_toshiba_mdt61;
#elif DISPLAY_MIPI_PANEL_RENESAS || DISPLAY_MIPI_VIDEO_PANEL_NT35510 || DISPLAY_MIPI_CMD_PANEL_NT35510
	if (machine_is_7x27a_surf_ffa()) {
		mipi_fb_cfg = &mipi_fb_cfg_renesas;
		if (machine_is_7x25a()) {
			mipi_fb_cfg->height = REN_MIPI_FB_HEIGHT_HVGA;
		}
	} else if (machine_is_evb() || machine_is_qrd5()) {
		mipi_fb_cfg = &mipi_fb_cfg_nt35510;
	}
#endif

	dprintf(SPEW, "get_mipi_fbcon_config() done, mipi_fb_cfg = %x\n", mipi_fb_cfg);

	return mipi_fb_cfg;
}

static int cmd_mode_status = 0;
void secure_writel(uint32_t, uint32_t);
uint32_t secure_readl(uint32_t);

static void dump_all_dsi_registers(void)
{
	int val = 0;
	int i;
	int addr = 0x0;

	dprintf(SPEW, "Dump DSI register\n");

	for (i = 0; i < 0x300; i += 4) {
		val = readl(REG_DSI(i));
		dprintf(SPEW, "DSI register %x = %x\n", i, val);
	}
}

static void dump_all_mdp_registers(void)
{
	int val = 0;
	int i;
	int addr = 0x0;

	dprintf(SPEW, "Dump MDP register\n");

	for (i = 0x020; i < 0x07c; i += 4) {
		val = readl(MDP_BASE + i);
		dprintf(SPEW, "MDP register %x = %x\n", i, val);
	}

	for (i = 0x90000; i < 0x90074; i += 4) {
		val = readl(MDP_BASE + i);
		dprintf(SPEW, "MDP register %x = %x\n", i, val);
	}

	val = readl(MDP_BASE + 0xf1000);
	dprintf(SPEW, "MDP register %x = %x\n", 0xf1000, val);

	val = readl(MDP_BASE + 0xf1004);
	dprintf(SPEW, "MDP register %x = %x\n", 0xf1004, val);
}

static void dump_dsi_status(void)
{
	int val = 0;

	dprintf(SPEW, "Dump DSI status\n");

	val = readl(MDP_INTR_ENABLE);
	dprintf(SPEW, "MDP_INTR_ENABLE = %x\n", val);

	val = readl(MDP_INTR_STATUS);
	dprintf(SPEW, "MDP_INTR_STATUS = %x\n", val);

	val = readl(MDP_DISPLAY_STATUS);
	dprintf(SPEW, "MDP_DISPLAY_STATUS = %x\n", val);

	val = readl(DSI_STATUS);
	dprintf(SPEW, "DSI_STATUS = %x\n", val);

	val = readl(DSI_FIFO_STATUS);
	dprintf(SPEW, "DSI_FIFO_STATUS = %x\n", val);

	val = readl(DSI_INT_CTRL);
	dprintf(SPEW, "DSI_INT_CTRL = %x\n", val);

	val = readl(DSI_ACK_ERR_STATUS);
	dprintf(SPEW, "DSI_ACK_ERR_STATUS = %x\n", val);

	val = readl(DSI_LANE_STATUS);
	dprintf(SPEW, "DSI_LANE_STATUS = %x\n", val);

	val = readl(DSI_TIMEOUT_STATUS);
	dprintf(SPEW, "DSI_TIMEOUT_STATUS = %x\n", val);

	val = readl(DSI_CAL_STATUS);
	dprintf(SPEW, "DSI_CAL_STATUS = %x\n", val);

	val = readl(DSI_CLK_STATUS);
	dprintf(SPEW, "DSI_CLK_STATUS = %x\n", val);
}

int mipi_dsi_phy_ctrl_config(struct mipi_dsi_panel_config *pinfo)
{
	unsigned i;
	unsigned off = 0;
	struct mipi_dsi_phy_ctrl *pd;

	dprintf(SPEW, "mipi_dsi_phy_ctrl_config()\n");

	writel(0x00000001, DSIPHY_SW_RESET);
	writel(0x00000000, DSIPHY_SW_RESET);

	pd = (pinfo->dsi_phy_config);

	off = 0x02cc;		/* regulator ctrl 0 */
	for (i = 0; i < 4; i++) {
		writel(pd->regulator[i], MIPI_DSI_BASE + off);
		off += 4;
	}

	off = 0x0260;		/* phy timig ctrl 0 */
	for (i = 0; i < 11; i++) {
		writel(pd->timing[i], MIPI_DSI_BASE + off);
		off += 4;
	}

	// T_CLK_POST, T_CLK_PRE for CLK lane P/N HS 200 mV timing length should >
	// data lane HS timing length
	writel(0xa1e, DSI_CLKOUT_TIMING_CTRL);

	off = 0x0290;		/* ctrl 0 */
	for (i = 0; i < 4; i++) {
		writel(pd->ctrl[i], MIPI_DSI_BASE + off);
		off += 4;
	}

	off = 0x02a0;		/* strength 0 */
	for (i = 0; i < 4; i++) {
		writel(pd->strength[i], MIPI_DSI_BASE + off);
		off += 4;
	}

#if DISPLAY_MIPI_PANEL_RENESAS
	if (machine_is_7x25a()) {
		pd->pll[10] |= 0x8;
	}
#endif
	off = 0x0204;		/* pll ctrl 1, skip 0 */
	for (i = 1; i < 21; i++) {
		writel(pd->pll[i], MIPI_DSI_BASE + off);
		off += 4;
	}

	/* pll ctrl 0 */
	writel(pd->pll[0], MIPI_DSI_BASE + 0x200);
	writel((pd->pll[0] | 0x01), MIPI_DSI_BASE + 0x200);
	/* lane swp ctrol */
	if (pinfo->lane_swap)
		writel(pinfo->lane_swap, MIPI_DSI_BASE + 0xac);

	dprintf(SPEW, "mipi_dsi_phy_ctrl_config() done\n");

	return (0);
}

struct mipi_dsi_panel_config *get_panel_info(void)
{
	struct mipi_dsi_panel_config *panel_info = NULL;

	dprintf(SPEW, "get_panel_info()\n");

#if DISPLAY_MIPI_PANEL_TOSHIBA
	panel_info = &toshiba_panel_info;
#elif DISPLAY_MIPI_PANEL_NOVATEK_BLUE
	panel_info = &novatek_panel_info;
#elif DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
	panel_info = &toshiba_mdt61_panel_info;
#elif DISPLAY_MIPI_PANEL_RENESAS || DISPLAY_MIPI_VIDEO_PANEL_NT35510 || DISPLAY_MIPI_CMD_PANEL_NT35510
	if (machine_is_7x27a_surf_ffa()) {
		if (machine_is_7x25a()) {
			renesas_panel_info.num_of_lanes = 1;
		}
		panel_info = &renesas_panel_info;
	} else if (machine_is_evb() || machine_is_qrd5()) {
		/* EVB uses different LCM with SKU5, so the display needs not reverse */
		if (machine_is_evb()) {
			nt35510_cmd19[6] = 0x00;
			nt35510_video19[6] = 0x00;
		}
#if DISPLAY_MIPI_VIDEO_PANEL_NT35510
		panel_info = &nt35510_video_panel_info;
#elif DISPLAY_MIPI_CMD_PANEL_NT35510
		panel_info = &nt35510_cmd_panel_info;
#endif
	}
#endif

	dprintf(SPEW, "get_panel_info() done, panel_info = %x\n", panel_info);

	return panel_info;
}

int dsi_cmd_dma_trigger_for_panel()
{
	unsigned long ReadValue;
	unsigned long count = 0;
	int status = 0;

	writel(0x03030303, DSI_INT_CTRL);
	writel(0x1, DSI_CMD_MODE_DMA_SW_TRIGGER);
	dsb();
	ReadValue = readl(DSI_INT_CTRL) & 0x00000001;
	while (ReadValue != 0x00000001) {
		ReadValue = readl(DSI_INT_CTRL) & 0x00000001;
		count++;
		if (count > 0xffff) {
			status = FAIL;
			dprintf(CRITICAL,
				"Panel CMD: command mode dma test failed\n");
			return status;
		}
	}

	writel((readl(DSI_INT_CTRL) | 0x01000001), DSI_INT_CTRL);
	dprintf(SPEW, "Panel CMD: command mode dma tested successfully\n");
	return status;
}

int mipi_dsi_cmds_tx(struct mipi_dsi_cmd *cmds, int count)
{
	int ret = 0;
	struct mipi_dsi_cmd *cm;
	int i = 0;
	char pload[256];
	uint32_t off;

	/* Align pload at 8 byte boundry */
	off = pload;
	off &= 0x07;
	if (off)
		off = 8 - off;
	off += pload;

	cm = cmds;
	for (i = 0; i < count; i++) {
		memcpy((void *)off, (cm->payload), cm->size);
		writel(off, DSI_DMA_CMD_OFFSET);
		writel(cm->size, DSI_DMA_CMD_LENGTH);	// reg 0x48 for this build
		dsb();
		ret += dsi_cmd_dma_trigger_for_panel();
		udelay(80);
		cm++;
	}
	return ret;
}

/*
 * mipi_dsi_cmd_rx: can receive at most 16 bytes
 * per transaction since it only have 4 32bits reigsters
 * to hold data.
 * therefore Maximum Return Packet Size need to be set to 16.
 * any return data more than MRPS need to be break down
 * to multiple transactions.
 */
int mipi_dsi_cmds_rx(char **rp, int len)
{
	uint32_t *lp, data;
	char *dp;
	int i, off, cnt;
	int rlen, res;

	if (len <= 2)
		rlen = 4;	/* short read */
	else
		rlen = MIPI_DSI_MRPS + 6;	/* 4 bytes header + 2 bytes crc */

	if (rlen > MIPI_DSI_REG_LEN) {
		return 0;
	}

	res = rlen & 0x03;

	rlen += res;		/* 4 byte align */
	lp = (uint32_t *) (*rp);

	cnt = rlen;
	cnt += 3;
	cnt >>= 2;

	if (cnt > 4)
		cnt = 4;	/* 4 x 32 bits registers only */

	off = 0x068;		/* DSI_RDBK_DATA0 */
	off += ((cnt - 1) * 4);

	for (i = 0; i < cnt; i++) {
		data = (uint32_t) readl(MIPI_DSI_BASE + off);
		*lp++ = ntohl(data);	/* to network byte order */
		off -= 4;
	}

	if (len > 2) {
		/*First 4 bytes + paded bytes will be header next len bytes would be payload */
		for (i = 0; i < len; i++) {
			dp = *rp;
			//dp[i] = dp[4 + res + i];
			dp[i] = dp[4 + i]; //FIXME
		}
	}

	return len;
}

static int mipi_dsi_cmd_bta_sw_trigger(void)
{
	uint32_t data;
	int cnt = 0;
	int err = 0;

	writel(0x01, MIPI_DSI_BASE + 0x094);	/* trigger */
	while (cnt < 10000) {
		data = readl(MIPI_DSI_BASE + 0x0004);	/*DSI_STATUS */
		if ((data & 0x0010) == 0)
			break;
		cnt++;
	}
	if (cnt == 10000)
		err = 1;

	dprintf(SPEW, "BTA done, cnt=%d, err=%d\n", cnt, err);
	return err;
}

#if DISPLAY_MIPI_PANEL_NOVATEK_BLUE
static uint32_t mipi_novatek_manufacture_id(void)
{
	char rec_buf[24];
	char *rp = rec_buf;
	uint32_t *lp, data;

	mipi_dsi_cmds_tx(&novatek_panel_manufacture_id_cmd, 1);
	mipi_dsi_cmds_rx(&rp, 3);

	lp = (uint32_t *) rp;
	data = (uint32_t) * lp;
	data = ntohl(data);
	data = data >> 8;
	return data;
}
#endif

#if DISPLAY_MIPI_CMD_PANEL_NT35510
static uint32_t mipi_nt35510_manufacture_id(void)
{
	char rec_buf[24];
	char *rp = rec_buf;
	uint32_t *lp, data;

	mipi_dsi_cmds_tx(nt35510_panel_enable_page1_cmd, ARRAY_SIZE(nt35510_panel_enable_page1_cmd));
	mdelay(50);
	mipi_dsi_cmds_tx(nt35510_panel_set_pkt_size_cmd, ARRAY_SIZE(nt35510_panel_set_pkt_size_cmd));
	mdelay(50);
	mipi_dsi_cmds_tx(nt35510_panel_manufacture_id_cmd, ARRAY_SIZE(nt35510_panel_manufacture_id_cmd));
	mdelay(10);
	mipi_dsi_cmds_rx(&rp, 3);

	lp = (uint32_t *) rp;
	data = (uint32_t) * lp;
	data = ntohl(data);
	data = data >> 8;

	dprintf(SPEW, "manufacture id = %x, lp = %x\n", data, *lp);

	return data;
}
#endif

int mipi_dsi_panel_initialize(struct mipi_dsi_panel_config *pinfo)
{
	unsigned char DMA_STREAM1 = 0;	// for mdp display processor path
	unsigned char EMBED_MODE1 = 1;	// from frame buffer
	unsigned char POWER_MODE2 = 1;	// from frame buffer
	unsigned char PACK_TYPE1 = 1;	// long packet
	unsigned char VC1 = 0;
	unsigned char DT1 = 0;	// non embedded mode
	unsigned short WC1 = 0;	// for non embedded mode only
	int status = 0;
	unsigned char DLNx_EN;

	dprintf(SPEW, "mipi_dsi_panel_initialize()\n");

	switch (pinfo->num_of_lanes) {
	default:
	case 1:
		DLNx_EN = 1;	// 1 lane
		break;
	case 2:
		DLNx_EN = 3;	// 2 lane
		break;
	case 3:
		DLNx_EN = 7;	// 3 lane
		break;
	}

	writel(0x0001, DSI_SOFT_RESET);
	writel(0x0000, DSI_SOFT_RESET);

	writel((0 << 16) | 0x3f, DSI_CLK_CTRL);	/* Turn on all DSI Clks */
	writel(DMA_STREAM1 << 8 | 0x04, DSI_TRIG_CTRL);	// reg 0x80 dma trigger: sw
	// trigger 0x4; dma stream1

	writel(0 << 30 | DLNx_EN << 4 | 0x105, DSI_CTRL);	// reg 0x00 for this
	// build
	writel(EMBED_MODE1 << 28 | POWER_MODE2 << 26
	       | PACK_TYPE1 << 24 | VC1 << 22 | DT1 << 16 | WC1,
	       DSI_COMMAND_MODE_DMA_CTRL);

	status = mipi_dsi_cmds_tx(pinfo->panel_cmds, pinfo->num_of_panel_cmds);

	dprintf(SPEW, "mipi_dsi_panel_initialize() done\n");

	return status;
}

//TODO: Clean up arguments being passed in not being used
int
config_dsi_video_mode(unsigned short disp_width, unsigned short disp_height,
		      unsigned short img_width, unsigned short img_height,
		      unsigned short hsync_porch0_fp,
		      unsigned short hsync_porch0_bp,
		      unsigned short vsync_porch0_fp,
		      unsigned short vsync_porch0_bp,
		      unsigned short hsync_width,
		      unsigned short vsync_width, unsigned short dst_format,
		      unsigned short traffic_mode, unsigned short datalane_num)
{

	unsigned char DST_FORMAT;
	unsigned char TRAFIC_MODE;
	unsigned char DLNx_EN;
	// video mode data ctrl
	int status = 0;
	unsigned long low_pwr_stop_mode = 0;
	unsigned char eof_bllp_pwr = 0x9;
	unsigned char interleav = 0;

	// disable mdp first
	mdp_disable();

	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000002, DSI_CLK_CTRL);
	writel(0x00000006, DSI_CLK_CTRL);
	writel(0x0000000e, DSI_CLK_CTRL);
	writel(0x0000001e, DSI_CLK_CTRL);
	writel(0x0000003e, DSI_CLK_CTRL);

	writel(0, DSI_CTRL);

	writel(0, DSI_ERR_INT_MASK0);

	DST_FORMAT = 0;		// RGB565
	dprintf(SPEW, "DSI_Video_Mode - Dst Format: RGB565\n");

	DLNx_EN = 1;		// 1 lane with clk programming
	dprintf(SPEW, "Data Lane: 1 lane\n");

	TRAFIC_MODE = 0;	// non burst mode with sync pulses
	dprintf(SPEW, "Traffic mode: non burst mode with sync pulses\n");

	writel(0x02020202, DSI_INT_CTRL);

	writel(((img_width + hsync_porch0_bp) << 16) | hsync_porch0_bp,
	       DSI_VIDEO_MODE_ACTIVE_H);

	writel(((img_height + vsync_porch0_bp) << 16) | (vsync_porch0_bp),
	       DSI_VIDEO_MODE_ACTIVE_V);

	writel(((img_height + vsync_porch0_fp + vsync_porch0_bp) << 16)
	       | img_width + hsync_porch0_fp + hsync_porch0_bp,
	       DSI_VIDEO_MODE_TOTAL);

	writel((hsync_width << 16) | 0, DSI_VIDEO_MODE_HSYNC);

	writel(0 << 16 | 0, DSI_VIDEO_MODE_VSYNC);

	writel(vsync_width << 16 | 0, DSI_VIDEO_MODE_VSYNC_VPOS);

	writel(1, DSI_EOT_PACKET_CTRL);

	writel(0x00000100, DSI_MISR_VIDEO_CTRL);

	writel(low_pwr_stop_mode << 16 | eof_bllp_pwr << 12 | TRAFIC_MODE << 8
	       | DST_FORMAT << 4 | 0x0, DSI_VIDEO_MODE_CTRL);

	writel(0x67, DSI_CAL_STRENGTH_CTRL);

	writel(0x80006711, DSI_CAL_CTRL);

	writel(0x00010100, DSI_MISR_VIDEO_CTRL);

	writel(0x00010100, DSI_INT_CTRL);
	writel(0x02010202, DSI_INT_CTRL);

	writel(0x02030303, DSI_INT_CTRL);

	writel(interleav << 30 | 0 << 24 | 0 << 20 | DLNx_EN << 4
	       | 0x103, DSI_CTRL);
	mdelay(10);

	return status;
}

int
config_dsi_cmd_mode(unsigned short disp_width, unsigned short disp_height,
		    unsigned short img_width, unsigned short img_height,
		    unsigned short dst_format,
		    unsigned short traffic_mode, unsigned short datalane_num)
{
	unsigned char DST_FORMAT;
	unsigned char TRAFIC_MODE;
	unsigned char DLNx_EN;
	// video mode data ctrl
	int status = 0;
	unsigned char interleav = 0;
	unsigned char ystride = 0x03;
	// disable mdp first

	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000000, DSI_CLK_CTRL);
	writel(0x00000002, DSI_CLK_CTRL);
	writel(0x00000006, DSI_CLK_CTRL);
	writel(0x0000000e, DSI_CLK_CTRL);
	writel(0x0000001e, DSI_CLK_CTRL);
	writel(0x0000003e, DSI_CLK_CTRL);

	writel(0x10000000, DSI_ERR_INT_MASK0);

	// writel(0, DSI_CTRL);

	// writel(0, DSI_ERR_INT_MASK0);

	DST_FORMAT = 8;		// RGB888
	dprintf(SPEW, "DSI_Cmd_Mode - Dst Format: RGB888\n");

	DLNx_EN = 3;		// 2 lane with clk programming
	dprintf(SPEW, "Data Lane: 2 lane\n");

	TRAFIC_MODE = 0;	// non burst mode with sync pulses
	dprintf(SPEW, "Traffic mode: non burst mode with sync pulses\n");

	writel(0x02020202, DSI_INT_CTRL);

	writel(0x00100000 | DST_FORMAT, DSI_COMMAND_MODE_MDP_CTRL);
	writel((img_width * ystride + 1) << 16 | 0x0039,
	       DSI_COMMAND_MODE_MDP_STREAM0_CTRL);
	writel((img_width * ystride + 1) << 16 | 0x0039,
	       DSI_COMMAND_MODE_MDP_STREAM1_CTRL);
	writel(img_height << 16 | img_width,
	       DSI_COMMAND_MODE_MDP_STREAM0_TOTAL);
	writel(img_height << 16 | img_width,
	       DSI_COMMAND_MODE_MDP_STREAM1_TOTAL);
	writel(0x67, DSI_CAL_STRENGTH_CTRL);
	writel(0x80000000, DSI_CAL_CTRL);
	writel(0x44, DSI_TRIG_CTRL);
	writel(0x13c2c, DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL);
	writel(interleav << 30 | 0 << 24 | 0 << 20 | DLNx_EN << 4 | 0x105,
	       DSI_CTRL);
	mdelay(10);
	writel(0x14000000, DSI_COMMAND_MODE_DMA_CTRL);
	writel(0x10000000, DSI_MISR_CMD_CTRL);
	writel(0x00000040, DSI_ERR_INT_MASK0);
	writel(0x1, DSI_EOT_PACKET_CTRL);
	// writel(0x0, MDP_OVERLAYPROC0_START);
	mdp_start_dma();
	mdelay(10);
	writel(0x1, DSI_CMD_MODE_MDP_SW_TRIGGER);

	status = 1;
	return status;
}

static void get_panel_timing_param(
	struct fbcon_config *mipi_fb_cfg,
	unsigned short *image_wd,
	unsigned short *image_ht,
	unsigned short *display_wd,
	unsigned short *display_ht,
	unsigned short *hsync_porch_fp,
	unsigned short *hsync_porch_bp,
	unsigned short *vsync_porch_fp,
	unsigned short *vsync_porch_bp,
	unsigned short *hsync_width,
	unsigned short *vsync_width,
	unsigned char *lane_en)
{
	if (machine_is_7x27a_surf_ffa()) {
		*image_wd = mipi_fb_cfg->width;
		*image_ht = mipi_fb_cfg->height;
		*display_wd = mipi_fb_cfg->width;
		*display_ht = mipi_fb_cfg->height;
		*hsync_porch_fp = MIPI_HSYNC_FRONT_PORCH_DCLK;
		*hsync_porch_bp = MIPI_HSYNC_BACK_PORCH_DCLK;
		*vsync_porch_fp = MIPI_VSYNC_FRONT_PORCH_LINES;
		*vsync_porch_bp = MIPI_VSYNC_BACK_PORCH_LINES;
		*hsync_width = MIPI_HSYNC_PULSE_WIDTH;
		*vsync_width = MIPI_VSYNC_PULSE_WIDTH;
		*lane_en = 3; /* 3 Lanes -- Enables Data Lane0, 1, 2 */

		if (machine_is_7x25a()) {
			display_wd = REN_MIPI_FB_WIDTH_HVGA;
			display_ht = REN_MIPI_FB_HEIGHT_HVGA;
			image_wd = REN_MIPI_FB_WIDTH_HVGA;
			image_ht = REN_MIPI_FB_HEIGHT_HVGA;
			hsync_porch_fp = MIPI_HSYNC_FRONT_PORCH_DCLK_HVGA;
			hsync_porch_bp = MIPI_HSYNC_BACK_PORCH_DCLK_HVGA;
			vsync_porch_fp = MIPI_VSYNC_FRONT_PORCH_LINES_HVGA;
			vsync_porch_bp = MIPI_VSYNC_BACK_PORCH_LINES_HVGA;
			hsync_width = MIPI_HSYNC_PULSE_WIDTH_HVGA;
			vsync_width = MIPI_VSYNC_PULSE_WIDTH_HVGA;
			*lane_en = 1;
		}
	} else if (machine_is_evb() || machine_is_qrd5()) {
		*image_wd = mipi_fb_cfg->width;
		*image_ht = mipi_fb_cfg->height;
		*display_wd = mipi_fb_cfg->width;
		*display_ht = mipi_fb_cfg->height;
		*hsync_porch_fp = NT35510_MIPI_HSYNC_FRONT_PORCH_DCLK;
		*hsync_porch_bp = NT35510_MIPI_HSYNC_BACK_PORCH_DCLK;
		*vsync_porch_fp = NT35510_MIPI_VSYNC_FRONT_PORCH_LINES;
		*vsync_porch_bp = NT35510_MIPI_VSYNC_BACK_PORCH_LINES;
		*hsync_width = NT35510_MIPI_HSYNC_PULSE_WIDTH;
		*vsync_width = NT35510_MIPI_VSYNC_PULSE_WIDTH;
		*lane_en = 3;
	}

	return;
}

static void dsi_cmd_mdp_trigger_for_panel(void)
{
	dprintf(SPEW, "dsi_cmd_mdp_trigger_for_panel()\n");
    unsigned long ReadValue;
    unsigned long count = 0;
    int status = 0;

	//start DMA_P
	writel(0x00000001, MDP_DMA_P_START);

	//start DSI trigger
    writel(0x03030303, DSI_INT_CTRL);
    writel(0x1, DSI_CMD_MODE_MDP_SW_TRIGGER);
    ReadValue = readl(DSI_INT_CTRL) & 0x00000100;
    while (ReadValue != 0x00000100) {
        ReadValue = readl(DSI_INT_CTRL) & 0x00000100;
        count++;
        if (count > 0xffff) {
            status = FAIL;
            dprintf(CRITICAL, "dsi_cmd_mdp_trigger_for_panel failed\n");
            return status;
        }
    }

    //writel((readl(DSI_INT_CTRL) | 0x01000100), DSI_INT_CTRL);
    dprintf(SPEW, "dsi_cmd_mdp_trigger_for_panel successfully\n");

    return status;
}

int mipi_dsi_video_config(unsigned short num_of_lanes)
{
	int status = 0;
	unsigned long ReadValue;
	unsigned long count = 0;
	struct fbcon_config *mipi_fb_cfg = get_mipi_fbcon_config();
	unsigned short image_wd = mipi_fb_cfg->width;
	unsigned short image_ht = mipi_fb_cfg->height;
#if !DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
	unsigned short display_wd = mipi_fb_cfg->width;
	unsigned short display_ht = mipi_fb_cfg->height;
	unsigned short hsync_porch_fp = MIPI_HSYNC_FRONT_PORCH_DCLK;
	unsigned short hsync_porch_bp = MIPI_HSYNC_BACK_PORCH_DCLK;
	unsigned short vsync_porch_fp = MIPI_VSYNC_FRONT_PORCH_LINES;
	unsigned short vsync_porch_bp = MIPI_VSYNC_BACK_PORCH_LINES;
	unsigned short hsync_width = MIPI_HSYNC_PULSE_WIDTH;
	unsigned short vsync_width = MIPI_VSYNC_PULSE_WIDTH;
	unsigned short dst_format = 0;
	unsigned short traffic_mode = 0;
	unsigned char lane_en = 3;
#endif
	unsigned short pack_pattern = 0x12;	//BGR
	unsigned char ystride = 3;

#if DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
	pack_pattern = 0x21;	//RGB
	config_mdt61_dsi_video_mode();

	/* Two functions make up mdp_setup_dma_p_video_mode with mdt61 panel functions */
	mdp_setup_dma_p_video_config(pack_pattern, image_wd, image_ht,
				     MIPI_FB_ADDR, image_wd, ystride);
	mdp_setup_mdt61_video_dsi_config();
#elif DISPLAY_MIPI_PANEL_RENESAS || DISPLAY_MIPI_VIDEO_PANEL_NT35510
	get_panel_timing_param(mipi_fb_cfg, &image_wd, &image_ht,
							&display_wd, &display_ht,
							&hsync_porch_fp, &hsync_porch_bp,
							&vsync_porch_fp, &vsync_porch_bp,
							&hsync_width, &vsync_width, &lane_en);

	pack_pattern = 0x21;	//RGB

	config_msm7627a_dsi_video_mode(display_wd, display_ht, image_wd,
				       image_ht, hsync_porch_fp, hsync_porch_bp,
				       vsync_porch_fp, vsync_porch_bp,
				       hsync_width, vsync_width, lane_en);

	status +=
	    mdp_setup_dma_p_video_mode(display_wd, display_ht, image_wd,
				       image_ht, hsync_porch_fp, hsync_porch_bp,
				       vsync_porch_fp, vsync_porch_bp,
				       hsync_width, vsync_width, MIPI_FB_ADDR,
				       image_wd, pack_pattern, ystride);
#else
	status +=
	    config_dsi_video_mode(display_wd, display_ht, image_wd, image_ht,
				  hsync_porch_fp, hsync_porch_bp,
				  vsync_porch_fp, vsync_porch_bp, hsync_width,
				  vsync_width, dst_format, traffic_mode,
				  num_of_lanes);

	status +=
	    mdp_setup_dma_p_video_mode(display_wd, display_ht, image_wd,
				       image_ht, hsync_porch_fp, hsync_porch_bp,
				       vsync_porch_fp, vsync_porch_bp,
				       hsync_width, vsync_width, MIPI_FB_ADDR,
				       image_wd, pack_pattern, ystride);
#endif

	ReadValue = readl(DSI_INT_CTRL) & 0x00010000;
	while (ReadValue != 0x00010000) {
		ReadValue = readl(DSI_INT_CTRL) & 0x00010000;
		count++;
		if (count > 0xffff) {
			status = FAIL;
			dprintf(CRITICAL, "Video lane test failed\n");
			return status;
		}
	}

	dprintf(SPEW, "Video lane tested successfully\n");
	return status;
}

int is_cmd_mode_enabled(void)
{
	return cmd_mode_status;
}

void mipi_dsi_cmd_mode_trigger(void)
{
	int status = 0;
	struct fbcon_config *mipi_fb_cfg = get_mipi_fbcon_config();
	struct mipi_dsi_panel_config *panel_info = get_panel_info();
	unsigned short display_wd = mipi_fb_cfg->width;
	unsigned short display_ht = mipi_fb_cfg->height;
	unsigned short image_wd = mipi_fb_cfg->width;
	unsigned short image_ht = mipi_fb_cfg->height;
	unsigned short dst_format = 0;
	unsigned short traffic_mode = 0;

	status += mipi_dsi_cmd_config(mipi_fb_cfg, panel_info->num_of_lanes);
	mdelay(50);
	config_dsi_cmd_mode(display_wd, display_ht, image_wd, image_ht,
			    dst_format, traffic_mode,
			    panel_info->num_of_lanes /* num_of_lanes */ );

	//dump_dsi_status();
	//dump_all_dsi_registers();
	//dump_all_mdp_registers();
}

void mipi_dsi_shutdown(void)
{
#if (!CONT_SPLASH_SCREEN)
	mdp_shutdown();
	writel(0x01010101, DSI_INT_CTRL);
	writel(0x13FF3BFF, DSI_ERR_INT_MASK0);
#if DISPLAY_MIPI_PANEL_TOSHIBA_MDT61
	/* Disable branch clocks */
	writel(0x0, DSI1_BYTE_CC_REG);
	writel(0x0, DSI_PIXEL_CC_REG);
	writel(0x0, DSI1_ESC_CC_REG);
	/* Disable root clock */
	writel(0x0, DSI_CC_REG);
#elif (!DISPLAY_MIPI_PANEL_RENESAS) && (!DISPLAY_MIPI_VIDEO_PANEL_NT35510) && (!DISPLAY_MIPI_CMD_PANEL_NT35510) && (!DISPLAY_LCDC_PANEL_TRULY)
	secure_writel(0x0, DSI_CC_REG);
	secure_writel(0x0, DSI_PIXEL_CC_REG);
#endif
	writel(0, DSI_CLK_CTRL);
	writel(0, DSI_CTRL);
	writel(0, DSIPHY_PLL_CTRL(0));
#else
        /* To keep the splash screen displayed till kernel driver takes
        control, do not turn off the video mode engine and clocks.
        Only disabling the MIPI DSI IRQs */
        writel(0x01010101, DSI_INT_CTRL);
        writel(0x13FF3BFF, DSI_ERR_INT_MASK0);
#endif
}

struct fbcon_config *mipi_init(void)
{
	int status = 0;
	struct mipi_dsi_panel_config *panel_info = get_panel_info();
	struct fbcon_config *mipi_fb_cfg = get_mipi_fbcon_config();

	dprintf(SPEW, "mipi_init()\n");

	/* Enable MMSS_AHB_ARB_MATER_PORT_E for arbiter master0 and master 1 request */
#if (!DISPLAY_MIPI_PANEL_RENESAS) && (!DISPLAY_MIPI_VIDEO_PANEL_NT35510) && (!DISPLAY_MIPI_CMD_PANEL_NT35510) &&(!DISPLAY_LCDC_PANEL_TRULY)
	writel(0x00001800, MMSS_SFPB_GPREG);
#endif

#if DISPLAY_MIPI_PANEL_TOSHIBA_MDT61 || DISPLAY_MIPI_VIDEO_PANEL_NT35510 || DISPLAY_MIPI_CMD_PANEL_NT35510
	mipi_dsi_phy_init(panel_info);
#else
	mipi_dsi_phy_ctrl_config(panel_info);
#endif

	status += mipi_dsi_panel_initialize(panel_info);

#ifdef DISPLAY_MIPI_MANUFACTURE_ID
	mipi_dsi_cmd_bta_sw_trigger();

#if DISPLAY_MIPI_PANEL_NOVATEK_BLUE
	mipi_novatek_manufacture_id();
#endif
#if DISPLAY_MIPI_CMD_PANEL_NT35510
	mipi_nt35510_manufacture_id();
#endif

#endif //DISPLAY_MIPI_MANUFACTURE_ID

	mipi_fb_cfg->base = MIPI_FB_ADDR;

	if (panel_info->mode == MIPI_VIDEO_MODE)
		status += mipi_dsi_video_config(panel_info->num_of_lanes);

	if (panel_info->mode == MIPI_CMD_MODE)
		cmd_mode_status = 1;

	dprintf(SPEW, "mipi_init() done\n");

	return mipi_fb_cfg;
}
