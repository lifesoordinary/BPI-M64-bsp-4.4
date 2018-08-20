/*
* Sunxi SD/MMC host driver
*
* Copyright (C) 2015 AllWinnertech Ltd.
* Author: lixiang <lixiang@allwinnertech>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed "as is" WITHOUT ANY WARRANTY of any
* kind, whether express or implied; without even the implied warranty
* of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <linux/clk.h>
#include <linux/clk/sunxi.h>

#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/reset.h>

#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>

#include <linux/mmc/host.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/core.h>
#include <linux/mmc/card.h>
#include <linux/mmc/slot-gpio.h>

#include "sunxi-mmc.h"
#include "sunxi-mmc-v4p5x.h"
#include "sunxi-mmc-export.h"

/*reg*/
/*SMHC eMMC4.5 DDR Start Bit Detection Control Register */
/*SMHC CRC Status Detect Control Register */
/*SMHC Card Threshold Control Register */
/*SMHC Drive Delay Control Register */
/*SMHC Sample Delay Control Register */
/*SMHC Data Strobe Delay Control Register */
#define SDXC_REG_EDSD		(0x010C)
#define SDXC_REG_CSDC		(0x0054)
#define SDXC_REG_THLD		(0x0100)
#define SDXC_REG_DRV_DL		(0x0140)
#define SDXC_REG_SAMP_DL	(0x0144)
#define SDXC_REG_DS_DL		(0x0148)
#define SDXC_REG_EMCE		(0x64)		/*SMHC EMCE Control Register*/
#define SDXC_REG_SMCV		(0x300)		/*SMHC Version Register */

/*bit*/
#define SDXC_HS400_MD_EN				(1U<<31)
#define SDXC_CARD_WR_THLD_ENB		(1U<<2)
#define SDXC_CARD_RD_THLD_ENB		(1U)

#define SDXC_DAT_DRV_PH_SEL			(1U<<17)
#define SDXC_CMD_DRV_PH_SEL			(1U<<16)
#define SDXC_SAMP_DL_SW_EN			(1u<<7)
#define SDXC_DS_DL_SW_EN			(1u<<7)

/*mask*/
#define SDXC_CRC_DET_PARA_MASK		(0xf)
#define SDXC_CARD_RD_THLD_MASK		(0x0FFF0000)
#define SDXC_TX_TL_MASK				(0xff)
#define SDXC_RX_TL_MASK				(0x00FF0000)

#define SDXC_SAMP_DL_SW_MASK		(0x0000003F)
#define SDXC_DS_DL_SW_MASK			(0x0000003F)

/*value*/
#define SDXC_CRC_DET_PARA_HS400		(6)
#define SDXC_CRC_DET_PARA_OTHER		(3)
#define SDXC_FIFO_DETH					(1024>>2)

/*size*/
#define SDXC_CARD_RD_THLD_SIZE		(0x00000FFF)

/*shit*/
#define SDXC_CARD_RD_THLD_SIZE_SHIFT		(16)

#define SUNXI_DMA_TL_SDMMC_V4P5X		((0x3<<28)|(15<<16)|240)
/*one dma des can transfer data size = 1<<SUNXI_DES_SIZE_SDMMC2*/
#define SUNXI_DES_SIZE_SDMMC_V4P5X	(12)

/* EMCE controller  */
#define SDXC_EMCE_ENCR		BIT(4)
#define SDXC_EMCE_AC_MD		BIT(1)
#define SDXC_EMCE_ENB		BIT(0)

/*Sunxi MMC Host Controller Version*/
#define SMHC_VERSION_V4P7	0x40700

#ifdef CONFIG_SUNXI_EMCE
extern int sunxi_emce_set_task_des(int data_len);
extern void sunxi_emce_set_task_load(u32 para);
#endif

struct sunxi_mmc_spec_regs {
	u32 drv_dl;		/*REG_DRV_DL */
	u32 samp_dl;		/*REG_SAMP_DL */
	u32 ds_dl;		/*REG_DS_DL */
	/*u32 sd_ntsr;//REG_SD_NTSR */
	u32 edsd;		/*REG_EDSD */
	u32 csdc;		/*REG_CSDC */
};

