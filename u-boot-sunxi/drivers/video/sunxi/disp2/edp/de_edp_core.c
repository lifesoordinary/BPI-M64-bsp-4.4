/* de_edp_core.c
 *
 * Copyright (c) 2007-2017 Allwinnertech Co., Ltd.
 * Author: zhengxiaobin <zhengxiaobin@allwinnertech.com>
 *
 * core function of edp driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include "de_edp_core.h"
#include "de_edp_hal.h"
#include "de_edp_config.h"
#include "de_edp.h"

int eDP_capable;
int dp_rev, dp_enhanced_frame_cap, dp_max_lane_count;
unsigned long long dp_max_link_rate;

char dp_rx_info[256];

int lane_sel, speed_sel;

/**
 * @name       :dp_get_sink_info
 * @brief      :get sink info from global var
 * @param[IN]  :sink_inf
 * @param[OUT] :
 * @return     :0 if success
 */
int dp_get_sink_info(struct sink_info *sink_inf)
{
	s32 ret = -1;

	if (!sink_inf) {
		edp_wrn("null hdl\n");
		goto OUT;
	}
	sink_inf->dp_rev = dp_rev;
	sink_inf->eDP_capable = eDP_capable;
	sink_inf->dp_enhanced_frame_cap = dp_enhanced_frame_cap;
	sink_inf->dp_max_link_rate = dp_max_link_rate;
	sink_inf->dp_max_lane_count = dp_max_lane_count;
	ret = 0;
OUT:
	return ret;
}

/**
 * @name       :dp_sink_init
 * @brief      :dp sink device init,note:call after phy_init and phy_cfg
 * @param[IN]  :sel index of edp
 * @return     :0 if success
 */
int dp_sink_init(u32 sel)
{
	int i, blk, ret = 0;

	/*link configuration*/
	blk = 8;
	/* read 16 Byte dp sink capability */
	for (i = 0; i < 16 / blk; i++) {
		ret = aux_rd(sel, 0x0000 + i * blk, blk, dp_rx_info + i * blk);
		if (ret == -1)
			return ret;
	}

#if EDP_DEBUG_LEVEL == 2
	edp_wrn("DPCD version:%d.%d\n", (dp_rx_info[0] >> 4) & 0x0f,
		(dp_rx_info[0] >> 4));

	if (dp_rx_info[1] == 0x06)
		edp_wrn("sink max bit rate:1.62Gbps\n");
	else if (dp_rx_info[1] == 0x0a)
		edp_wrn("sink max bit rate:2.7Gbps\n");
	else if (dp_rx_info[1] == 0x14)
		edp_wrn("sink max bit rate:5.4Gbps\n");

	edp_wrn("sink max lane count:%d\n", dp_rx_info[2] & 0xf);
	if (dp_rx_info[2] & 0x80)
		edp_wrn("enhanced mode:support\n");
	else
		edp_wrn("enhanced mode:not support\n");

	if (dp_rx_info[3] & 0x1)
		edp_wrn("down spread:up to 0.5\n");
	else
		edp_wrn("down spread:no\n");
	if (dp_rx_info[3] & (1 << 6))
		edp_wrn("aux handshake link training:not require\n");
	else
		edp_wrn("aux handshake link training:require\n");

	edp_wrn("number of receiver ports:%d\n", dp_rx_info[4] + 1);

	if (dp_rx_info[5] & 0x1)
		edp_wrn("downstream port:present\n");
	else
		edp_wrn("downstream port:not present\n");

	if (((dp_rx_info[5] >> 1) & 0x3) == 0)
		edp_wrn("downstream port type:DisplayPort\n");
	else if (((dp_rx_info[5] >> 1) & 0x3) == 1)
		edp_wrn("downstream port type:VGA or DVI-I\n");
	else if (((dp_rx_info[5] >> 1) & 0x3) == 2)
		edp_wrn("downstream port type:DVI or HDMI\n");
	else if (((dp_rx_info[5] >> 1) & 0x3) == 3)
		edp_wrn("downstream port type:Others\n");
	if (dp_rx_info[5] & 0x4)
		edp_wrn("format conversion block:support\n");
	else
		edp_wrn("format conversion block:not support\n");

	if (dp_rx_info[6] & 0x1)
		edp_wrn("Main Link channel Coding:support ANSI 8B\\10B\n");
	else
		edp_wrn("Main Link channel Coding:not support ANSI 8B\\10B\n");

	edp_wrn("Downstream port count:%d\n", dp_rx_info[7] & 0xf);
	if (dp_rx_info[7] & 0x80)
		edp_wrn("OUI:support\n");
	else
		edp_wrn("OUI:not support\n");

	if (dp_rx_info[8] & (1 << 1))
		edp_wrn("ReceiverPort0 Capability_0:Has a local EDID\n");
	else
		edp_wrn("ReceiverPort0 Capability_0:Not has a local EDID\n");
	if (dp_rx_info[8] & (1 << 2))
		edp_wrn("ReceiverPort0 Capability_0:sec isochronous stream\n");
	else
		edp_wrn("ReceiverPort0 Capability_0:main isochronous stream\n");

	edp_wrn("ReceiverPort0 Capability_1:Buffer size per lane is %d bytes\n",
		32 * (dp_rx_info[9] + 1));

	if ((dp_rx_info[4] + 1) > 1) {
		if (dp_rx_info[10] & (1 << 1))
			edp_wrn(
			    "ReceiverPort1 Capability0:Has a local EDID\n");
		else
			edp_wrn("ReceiverPort1 Capability0:local EDID exist\n");
		if (dp_rx_info[10] & (1 << 2))
			edp_wrn(
			"ReceiverPort1 Capability0:sec isochronous stream\n");
		else
			edp_wrn(
			"ReceiverPort1 Capability0:main isochronous stream\n");

		edp_wrn("ReceiverPort1 Capability1:Buffer size per lane:%dB\n");
			32 * (dp_rx_info[11] + 1));
	}