enum sunxi_mmc_speed_mode {
	SM0_DS26_SDR12 = 0,
	SM1_HSSDR52_SDR25,
	SM2_HSDDR52_DDR50,
	SM3_HS200_SDR104,
	SM4_HS400,
	SM_NUM,
};

struct sunxi_mmc_clk_dly {
	enum sunxi_mmc_speed_mode spm;
	char *mod_str;
	char *raw_tm_sm_str[2];
	u32 raw_tm_sm[2];
	u32 raw_tm_sm_def[2];
};


struct sunxi_mmc_ver_priv {
	struct sunxi_mmc_spec_regs bak_spec_regs;
	struct sunxi_mmc_clk_dly mmc_clk_dly[SM_NUM];
};

static void sunxi_mmc_set_clk_dly(struct sunxi_mmc_host *host, int clk,
				  int bus_width, int timing)
{
	struct mmc_host *mmc = host->mmc;
	enum sunxi_mmc_speed_mode speed_mod = SM0_DS26_SDR12;
	char *raw_sm_str = NULL;
	char *m_str = NULL;
	struct device_node *np = NULL;
	u32 *raw_sm = 0;
	u32 *raw_sm_def = 0;
	u32 rval = 0;
	int frq_index = 0;
	u32 cmd_drv_ph = 1;
	u32 dat_drv_ph = 0;
	u32 sam_dly = 0;
	u32 ds_dly = 0;
	struct sunxi_mmc_clk_dly *mmc_clk_dly =
	    ((struct sunxi_mmc_ver_priv *)host->version_priv_dat)->mmc_clk_dly;

	if (!mmc->parent || !mmc->parent->of_node) {
		dev_err(mmc_dev(host->mmc),
			"no dts to parse clk dly,use default\n");
		return;
	}

	np = mmc->parent->of_node;

	switch (timing) {
	case MMC_TIMING_LEGACY:
	case MMC_TIMING_UHS_SDR12:
		speed_mod = SM0_DS26_SDR12;
		break;
	case MMC_TIMING_MMC_HS:
	case MMC_TIMING_SD_HS:
	case MMC_TIMING_UHS_SDR25:
		speed_mod = SM1_HSSDR52_SDR25;
		break;
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_DDR52:
		speed_mod = SM2_HSDDR52_DDR50;
		break;
	case MMC_TIMING_UHS_SDR50:
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_MMC_HS200:
		speed_mod = SM3_HS200_SDR104;
		break;
	case MMC_TIMING_MMC_HS400:
		speed_mod = SM4_HS400;
		break;
	default:
		dev_err(mmc_dev(mmc), "Wrong timing input\n");
		return;
	}

	if (clk <= 400 * 1000) {
		frq_index = 0;
	} else if (clk <= 25 * 1000 * 1000) {
		frq_index = 1;
	} else if (clk <= 50 * 1000 * 1000) {
		frq_index = 2;
	} else if (clk <= 100 * 1000 * 1000) {
		frq_index = 3;
	} else if (clk <= 150 * 1000 * 1000) {
		frq_index = 4;
	} else if (clk <= 200 * 1000 * 1000) {
		frq_index = 5;
	} else if (clk <= 250 * 1000 * 1000) {
		frq_index = 6;
	} else if (clk <= 300 * 1000 * 1000) {
		frq_index = 7;
	} else {
		dev_err(mmc_dev(mmc), "clk is over 300mhz\n");
		return;
	}

	if (frq_index / 4 > 2) {
		dev_err(mmc_dev(host->mmc), "err frq_index\n");
		return;
	}

	dev_dbg(mmc_dev(host->mmc), "freq %d frq index %d,frq/4 %x\n", clk,
		frq_index, frq_index / 4);
	raw_sm_str = mmc_clk_dly[speed_mod].raw_tm_sm_str[frq_index / 4];
	raw_sm = &mmc_clk_dly[speed_mod].raw_tm_sm[frq_index / 4];
	raw_sm_def = &mmc_clk_dly[speed_mod].raw_tm_sm_def[frq_index / 4];
	m_str = mmc_clk_dly[speed_mod].mod_str;

	rval = of_property_read_u32(np, raw_sm_str, raw_sm);
	if (rval) {
		dev_info(mmc_dev(host->mmc), "failed to get %s used default\n",
			 m_str);
	} else {
		u32 sm_shift = (frq_index % 4) * 8;

		rval = ((*raw_sm) >> sm_shift) & 0xff;
		if (rval != 0xff) {
			if (timing == MMC_TIMING_MMC_HS400) {
				u32 raw_sm_hs200 = 0;

				ds_dly = rval;
				raw_sm_hs200 =
				    mmc_clk_dly[SM3_HS200_SDR104].
				    raw_tm_sm[frq_index / 4];
				sam_dly = ((raw_sm_hs200) >> sm_shift) & 0xff;
			} else {
				sam_dly = rval;
			}
			dev_dbg(mmc_dev(host->mmc),
				"Get speed mode %s clk dly %s ok\n", m_str,
				raw_sm_str);
		} else {
			u32 sm_shift = (frq_index % 4) * 8;

			dev_dbg(mmc_dev(host->mmc), "%s use default value\n",
				m_str);
			rval = ((*raw_sm_def) >> sm_shift) & 0xff;
			if (timing == MMC_TIMING_MMC_HS400) {
				u32 raw_sm_hs200 = 0;

				ds_dly = rval;
				raw_sm_hs200 =
				    mmc_clk_dly[SM3_HS200_SDR104].
				    raw_tm_sm_def[frq_index / 4];
				sam_dly = ((raw_sm_hs200) >> sm_shift) & 0xff;
			} else {
				sam_dly = rval;
			}
		}

	}

	dev_dbg(mmc_dev(host->mmc), "Try set %s clk dly	ok\n", m_str);
	dev_dbg(mmc_dev(host->mmc), "cmd_drv_ph	%d\n", cmd_drv_ph);
	dev_dbg(mmc_dev(host->mmc), "dat_drv_ph	%d\n", dat_drv_ph);
	dev_dbg(mmc_dev(host->mmc), "sam_dly	%d\n", sam_dly);
	dev_dbg(mmc_dev(host->mmc), "ds_dly		%d\n", ds_dly);

	rval = mmc_readl(host, REG_DRV_DL);
	if (cmd_drv_ph)
		rval |= SDXC_CMD_DRV_PH_SEL;	/*180 phase */
	else
		rval &= ~SDXC_CMD_DRV_PH_SEL;	/*90 phase */

	if (dat_drv_ph)
		rval |= SDXC_DAT_DRV_PH_SEL;	/*180 phase */
	else
		rval &= ~SDXC_DAT_DRV_PH_SEL;	/*90 phase */

	mmc_writel(host, REG_DRV_DL, rval);

	rval = mmc_readl(host, REG_SAMP_DL);
	rval &= ~SDXC_SAMP_DL_SW_MASK;
	rval |= sam_dly & SDXC_SAMP_DL_SW_MASK;
	rval |= SDXC_SAMP_DL_SW_EN;
	mmc_writel(host, REG_SAMP_DL, rval);

	rval = mmc_readl(host, REG_DS_DL);
	rval &= ~SDXC_DS_DL_SW_MASK;
	rval |= ds_dly & SDXC_DS_DL_SW_MASK;
	rval |= SDXC_DS_DL_SW_EN;
	mmc_writel(host, REG_DS_DL, rval);

	dev_dbg(mmc_dev(host->mmc), " REG_DRV_DL    %08x\n",
		mmc_readl(host, REG_DRV_DL));
	dev_dbg(mmc_dev(host->mmc), " REG_SAMP_DL  %08x\n",
		mmc_readl(host, REG_SAMP_DL));
	dev_dbg(mmc_dev(host->mmc), " REG_DS_DL      %08x\n",
		mmc_readl(host, REG_DS_DL));

}