#endif

	fp_tx_buf[0] = 0x01;
	/*set sink to D0 mode(Normal Operation Mode)*/
	/*--- DP_PWR keeps the default 3.3V*/
	ret = aux_wr(sel, 0x00600, 1,
		     fp_tx_buf);

	if (ret == -1)
		return ret;

	dp_rev = dp_rx_info[0] & 0x0f;
	dp_max_link_rate = dp_rx_info[1] * 270000000ul; /* 270M */
	dp_max_lane_count = dp_rx_info[2] & 0x0f;
	sink_ssc_flag = dp_rx_info[3] & 0x1;

	/* eDP_CONFIGURATION_CAP */
	/* Always reads 0x00 for external receivers */
	if (dp_rx_info[0x0d] != 0) {
		eDP_capable = 1;
		edp_dbg("Sink device is eDP receiver!\n");
	} else {
		eDP_capable = 0;
		edp_dbg("Sink device is external receiver!\n");
	}

	switch (dp_rx_info[0x0e]) {
	case 0x00:
		/*Link Status/Adjust Request read interval during CR*/
		/*phase --- 100us*/
		training_aux_rd_interval_CR = 100;
		/*Link Status/Adjust Request read interval during EQ*/
		/*phase --- 400us*/
		training_aux_rd_interval_EQ = 400;

		break;
	case 0x01:
		training_aux_rd_interval_CR = 100;
		training_aux_rd_interval_EQ = 4000;
		break;
	case 0x02:
		training_aux_rd_interval_CR = 100;
		training_aux_rd_interval_EQ = 8000;
		break;
	case 0x03:
		training_aux_rd_interval_CR = 100;
		training_aux_rd_interval_EQ = 12000;
		break;
	case 0x04:
		training_aux_rd_interval_CR = 100;
		training_aux_rd_interval_EQ = 16000;
		break;
	default:
		training_aux_rd_interval_CR = 100;
		training_aux_rd_interval_EQ = 400;
	}

	/*link configuration*/
	/* set bandwidth  = 1.62G */
	fp_tx_buf[0] = glb_bit_rate / 270000000;
	ret = aux_wr(sel, 0x00100, 1, fp_tx_buf);
	if (ret == -1)
		return ret;

	/*DPTX ENHANCED_FRAME_EN shall be set to 1 when DPRX*/
	/*ENHANCED_FRAME_CAP is set*/
	if ((dp_rev == 1) && (dp_rx_info[2] & 0x80)) {
		dp_enhanced_frame_cap = 1;
		fp_tx_buf[0] = 0x80 | glb_lane_cnt;
		edp_dbg("Enhanced frame is enable!\n");
	} else if (dp_rev == 2) {
		dp_enhanced_frame_cap = 1;
		fp_tx_buf[0] = 0x80 | glb_lane_cnt;
		edp_dbg("Enhanced frame is enable!\n");
	} else {
		dp_enhanced_frame_cap = 0;
		fp_tx_buf[0] = 0x00 | glb_lane_cnt;
		edp_dbg("Default frame is enable!\n");
	}
	ret = aux_wr(sel, 0x00101, 1, fp_tx_buf); /* set lane count */
	if (ret == -1)
		return ret;

	return 0;
}