static void sunxi_mmc_dump_dly2(struct sunxi_mmc_host *host)
{
	struct sunxi_mmc_clk_dly *mmc_clk_dly =
	    ((struct sunxi_mmc_ver_priv *)host->version_priv_dat)->mmc_clk_dly;
	int i = 0;

	for (i = 0; i < SM_NUM; i++) {
		pr_info("mod_str %s\n", mmc_clk_dly[i].mod_str);
		pr_info("raw_tm_sm_str %s\n", mmc_clk_dly[i].raw_tm_sm_str[0]);
		pr_info("raw_tm_sm_str %s\n", mmc_clk_dly[i].raw_tm_sm_str[1]);
		pr_info("raw_tm_sm0 %x\n", mmc_clk_dly[i].raw_tm_sm[0]);
		pr_info("raw_tm_sm1 %x\n", mmc_clk_dly[i].raw_tm_sm[1]);
		pr_info("********************\n");
	}
}

static void sunxi_mmc_on_off_emce_v4p6x(struct sunxi_mmc_host *host,
		u32 en_crypt, u32 ac_mode, u32 en_emce, int data_len)
{
	u32 rval = 0;
#ifdef CONFIG_SUNXI_EMCE
	sunxi_emce_set_task_des(data_len);
#endif
	rval = mmc_readl(host, REG_EMCE);
	rval &= 0x0000FFFF;
	rval |= (0x200 << 16);
	mmc_writel(host, REG_EMCE, rval);
	rval &= ~(SDXC_EMCE_ENB | SDXC_EMCE_ENCR | SDXC_EMCE_AC_MD);
	if (en_emce)
		rval |= SDXC_EMCE_ENB;
	if (en_crypt)
		rval |= SDXC_EMCE_ENCR;
	if (ac_mode)
		rval |= SDXC_EMCE_AC_MD;
	mmc_writel(host, REG_EMCE, rval);
	dev_dbg(mmc_dev(host->mmc), "%s REG_EMCE:%x\n", __func__,
		mmc_readl(host, REG_EMCE));
#ifdef CONFIG_SUNXI_EMCE
	sunxi_emce_set_task_load(0x1);
#endif
}

static int __sunxi_mmc_do_oclk_onoff(struct sunxi_mmc_host *host, u32 oclk_en,
				     u32 pwr_save, u32 ignore_dat0)
{
	s32 ret = 0;
	u32 rval;

	rval = mmc_readl(host, REG_CLKCR);
	rval &= ~(SDXC_CARD_CLOCK_ON | SDXC_LOW_POWER_ON | SDXC_MASK_DATA0);

	if (oclk_en)
		rval |= SDXC_CARD_CLOCK_ON;
	if (pwr_save)
		rval |= SDXC_LOW_POWER_ON;
	if (ignore_dat0)
		rval |= SDXC_MASK_DATA0;

	mmc_writel(host, REG_CLKCR, rval);

	dev_dbg(mmc_dev(host->mmc), "%s REG_CLKCR:%x\n", __func__,
		mmc_readl(host, REG_CLKCR));

	rval = SDXC_START | SDXC_UPCLK_ONLY | SDXC_WAIT_PRE_OVER;
	mmc_writel(host, REG_CMDR, rval);

	ret = mmc_wbclr(host, REG_CMDR, SDXC_START, 250);

	/* clear irq status bits set by the command */
	mmc_writel(host, REG_RINTR,
		   mmc_readl(host, REG_RINTR) & ~SDXC_SDIO_INTERRUPT);

	if (ret) {
		dev_err(mmc_dev(host->mmc), "fatal err update clk timeout\n");
		return -EIO;
	}

	/*only use mask data0 when update clk,clear it when not update clk */
	if (ignore_dat0)
		mmc_writel(host, REG_CLKCR,
			   mmc_readl(host, REG_CLKCR) & ~SDXC_MASK_DATA0);

	return 0;
}