/**
 * @name       :dp_video_set
 * @brief      :dp video setting function --- call after edp_hal_ctrl_init and
 *		dp_sink_init
 * @param[IN]  :tmg video timing parameter
 * @param[IN]  :src video source of edp.0:RGB input,1:embedded Color bar
 *		2:Mosaic
 * @return     :0:successful; -1:fps or vt error; -2:symbol per line error;
 *		-3:tu ratio error
 */
int dp_video_set(u32 sel, struct disp_video_timings *tmg, u32 fps,
		 enum edp_video_src_t src)
{
	struct video_para para;
	unsigned int dly, total_symbol_per_line;
	/*unsigned int video_ht_symbol_per_lane, ht_ratio, ht_per_lane;*/
	int ret_val;

	/*total_symbol_per_line = ((tmg->ht- tmg->ht/8) * 4 */
	/*glb_lane_cnt);*/
	total_symbol_per_line =
	    ((glb_bit_rate / 10) / (tmg->ver_total_time * fps));
	/* depend on vblank and dram mdfs needs */
	dly = tmg->ver_total_time - tmg->y_res - 10;
	/*total_symbol_per_line = 4000;*/
	/*dly = STA_DLY;*/

	if (((tmg->hor_total_time) * (tmg->ver_total_time) * fps) > SRC_VIDEO) {
		edp_wrn("Warn: Video is excess boundary!\n");
		total_symbol_per_line =
		    ((tmg->hor_total_time) * (glb_bit_rate / 10)) / SRC_VIDEO;
	}
#if 0
	/*TX Total Bit >= RX Total Bit*/
	video_ht_symbol_per_lane =
	    tmg->hor_total_time * (3 * COLOR_MODE) / (glb_lane_cnt * 10);
	/*TU FILL is >= 0*/
	ht_ratio =
	    (tmg->hor_total_time * ((3 * COLOR_MODE) / 8)) / glb_lane_cnt;
	ht_per_lane = max(video_ht_symbol_per_lane, ht_ratio);
	if (total_symbol_per_line < ht_per_lane) {
		edp_wrn("total_symbol_per_line is invalid!\n");
		total_symbol_per_line = ht_per_lane;
	}
#endif

	ret_val = para_convert(glb_lane_cnt, COLOR_MODE, total_symbol_per_line,
			       tmg->hor_total_time, &para);
	if (ret_val != RET_OK) {
		edp_wrn("Current Lane Parameter is Wrong!\n");
		return RET_FAIL;
	}

	edp_hal_video_para_cfg(sel, &para, COLOR_MODE, src, dly,
			  total_symbol_per_line);
	edp_hal_video_timing_cfg(sel, tmg);

	if (CLOCK_MODE == 0)
		edp_dbg("Link and main video stream clock are asyn!\n");
	else
		edp_dbg("Link and main video stream clock are sync!\n");

	if (eDP_capable)
		edp_hal_dp_scramble_seed(sel, 0xfffe);
	else
		edp_hal_dp_scramble_seed(sel, 0xffff);

	if (dp_enhanced_frame_cap)
		edp_hal_dp_framing_mode(sel, 1);
	else
		edp_hal_dp_framing_mode(sel, 0);

	return RET_OK;
}

int para_convert(int lanes, int color_bits, int ht_per_lane, int ht,
		 struct video_para *result)
{
	int Mvid, Nvid;
	int K;
	unsigned int ratio;

	K = cdiv(ht, ht_per_lane);
	Mvid = ht / K;
	Nvid = ht_per_lane / K;
	/*Align to it*/
	ratio = ((3 * color_bits * Mvid << 10) + (lanes * 8 * Nvid) / 2) /
		(lanes * 8 * Nvid);
	/*ratio = (3 * color_bits * Mvid << 10)/(lanes * 8 * Nvid);*/

	/* ratio = Stream Rate/Link Symbol Rate */
	if (ratio > 0x400) {
		edp_wrn("Valid Data Symbol is excess the TU Size!\n");
		return RET_FAIL;
	}
	/*For Division By shift 10*/
	result->fill = (TU_SIZE * (0x400 - ratio)) >> 10;
	result->mvid = Mvid;
	result->nvid = Nvid;