static int sunxi_mmc_oclk_onoff(struct sunxi_mmc_host *host, u32 oclk_en)
{
	struct device_node *np = NULL;
	struct mmc_host *mmc = host->mmc;
	int pwr_save = 0;
	int len = 0;

	if (!mmc->parent || !mmc->parent->of_node) {
		dev_err(mmc_dev(host->mmc),
			"no dts to parse power save mode\n");
		return -EIO;
	}

	np = mmc->parent->of_node;
	if (of_find_property(np, "sunxi-power-save-mode", &len))
		pwr_save = 1;
	return __sunxi_mmc_do_oclk_onoff(host, oclk_en, pwr_save, 1);
}
int sunxi_mmc_clk_set_rate_for_sdmmc_v4p5x(struct sunxi_mmc_host *host,
					   struct mmc_ios *ios)
{
	u32 mod_clk = 0;
	u32 src_clk = 0;
	u32 rval = 0;
	s32 err = 0;
	u32 rate = 0;
	char *sclk_name = NULL;
	struct clk *mclk = host->clk_mmc;
	struct clk *sclk = NULL;
	struct device *dev = mmc_dev(host->mmc);
	int div = 0;

	if (ios->clock == 0) {
		__sunxi_mmc_do_oclk_onoff(host, 0, 0, 1);
		return 0;
	}

	if ((ios->bus_width == MMC_BUS_WIDTH_8)
	    && (ios->timing == MMC_TIMING_MMC_DDR52)
	    ) {
		mod_clk = ios->clock << 2;
		div = 1;
	} else {
		mod_clk = ios->clock << 1;
		div = 0;
	}

	sclk = clk_get(dev, "osc24m");
	sclk_name = "osc24m";
	if (IS_ERR(sclk)) {
		dev_err(mmc_dev(host->mmc), "Error to get source clock %s\n",
			sclk_name);
		return -1;
	}

	src_clk = clk_get_rate(sclk);
	if (mod_clk > src_clk) {
		clk_put(sclk);
		sclk = clk_get(dev, "pll_periph");
		sclk_name = "pll_periph";
	}
	if (IS_ERR(sclk)) {
		dev_err(mmc_dev(host->mmc), "Error to get source clock %s\n",
			sclk_name);
		return -1;
	}

	sunxi_mmc_oclk_onoff(host, 0);

	err = clk_set_parent(mclk, sclk);
	if (err) {
		dev_err(mmc_dev(host->mmc), "set parent failed\n");
		clk_put(sclk);
		return -1;
	}

	rate = clk_round_rate(mclk, mod_clk);

	dev_dbg(mmc_dev(host->mmc), "get round rate %d\n", rate);

	clk_disable_unprepare(host->clk_mmc);

	err = clk_set_rate(mclk, rate);
	if (err) {
		dev_err(mmc_dev(host->mmc), "set mclk rate error, rate %dHz\n",
			rate);
		clk_put(sclk);
		return -1;
	}

	rval = clk_prepare_enable(host->clk_mmc);
	if (rval) {
		dev_err(mmc_dev(host->mmc), "Enable mmc clk err %d\n", rval);
		return -1;
	}

	src_clk = clk_get_rate(sclk);
	clk_put(sclk);

	dev_dbg(mmc_dev(host->mmc), "set round clock %d, soure clk is %d\n",
		rate, src_clk);

#ifdef MMC_FPGA
	if ((ios->bus_width == MMC_BUS_WIDTH_8)
	    && (ios->timing == MMC_TIMING_MMC_DDR52)
	    ) {
		/* clear internal divider */
		rval = mmc_readl(host, REG_CLKCR);
		rval &= ~0xff;
		rval |= 1;
	} else {
		/* support internal divide clock under fpga environment  */
		rval = mmc_readl(host, REG_CLKCR);
		rval &= ~0xff;
		rval |= 24000000 / mod_clk / 2;	/* =24M/400K/2=0x1E */
	}
	mmc_writel(host, REG_CLKCR, rval);
	dev_info(mmc_dev(host->mmc), "--FPGA REG_CLKCR: 0x%08x\n",
		 mmc_readl(host, REG_CLKCR));
#else
	/* clear internal divider */
	rval = mmc_readl(host, REG_CLKCR);
	rval &= ~0xff;
	rval |= div;
	mmc_writel(host, REG_CLKCR, rval);
#endif