	return RET_OK;
}

/*for clock recovery*/
int dp_tps1_test(u32 sel, u32 swing_lv, u32 pre_lv, u8 is_swing_max,
		 u8 is_pre_max)
{
	unsigned int to_cnt;
	int ret;

	edp_hal_link_training_ctrl(sel, 1, 0, 1);
	mdelay(1);
	phy_cfg(sel, swing_lv, pre_lv, 0);
	to_cnt = TRAIN_CNT;

	while (1) {
		/*set pattern 1 with scramble disable*/
		fp_tx_buf[0] = 0x21;
		/*set pattern 1 with max swing and emphasis 0x13*/
		fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) |
			       ((pre_lv & 0x3) << 3) |
			       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
		fp_tx_buf[2] = fp_tx_buf[1];
		fp_tx_buf[3] = fp_tx_buf[1];
		fp_tx_buf[4] = fp_tx_buf[1];
		ret = aux_wr(sel, 0x00102, 5, fp_tx_buf);
		if (ret != RET_OK)
			return RET_FAIL;

		/*wait for the training finish*/
		udelay(training_aux_rd_interval_CR);

		ret = aux_rd(sel, 0x0202, 2, fp_rx_buf);
		if (ret == -1)
			return RET_FAIL;
#if EDP_BYPASS_CR_TRANNING == 1
		return RET_OK;
#endif

		if (glb_lane_cnt == 1) {
			if ((fp_rx_buf[0] & 0x01) == 0x01)
				return RET_OK;
		}

		if (glb_lane_cnt == 2) {
			if ((fp_rx_buf[0] & 0x11) == 0x11)
				return RET_OK;
		}
		if (glb_lane_cnt == 4) {
			if (((fp_rx_buf[0] & 0x11) == 0x11) &&
			    ((fp_rx_buf[1] & 0x11) == 0x11))
				return RET_OK;
		}

		to_cnt--;
		if (to_cnt == 0) {
			ret = aux_rd(sel, 0x0206, 2, fp_rx_buf);
			if (((fp_rx_buf[0] & (0x3 << 0)) > swing_lv) ||
			    ((fp_rx_buf[1] & (0x3 << 4)) > swing_lv)) {
				edp_dbg("DPTX adjust the drive swing level!\n");
			}

			if (((fp_rx_buf[0] & (0x3 << 2)) > pre_lv) ||
			    ((fp_rx_buf[1] & (0x3 << 6)) > pre_lv)) {
				edp_dbg(
				    "DPTX adjust the pre-emphasize level!\n");
			}

			return RET_FAIL;
		}
	}
	return ret;
}

s32 dp_tps2_test(u32 sel, u32 swing_lv, u32 pre_lv, u8 is_swing_max,
		 u8 is_pre_max)
{
	s32 result, ret;
	u32 to_cnt;

	edp_hal_link_training_ctrl(sel, 1, 0, 2);
	mdelay(1);
	phy_cfg(sel, swing_lv, pre_lv, 0);
	mdelay(1);

	result = RET_FAIL;
	to_cnt = TRAIN_CNT + 1;

	while (1) {
		fp_tx_buf[0] = 0x22; /* set pattern 2 with scramble disable*/
		fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) |
			       ((pre_lv & 0x3) << 3) |
			       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
		fp_tx_buf[2] = fp_tx_buf[1];
		fp_tx_buf[3] = fp_tx_buf[1];
		fp_tx_buf[4] = fp_tx_buf[1];
		ret = aux_wr(sel, 0x00102, 5, fp_tx_buf);
		if (ret != RET_OK)
			return RET_FAIL;

		/*wait for the training finish*/
		if (training_aux_rd_interval_EQ > 1)
			udelay(training_aux_rd_interval_EQ);
		else
			udelay(400);

		ret = aux_rd(sel, 0x0202, 2,
			     fp_rx_buf); /* Reading the link status bits */
		if (ret != RET_OK)
			return RET_FAIL;

#if EDP_BYPASS_EQ_TRANNING == 1
		result = 0;
		goto tps2_end;
#endif
		/* DPCD Link Status Field ---
		 * lANEX_CHANNEL_EQ_DOWN;LANEX_SYMBOL_LOCKED;
		 * INTERLANE_ALIGN_DONE
		 */
		if (glb_lane_cnt == 1) {
			if ((fp_rx_buf[0] & 0x07) == 0x07) {
				result = 0;
				goto tps2_end;
			}
		}

		if (glb_lane_cnt == 2) {
			if ((fp_rx_buf[0] & 0x77) == 0x77) {
				result = 0;
				goto tps2_end;
			}
		}

		if (glb_lane_cnt == 4) {
			if (((fp_rx_buf[0] & 0x77) == 0x77) &&
			    ((fp_rx_buf[1] & 0x77) == 0x77)) {
				result = 0;
				goto tps2_end;
			}
		}

		to_cnt--;
		if (to_cnt == 0) {
			ret = aux_rd(sel, 0x0206, 2, fp_rx_buf);
			if (ret != RET_OK)
				return RET_FAIL;

			if (((fp_rx_buf[0] & (0x3 << 0)) > swing_lv) ||
			    ((fp_rx_buf[1] & (0x3 << 4)) > swing_lv)) {
				edp_dbg("DPTX adjust the drive swing level!\n");
			}

			if (((fp_rx_buf[0] & (0x3 << 2)) > pre_lv) ||
			    ((fp_rx_buf[1] & (0x3 << 6)) > pre_lv)) {
				edp_dbg(
				    "DPTX adjust the pre-emphasize level!\n");
			}

			return RET_FAIL;
		}
	}