	if ((ios->bus_width == MMC_BUS_WIDTH_8)
	    && (ios->timing == MMC_TIMING_MMC_HS400)
	    ) {
		rval = mmc_readl(host, REG_EDSD);
		rval |= SDXC_HS400_MD_EN;
		mmc_writel(host, REG_EDSD, rval);
		rval = mmc_readl(host, REG_CSDC);
		rval &= ~SDXC_CRC_DET_PARA_MASK;
		rval |= SDXC_CRC_DET_PARA_HS400;
		mmc_writel(host, REG_CSDC, rval);
	} else {
		rval = mmc_readl(host, REG_EDSD);
		rval &= ~SDXC_HS400_MD_EN;
		mmc_writel(host, REG_EDSD, rval);
		rval = mmc_readl(host, REG_CSDC);
		rval &= ~SDXC_CRC_DET_PARA_MASK;
		rval |= SDXC_CRC_DET_PARA_OTHER;
		mmc_writel(host, REG_CSDC, rval);
	}
	dev_dbg(mmc_dev(host->mmc), "SDXC_REG_EDSD: 0x%08x\n",
		mmc_readl(host, REG_EDSD));
	dev_dbg(mmc_dev(host->mmc), "SDXC_REG_CSDC: 0x%08x\n",
		mmc_readl(host, REG_CSDC));

	/*sunxi_of_parse_clk_dly(host); */
	if ((ios->bus_width == MMC_BUS_WIDTH_8)
	    && (ios->timing == MMC_TIMING_MMC_DDR52)
	    ) {
		ios->clock = rate >> 2;
	} else {
		ios->clock = rate >> 1;
	}

	sunxi_mmc_set_clk_dly(host, ios->clock, ios->bus_width, ios->timing);

	return sunxi_mmc_oclk_onoff(host, 1);
}

void sunxi_mmc_thld_ctl_for_sdmmc_v4p5x(struct sunxi_mmc_host *host,
					struct mmc_ios *ios,
					struct mmc_data *data)
{
	u32 bsz = data->blksz;
	/*unit:byte */
	u32 tdtl = (host->dma_tl & SDXC_TX_TL_MASK) << 2;
	/*unit:byte */
	u32 rdtl = ((host->dma_tl & SDXC_RX_TL_MASK) >> 16) << 2;
	u32 rval = 0;

	if ((data->flags & MMC_DATA_WRITE)
	    && (bsz <= SDXC_CARD_RD_THLD_SIZE)
	    && (bsz <= tdtl)) {
		rval = mmc_readl(host, REG_THLD);
		rval &= ~SDXC_CARD_RD_THLD_MASK;
		rval |= data->blksz << SDXC_CARD_RD_THLD_SIZE_SHIFT;
		rval |= SDXC_CARD_WR_THLD_ENB;
		mmc_writel(host, REG_THLD, rval);
	} else {
		rval = mmc_readl(host, REG_THLD);
		rval &= ~SDXC_CARD_WR_THLD_ENB;
		mmc_writel(host, REG_THLD, rval);
	}

	if ((data->flags & MMC_DATA_READ)
	    && (bsz <= SDXC_CARD_RD_THLD_SIZE)
	    /*((SDXC_FIFO_DETH<<2)-bsz) >= (rdtl) */
	    && ((SDXC_FIFO_DETH << 2) >= (rdtl + bsz))
	    && ((ios->timing == MMC_TIMING_MMC_HS200)
	       || (ios->timing == MMC_TIMING_MMC_HS400))) {
		rval = mmc_readl(host, REG_THLD);
		rval &= ~SDXC_CARD_RD_THLD_MASK;
		rval |= data->blksz << SDXC_CARD_RD_THLD_SIZE_SHIFT;
		rval |= SDXC_CARD_RD_THLD_ENB;
		mmc_writel(host, REG_THLD, rval);
	} else {
		rval = mmc_readl(host, REG_THLD);
		rval &= ~SDXC_CARD_RD_THLD_ENB;
		mmc_writel(host, REG_THLD, rval);
	}

	dev_dbg(mmc_dev(host->mmc), "--SDXC_REG_THLD: 0x%08x\n",
		mmc_readl(host, REG_THLD));

}

void sunxi_mmc_save_spec_reg_v4p5x(struct sunxi_mmc_host *host)
{
	struct sunxi_mmc_spec_regs *spec_regs =
	    &((struct sunxi_mmc_ver_priv *)(host->version_priv_dat))->
	    bak_spec_regs;
	spec_regs->drv_dl = mmc_readl(host, REG_DRV_DL);
	spec_regs->samp_dl = mmc_readl(host, REG_SAMP_DL);
	spec_regs->ds_dl = mmc_readl(host, REG_DS_DL);
	/*bak_spec_regs.sd_ntsr = mmc_readl(host,REG_SD_NTSR); */
	spec_regs->edsd = mmc_readl(host, REG_EDSD);
	spec_regs->csdc = mmc_readl(host, REG_CSDC);
}

void sunxi_mmc_restore_spec_reg_v4p5x(struct sunxi_mmc_host *host)
{
	struct sunxi_mmc_spec_regs *spec_regs =
	    &((struct sunxi_mmc_ver_priv *)(host->version_priv_dat))->
	    bak_spec_regs;
	mmc_writel(host, REG_DRV_DL, spec_regs->drv_dl);
	mmc_writel(host, REG_SAMP_DL, spec_regs->samp_dl);
	mmc_writel(host, REG_DS_DL, spec_regs->ds_dl);
	/*mmc_writel(host,REG_SD_NTSR,bak_spec_regs.sd_ntsr); */
	mmc_writel(host, REG_EDSD, spec_regs->edsd);
	mmc_writel(host, REG_CSDC, spec_regs->csdc);
}


void sunxi_mmc_init_priv_v4p5x(struct sunxi_mmc_host *host,
			       struct platform_device *pdev, int phy_index)
{
	struct sunxi_mmc_ver_priv *ver_priv =
	    devm_kzalloc(&pdev->dev, sizeof(struct sunxi_mmc_ver_priv),
			 GFP_KERNEL);
	host->version_priv_dat = ver_priv;

	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].spm = SM0_DS26_SDR12;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].mod_str = "DS26_SDR12";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_str[0] =
	    "sdc_tm4_sm0_freq0";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_str[1] =
	    "sdc_tm4_sm0_freq1";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].spm = SM1_HSSDR52_SDR25;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].mod_str = "HSSDR52_SDR25";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_str[0] =
	    "sdc_tm4_sm1_freq0";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_str[1] =
	    "sdc_tm4_sm1_freq1";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].spm = SM2_HSDDR52_DDR50;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].mod_str = "HSDDR52_DDR50";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_str[0] =
	    "sdc_tm4_sm2_freq0";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_str[1] =
	    "sdc_tm4_sm2_freq1";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].spm = SM3_HS200_SDR104;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].mod_str = "HS200_SDR104";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_str[0] =
	    "sdc_tm4_sm3_freq0";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_str[1] =
	    "sdc_tm4_sm3_freq1";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_def[1] = 0x00000405;

	ver_priv->mmc_clk_dly[SM4_HS400].spm = SM4_HS400;
	ver_priv->mmc_clk_dly[SM4_HS400].mod_str = "HS400";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_str[0] = "sdc_tm4_sm4_freq0";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_str[1] = "sdc_tm4_sm4_freq1";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm[1] = 0x00000608;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_def[1] = 0x00000408;

	host->sunxi_mmc_clk_set_rate = sunxi_mmc_clk_set_rate_for_sdmmc_v4p5x;
	/*host->dma_tl = (0x2<<28)|(7<<16)|248; */
	host->dma_tl = SUNXI_DMA_TL_SDMMC_V4P5X;
	/*host->idma_des_size_bits = 15; */
	host->idma_des_size_bits = SUNXI_DES_SIZE_SDMMC_V4P5X;
	host->sunxi_mmc_thld_ctl = sunxi_mmc_thld_ctl_for_sdmmc_v4p5x;
	host->sunxi_mmc_save_spec_reg = sunxi_mmc_save_spec_reg_v4p5x;
	host->sunxi_mmc_restore_spec_reg = sunxi_mmc_restore_spec_reg_v4p5x;
	sunxi_mmc_reg_ex_res_inter(host, phy_index);
	host->sunxi_mmc_set_acmda = sunxi_mmc_set_a12a;
	host->sunxi_mmc_dump_dly_table = sunxi_mmc_dump_dly2;
	host->phy_index = phy_index;

	host->sunxi_mmc_oclk_en = sunxi_mmc_oclk_onoff;
}