tps2_end:
	fp_tx_buf[0] = 0x00; /* 102 --- indicate the end of training*/
	fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) | ((pre_lv & 0x3) << 3) |
		       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
	fp_tx_buf[2] = fp_tx_buf[1];
	fp_tx_buf[3] = fp_tx_buf[1];
	fp_tx_buf[4] = fp_tx_buf[1];

	fp_tx_buf[5] = 0x00;  /* 107*/
	fp_tx_buf[6] = 0x01;  /* 108*/
	fp_tx_buf[7] = 0x00;  /* 109*/
	if (eDP_capable)
		fp_tx_buf[8] = 0x01;
	else
		fp_tx_buf[8] = 0x00;
	fp_tx_buf[9] = 0x00;  /* 10b*/
	fp_tx_buf[10] = 0x00; /* 10c*/
	fp_tx_buf[11] = 0x00; /* 10d*/
	fp_tx_buf[12] = 0x00; /* 10e*/
	ret = aux_wr(sel, 0x0102, 13, fp_tx_buf);
	if (ret != RET_OK)
		return RET_FAIL;

	return result;
}

s32 dp_quality_test(u32 sel, u32 swing_lv, u32 pre_lv, u8 is_swing_max,
		    u8 is_pre_max)
{
	s32 i, to_cnt;
	s32 temp1, temp2;
	u32 Meas_Tim;
	u32 error_cnt;

	to_cnt = TRAIN_CNT;
	while (1) {
dq_quality_start:
		/* quality pattern 2 --- Symbol Error Rate Measurement */
		edp_hal_link_training_ctrl(sel, 1, 2, 0);
		fp_tx_buf[0] = 0x00; /* 102*/
		fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) |
			       ((pre_lv & 0x3) << 3) |
			       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
		fp_tx_buf[2] = fp_tx_buf[1];
		fp_tx_buf[3] = fp_tx_buf[1];
		fp_tx_buf[4] = fp_tx_buf[1];
		fp_tx_buf[5] = 0x00;  /* 107*/
		fp_tx_buf[6] = 0x01;  /* 108*/
		fp_tx_buf[7] = 0x00;  /* 109*/
		fp_tx_buf[8] = 0x00;  /* 10a*/
		fp_tx_buf[9] = 0x02;  /* 10b*/
		fp_tx_buf[10] = 0x02; /* 10c*/
		fp_tx_buf[11] = 0x02; /* 10d*/
		fp_tx_buf[12] = 0x02; /* 10e*/
		aux_wr(sel, 0x0102, 13, fp_tx_buf);

		Meas_Tim = 100;
		edp_hal_delay_ms(sel, Meas_Tim);
		aux_rd(sel, 0x0210, 8, fp_rx_buf);
		/*Measure_Period = 1.365 * Meas_Tim;*/

		to_cnt--;
		for (i = 0; i < 7;) {
			temp1 = fp_rx_buf[i];
			temp2 = fp_rx_buf[i + 1] << 8;
			temp2 &= ~0x8000;
			error_cnt = temp1 + temp2;

			if (error_cnt < SYMBOL_ERROR_PERMIT)
				i += 2;
			else if (to_cnt > 0)
				goto dq_quality_start;
			else
				goto dq_quality_end;
		}

		/*edp_hal_delay_ms(sel, 200);*/
		/*aux_rd(sel, 0x0210,8,fp_rx_buf);*/

		edp_hal_link_training_ctrl(sel, 1, 3,
				   0); /* quality pattern 3 --- PRBS7 */
		fp_tx_buf[0] = 0x00;   /* 102*/
		fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) |
			       ((pre_lv & 0x3) << 3) |
			       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
		fp_tx_buf[2] = fp_tx_buf[1];
		fp_tx_buf[3] = fp_tx_buf[1];
		fp_tx_buf[4] = fp_tx_buf[1];
		fp_tx_buf[5] = 0x00;  /* 107*/
		fp_tx_buf[6] = 0x01;  /* 108*/
		fp_tx_buf[7] = 0x00;  /* 109*/
		fp_tx_buf[8] = 0x00;  /* 10a*/
		fp_tx_buf[9] = 0x03;  /* 10b*/
		fp_tx_buf[10] = 0x03; /* 10c*/
		fp_tx_buf[11] = 0x03; /* 10d*/
		fp_tx_buf[12] = 0x03; /* 10e*/
		aux_wr(sel, 0x0102, 13, fp_tx_buf);

		Meas_Tim = 100;
		edp_hal_delay_ms(sel, Meas_Tim);
		aux_rd(sel, 0x0210, 8, fp_rx_buf);
		/*Measure_Period = 1.365 * Meas_Tim;*/
		for (i = 0; i < 7;) {
			temp1 = fp_rx_buf[i];
			temp2 = fp_rx_buf[i + 1] << 8;
			temp2 &= ~0x8000;
			error_cnt = temp1 + temp2;

			if (error_cnt < 127)
				i += 2;
			else if (to_cnt > 0)
				goto dq_quality_start;
			else
				goto dq_quality_end;
		}