void sunxi_mmc_init_priv_v4p6x(struct sunxi_mmc_host *host,
			       struct platform_device *pdev, int phy_index)
{
	struct sunxi_mmc_ver_priv *ver_priv =
	    devm_kzalloc(&pdev->dev, sizeof(struct sunxi_mmc_ver_priv),
			 GFP_KERNEL);
	host->version_priv_dat = ver_priv;

	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].spm = SM0_DS26_SDR12;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].mod_str = "DS26_SDR12";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_str[0] =
	    "sdc_tm4_sm0_freq0";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_str[1] =
	    "sdc_tm4_sm0_freq1";
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM0_DS26_SDR12].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].spm = SM1_HSSDR52_SDR25;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].mod_str = "HSSDR52_SDR25";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_str[0] =
	    "sdc_tm4_sm1_freq0";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_str[1] =
	    "sdc_tm4_sm1_freq1";
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM1_HSSDR52_SDR25].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].spm = SM2_HSDDR52_DDR50;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].mod_str = "HSDDR52_DDR50";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_str[0] =
	    "sdc_tm4_sm2_freq0";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_str[1] =
	    "sdc_tm4_sm2_freq1";
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM2_HSDDR52_DDR50].raw_tm_sm_def[1] = 0;

	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].spm = SM3_HS200_SDR104;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].mod_str = "HS200_SDR104";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_str[0] =
	    "sdc_tm4_sm3_freq0";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_str[1] =
	    "sdc_tm4_sm3_freq1";
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm[1] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM3_HS200_SDR104].raw_tm_sm_def[1] = 0x00000405;

	ver_priv->mmc_clk_dly[SM4_HS400].spm = SM4_HS400;
	ver_priv->mmc_clk_dly[SM4_HS400].mod_str = "HS400";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_str[0] = "sdc_tm4_sm4_freq0";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_str[1] = "sdc_tm4_sm4_freq1";
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm[0] = 0;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm[1] = 0x00000608;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_def[0] = 0;
	ver_priv->mmc_clk_dly[SM4_HS400].raw_tm_sm_def[1] = 0x00000408;

	host->sunxi_mmc_clk_set_rate = sunxi_mmc_clk_set_rate_for_sdmmc_v4p5x;
	/*host->dma_tl = (0x2<<28)|(7<<16)|248; */
	host->dma_tl = SUNXI_DMA_TL_SDMMC_V4P5X;
	/*host->idma_des_size_bits = 15; */
	host->idma_des_size_bits = SUNXI_DES_SIZE_SDMMC_V4P5X;
	host->sunxi_mmc_thld_ctl = sunxi_mmc_thld_ctl_for_sdmmc_v4p5x;
	host->sunxi_mmc_save_spec_reg = sunxi_mmc_save_spec_reg_v4p5x;
	host->sunxi_mmc_restore_spec_reg = sunxi_mmc_restore_spec_reg_v4p5x;
	sunxi_mmc_reg_ex_res_inter(host, phy_index);
	host->sunxi_mmc_set_acmda = sunxi_mmc_set_a12a;
	host->sunxi_mmc_dump_dly_table = sunxi_mmc_dump_dly2;
	host->phy_index = phy_index;
	if (mmc_readl(host, REG_SMCV) >= SMHC_VERSION_V4P7)
		host->sunxi_mmc_on_off_emce = sunxi_mmc_on_off_emce_v4p6x;

	host->sunxi_mmc_oclk_en = sunxi_mmc_oclk_onoff;
}