dq_quality_end:
		if (i == 8)
			break;
		return -1;

		/*edp_hal_delay_ms(sel, 100);*/
		/*aux_rd(sel, 0x0210,8,fp_rx_buf);*/
	}

	fp_tx_buf[0] = 0x00; /* 102*/

	fp_tx_buf[1] = ((is_pre_max & 0x1) << 5) | ((pre_lv & 0x3) << 3) |
		       ((is_swing_max & 0x1) << 2) | (swing_lv & 0x3);
	fp_tx_buf[2] = fp_tx_buf[1];
	fp_tx_buf[3] = fp_tx_buf[1];
	fp_tx_buf[4] = fp_tx_buf[1];
	fp_tx_buf[5] = 0x00;  /* 107*/
	fp_tx_buf[6] = 0x01;  /* 108*/
	fp_tx_buf[7] = 0x00;  /* 109*/
	fp_tx_buf[8] = 0x00;  /* 10a*/
	fp_tx_buf[9] = 0x00;  /* 10b*/
	fp_tx_buf[10] = 0x00; /* 10c*/
	fp_tx_buf[11] = 0x00; /* 10d*/
	fp_tx_buf[12] = 0x00; /* 10e*/
	aux_wr(sel, 0x0102, 13, fp_tx_buf);

	return 0;
}

s32 dp_lane_par(u32 lane_cnt, u64 ls_clk, s32 *lane_cfg)
{
	int ret = -1;

	glb_lane_cnt = lane_cnt;
	glb_bit_rate = ls_clk;

	if (!lane_cfg) {
		edp_wrn("null hdl\n");
		goto OUT;
	}

	switch (glb_lane_cnt) {
	case 1:
		lane_sel = 0;
		break;
	case 2:
		lane_sel = 1;
		break;
	case 4:
		lane_sel = 2;
		break;
	default:
		lane_sel = 2;
	}

	switch (glb_bit_rate) {
	case BR_1P62G:
		speed_sel = 0;
		break;
	case BR_2P7G:
		speed_sel = 1;
		break;
	case BR_5P4G:
		speed_sel = 2;
		break;
	default:
		speed_sel = 1;
	}

	lane_cfg[0] = lane_sel;
	lane_cfg[1] = speed_sel;
	ret = 0;
OUT:
	return ret;
}

int cdiv(int a, int b)
{
	if (a == b)
		return a;
	if (a > b)
		return cdiv(a - b, b);
	else
		return cdiv(b - a, a);
}

/*End of File*/
