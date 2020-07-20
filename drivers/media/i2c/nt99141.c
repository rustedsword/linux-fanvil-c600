/*
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-image-sizes.h>

#define NT99141_MCLK_MIN 9000000
#define NT99141_MCLK_MAX 27000000
#define NT99141_WIDTH_MIN 64
#define NT99141_WIDTH_MAX 1280
#define NT99141_HEIGHT_MIN 64
#define NT99141_HEIGHT_MAX 720

#define NT99141_CHIP_VERSION_H 0x3000
#define NT99141_CHIP_VERSION_L 0x3001

#define NT99141_X_ADDR_START_H 0x3002
#define NT99141_X_ADDR_START_L 0x3003
#define NT99141_Y_ADDR_START_H 0x3004
#define NT99141_Y_ADDR_START_L 0x3005

#define NT99141_X_ADDR_END_H 0x3006
#define NT99141_X_ADDR_END_L 0x3007
#define NT99141_Y_ADDR_END_H 0x3008
#define NT99141_Y_ADDR_END_L 0x3009

#define NT99141_LINE_LEN_H 0x300A
#define NT99141_LINE_LEN_L 0x300B
#define NT99141_FRAME_LEN_H 0x300C
#define NT99141_FRAME_LEN_L 0x300D

#define NT99141_X_OUTPUT_SIZE_H 0x300E
#define NT99141_X_OUTPUT_SIZE_L 0x300F

#define NT99141_Y_OUTPUT_SIZE_H 0x3010
#define NT99141_Y_OUTPUT_SIZE_L 0x3011

#define NT99141_INTEGRATION_TIME_H 0x3012
#define NT99141_INTEGRATION_TIME_L 0x3013

#define NT99141_RESET_REGISTER				0x3021
#define		RESET_REGISTER_OUTPUT_BIT_SWAP		BIT(7)
#define		RESET_REGISTER_CLOCK_OUTPUT_TRISTATE	BIT(6)
#define		RESET_REGISTER_DATA_OUTPUT_TRISTATE	BIT(5)
#define		RESET_REGISTER_OUTPUT_MODE_SEL		BIT(1)
#define		RESET_REGISTER_RESET			BIT(0)

#define NT99141_READ_MODE0 			0x3022
#define		READ_MODE0_Y_EVEN_INC_MASK	0xE0
#define		READ_MODE0_X_EVEN_INC_MASK	0x1C
#define		READ_MODE0_MIRROR_HORIZONTAL	BIT(1)
#define		READ_MODE0_FLIP_VERTICAL	BIT(0)

#define NT99141_READ_MODE1 0x3023
#define		READ_MODE1_Y_ODD_INC_MASK	0xE0
#define		READ_MODE1_X_ODD_INC_MASK	0x1C
#define		READ_MODE1_X_BIN_EN		BIT(0)

#define NT99141_PATTERN_MODE 0x3025

//PLL
#define NT99141_PLL_CTRL1		0x3028 // [5:0] PLL_M
#define		PLL_CTRL1_PLL_M_MASK	0x3F
#define NT99141_PLL_CTRL2		0x3029 // [5:4] PLL_N , [2:0] PLL_P
#define		PLL_CTRL2_PLL_N_MASK 	0x30
#define		PLL_CTRL2_PLL_P_MASK	0x7
#define NT99141_PLL_CTRL3		0x302A // CLOCK CONTROL
#define		PLL_CTRL3_PLL_RESET	BIT(6)
#define		PLL_CTRL3_PLL_PWDN	BIT(5)
#define		PLL_CTRL3_PLL_BYPASS	BIT(4)
#define		PLL_CTRL3_PLL_PCLK_MASK	0xC
#define		PLL_CTRL3_EXT_PCLK_MASK	0x3

//Black level calibration registers
#define NT99141_CALIBRATION_CONTROL0				0x3040
#define		CALIBRATION_CONTROL0_LSC_CLAMP_EN		BIT(2)
#define		CALIBRATION_CONTROL0_CLAMP_EN			BIT(1)

#define NT99141_CALIBRATION_CONTROL1 				0x3041
#define		CALIBRATION_CONTROL1_BLC_SAMPLED_PIXEL_MASK 	0xC
#define		CALIBRATION_CONTROL1_DBLC_EN			BIT(1)
#define		CALIBRATION_CONTROL1_ABLC_MANUAL_EN		BIT(0)

#define NT99141_ABLC_THR_TOP 0x3042
#define NT99141_ABLC_THR_BOTTOM 0x3043

#define NT99141_OPTICAL_BLACK_WEIGHT0			0x3052
#define		OPTICAL_BLACK_WEIGHT0_ABLC_GAINW_MASK	0xF0
#define NT99141_OPTICAL_BLACK_WEIGHT1			0x3053
#define 	OPTICAL_BLACK_WEIGHT1_OB_MUL_MASK	0x0F

//Special register for applying settings
#define NT99141_REG_ACTIVATE_CTRL	0x3060

#define NT99141_PAD_CONFIG_PCLKT	0x306A //PCLK output driving
#define 	PAD_CONFIG_PCLKT_2MA	0
#define 	PAD_CONFIG_PCLKT_4MA	1
#define 	PAD_CONFIG_PCLKT_6MA	2
#define 	PAD_CONFIG_PCLKT_8MA	3

//control
#define NT99141_MODE_CONTROL0			0x3200 //[5:0] // 1 Luma Acc, 2 Color Acc, 3 Color Gain, 4 Gamma Corr, 5 Lens Shading Corr
#define		MODE_CONTROL0_LSC_EN		BIT(5)
#define		MODE_CONTROL0_GAMMA_EN 		BIT(4)
#define		MODE_CONTROL0_COLOR_GAIN_EN	BIT(3)
#define		MODE_CONTROL0_COLOR_ACCMU_EN	BIT(2)
#define		MODE_CONTROL0_LUMA_ACCMU_EN	BIT(1)

#define NT99141_MODE_CONTROL1			0x3201 //[6:0] // 0 Spec Effects, 1 Color Corr, 2 Edge Ench, 3 Noise reduction, 4 AWB, 5 AE, 6 Scaling
#define		MODE_CONTROL1_SCALING_DOWN_EN	BIT(6)
#define		MODE_CONTROL1_AE_EN		BIT(5)
#define		MODE_CONTROL1_AWB_EN		BIT(4)
#define		MODE_CONTROL1_NOISE_REDUCT_EN	BIT(3)
#define		MODE_CONTROL1_EDGE_ENHANCE_EN	BIT(2)
#define		MODE_CONTROL1_COLOR_CORR_EN	BIT(1)
#define		MODE_CONTROL1_SPECIAL_EFF_EN	BIT(0)

//Soc statistic | color accumulation
#define NT99141_COLOR_ACC_CTRL 0x3250
#define NT99141_CA_Y_TOP_THR 0x3257

//Gamma
#define NT99141_GAMMA_TAB0 0x3270
#define NT99141_GAMMA_TAB1 0x3271
#define NT99141_GAMMA_TAB2 0x3272
#define NT99141_GAMMA_TAB3 0x3273
#define NT99141_GAMMA_TAB4 0x3274
#define NT99141_GAMMA_TAB5 0x3275
#define NT99141_GAMMA_TAB6 0x3276
#define NT99141_GAMMA_TAB7 0x3277
#define NT99141_GAMMA_TAB8 0x3278
#define NT99141_GAMMA_TAB9 0x3279
#define NT99141_GAMMA_TAB10 0x327A
#define NT99141_GAMMA_TAB11 0x327B
#define NT99141_GAMMA_TAB12 0x327C
#define NT99141_GAMMA_TAB13 0x327D
#define NT99141_GAMMA_TAB14 0x327E

//Automatic White Balance
#define NT99141_WB_GAIN_R_H 0x3290
#define NT99141_WB_GAIN_R_L 0x3291
#define NT99141_WB_GAIN_B_H 0x3296
#define NT99141_WB_GAIN_B_L 0x3297

//exposure
#define NT99141_AE_DETCRANGE_UPPER 0x32B8
#define NT99141_AE_DETCRANGE_LOWER 0x32B9

#define NT99141_AE_CONTROL0 			0x32BB
#define		AE_CONTROL0_SPEED_MASK		0xF0
#define		AE_CONTROL0_ANTI_FLICKER_ALL_EN BIT(3)
#define		AE_CONTROL0_ANTI_FLICKER_EN 	BIT(2)
#define		AE_CONTROL0_AGC_EN		BIT(1)
#define		AE_CONTROL0_AEC_EN		BIT(0)

#define NT99141_AE_TARGET_LUM 0x32BC

#define NT99141_AE_CONVRANGE_UPPER 0x32BD //AE Convergence, WTF ???
#define NT99141_AE_CONVRANGE_LOWER 0x32BE

#define NT99141_AEC_MIN_EXPLINE 0x32BF
#define NT99141_AEC_MAX_EXPLINE 0x32C0

//Automatic gain?
#define NT99141_AGC_MIN_LIMIT 0x32C3
#define NT99141_AGC_MAX_LIMIT 0x32C4

#define NT99141_AE_CONTROL1 0x32C7
#define		AE_CONTROL1_AEC_FLICKER_BASE_H_MASK 0xC0
#define		AE_CONTROL1_AE_EV_A_H_MASK BIT(5)
#define		AE_CONTROL1_AE_EV_B_H_MASK BIT(4)
#define		AE_CONTROL1_AE_EV_C_H_MASK BIT(3)
#define		AE_CONTROL1_AE_EV_D_H_MASK BIT(2)
#define		AE_CONTROL1_AE_EV_E_H_MASK BIT(1)
#define		AE_CONTROL1_AE_EV_H_MASK BIT(0)

#define NT99141_AEC_FLICKER_BASE_L 0x32C8

#define NT99141_AE_EV_A_L 0x32C9
#define NT99141_AE_EV_B_L 0x32CA
#define NT99141_AE_EV_C_L 0x32CB
#define NT99141_AE_EV_D_L 0x32CC
#define NT99141_AE_EV_E_L 0x32CD

//scaling
#define NT99141_SCALER_OUT_SIZE_X_H 0x32E0 //min 56
#define NT99141_SCALER_OUT_SIZE_X_L 0x32E1

#define NT99141_SCALER_OUT_SIZE_Y_H 0x32E2 //min 42
#define NT99141_SCALER_OUT_SIZE_Y_L 0x32E3

#define NT99141_HSC_SCF_I 0x32E4 // [4:0] Integer part of horizontal scaling
#define NT99141_HSC_SCF 0x32E5 // Fraction
#define NT99141_VSC_SCF_I 0x32E6 // [4:0] Integer part of vertical scaling
#define NT99141_VSC_SCF 0x32E7 // Fraction

//Output Format and Special effects registers
#define NT99141_OUTPUT_FORMAT 0x32F0

//Image quality // Edge and color corretion
#define NT99141_NOISE_ENH_PARAM 0x3300
#define NT99141_EDGE_ENHANCEMENT_L 0x3301
#define NT99141_MATRIX_RR_H 0x3302
#define NT99141_MATRIX_RR_L 0x3303
#define NT99141_MATRIX_RG_H 0x3304
#define NT99141_MATRIX_RG_L 0x3305
#define NT99141_MATRIX_RB_H 0x3306
#define NT99141_MATRIX_RB_L 0x3307
#define NT99141_MATRIX_GR_H 0x3308
#define NT99141_MATRIX_GR_L 0x3309
#define NT99141_MATRIX_GG_H 0x330A
#define NT99141_MATRIX_GG_L 0x330B
#define NT99141_MATRIX_GB_H 0x330C
#define NT99141_MATRIX_GB_L 0x330D
#define NT99141_MATRIX_BR_H 0x330E
#define NT99141_MATRIX_BR_L 0x330F
#define NT99141_MATRIX_BG_H 0x3310
#define NT99141_MATRIX_BG_L 0x3311
#define NT99141_MATRIX_BB_H 0x3312
#define NT99141_MATRIX_BB_L 0x3313

static struct reg_sequence nt99141_496x300[] = {

	{NT99141_AEC_MIN_EXPLINE, 0x60},{NT99141_AEC_MAX_EXPLINE, 0x60},
	{0x32C1, 0x5F},{0x32C2, 0x5F}, // <?????
	{NT99141_AGC_MIN_LIMIT, 0x04},{NT99141_AGC_MAX_LIMIT, 0x2B},
	{0x32C5, 0x10},{0x32C6, 0x18}, // <?????

	//Exposure ?
	{NT99141_AE_CONTROL1, 0x00},
	{NT99141_AEC_FLICKER_BASE_L, 0xBB},
	{NT99141_AE_EV_A_L, 0x63},
	{NT99141_AE_EV_B_L, 0x6F},
	{NT99141_AE_EV_C_L, 0x6F},
	{NT99141_AE_EV_D_L, 0x77},
	{NT99141_AE_EV_E_L, 0x78},

	{0x32DB, 0x77}, // < ???????

	//Scaling output size
	{NT99141_SCALER_OUT_SIZE_X_H, 0x01},{NT99141_SCALER_OUT_SIZE_X_L, 0xF0}, //496
	{NT99141_SCALER_OUT_SIZE_Y_H, 0x01},{NT99141_SCALER_OUT_SIZE_Y_L, 0x2C}, //300

	// Scaling factors /
	{NT99141_HSC_SCF_I, 0x00},{NT99141_HSC_SCF, 0xf0}, //ef
	{NT99141_VSC_SCF_I, 0x01},{NT99141_VSC_SCF, 0x66}, //66

	{NT99141_MODE_CONTROL0, 0x3E}, //Everything is enabled
//	{NT99141_MODE_CONTROL1, 0x0F}, //Special Effects, Color corr, Edge Ench, Noise red enabled. Scaling down disabled ?

	// Clocks /
//	{NT99141_PLL_CTRL1, 0x07}, //7
//	{NT99141_PLL_CTRL2, 0x00}, //0
//	{NT99141_PLL_CTRL3, 0x14}, //EXT_PCLK = CLK_SRC ,  PLL_PCLK = PLL_CKOUT/2, PLL_BYPASS ON
	/*test clocks */
	{NT99141_PLL_CTRL1, 0x09}, //7
	{NT99141_PLL_CTRL2, 0x00}, //0
	{NT99141_PLL_CTRL3, 0x04}, //EXT_PCLK = CLK_SRC ,  PLL_PCLK = PLL_CKOUT/2, PLL_BYPASS ON

	//Some unknown shit
	{NT99141_READ_MODE0, 0x25}, //Flip Vertical, Even X+1 ,Even Y+1
	{NT99141_READ_MODE1, 0x24}, // ODD X - normal, ODD Y - normal

	//Resoulution
	{NT99141_X_ADDR_START_H, 0x00},{NT99141_X_ADDR_START_L, 0x04}, // X_Addr start /4
	{NT99141_Y_ADDR_START_H, 0x00},{NT99141_Y_ADDR_START_L, 0x04}, // Y addr start /4

	{NT99141_X_ADDR_END_H, 0x04},{NT99141_X_ADDR_END_L, 0x63}, //X Addr end /1123
	{NT99141_Y_ADDR_END_H, 0x02},{NT99141_Y_ADDR_END_L, 0xD3}, //Y Addr end /723

	{NT99141_LINE_LEN_H, 0x05}, {NT99141_LINE_LEN_L, 0x3C}, //Line len PIX clk   //1340
	{NT99141_FRAME_LEN_H, 0x02},{NT99141_FRAME_LEN_L, 0xEa}, //Frame Len  //746

	{NT99141_X_OUTPUT_SIZE_H, 0x03},{NT99141_X_OUTPUT_SIZE_L, 0xC0}, //Output Size X //960
   	{NT99141_Y_OUTPUT_SIZE_H, 0x02},{NT99141_Y_OUTPUT_SIZE_L, 0xD0}, //Output Size Y //720

	// More exposure controls?
	{NT99141_AE_DETCRANGE_UPPER, 0x3F},
	{NT99141_AE_DETCRANGE_LOWER, 0x31},

	{NT99141_AE_CONTROL0, 0x37}, //AEC Enabled, AGC Enabled, Anti_Flicker Enabled. AE speed is fast?
//	{NT99141_AE_CONTROL0, 0x30}, //AEC Dis, AGC Dis, Anti_Flicker Dis. AE speed is fast?
	{NT99141_AE_TARGET_LUM, 0x38},
	{NT99141_AE_CONVRANGE_UPPER, 0x3C},
	{NT99141_AE_CONVRANGE_LOWER, 0x34},

	{NT99141_MODE_CONTROL1, 0x7F}, //EVERYTHING IS ENABLED
	{NT99141_RESET_REGISTER, 0x06}, //Reset + Streaming + reserved??

	{NT99141_REG_ACTIVATE_CTRL, 0x01}, //Apply Settings
};

static struct reg_sequence nt99141_640x480[] = {

	{NT99141_AEC_MIN_EXPLINE, 0x60},{NT99141_AEC_MAX_EXPLINE, 0x60},
	{0x32C1, 0x5F},{0x32C2, 0x5F}, // <?????
	{NT99141_AGC_MIN_LIMIT, 0x04},{NT99141_AGC_MAX_LIMIT, 0x2B},
	{0x32C5, 0x10},{0x32C6, 0x18}, // <?????

	//Exposure ?
	{NT99141_AE_CONTROL1, 0x00},
	{NT99141_AEC_FLICKER_BASE_L, 0xBB},
	{NT99141_AE_EV_A_L, 0x63},
	{NT99141_AE_EV_B_L, 0x6F},
	{NT99141_AE_EV_C_L, 0x6F},
	{NT99141_AE_EV_D_L, 0x77},
	{NT99141_AE_EV_E_L, 0x78},

	{0x32DB, 0x77}, // < ???????

	//Scaling output size
	{NT99141_SCALER_OUT_SIZE_X_H, 0x02},{NT99141_SCALER_OUT_SIZE_X_L, 0x80}, //640
	{NT99141_SCALER_OUT_SIZE_Y_H, 0x01},{NT99141_SCALER_OUT_SIZE_Y_L, 0xE0}, //480

	// Scaling factors /
	{NT99141_HSC_SCF_I, 0x00},{NT99141_HSC_SCF, 0x80},
	{NT99141_VSC_SCF_I, 0x00},{NT99141_VSC_SCF, 0x80},

	{NT99141_MODE_CONTROL0, 0x3E}, //Everything is enabled
//	{NT99141_MODE_CONTROL1, 0x0F}, //Special Effects, Color corr, Edge Ench, Noise red enabled. Scaling down disabled ?

	// Clocks /
//	{NT99141_PLL_CTRL1, 0x07}, //7
//	{NT99141_PLL_CTRL2, 0x00}, //0
//	{NT99141_PLL_CTRL3, 0x14}, //EXT_PCLK = CLK_SRC ,  PLL_PCLK = PLL_CKOUT/2, PLL_BYPASS ON
	/*test clocks */
	{NT99141_PLL_CTRL1, 0x09}, //7
	{NT99141_PLL_CTRL2, 0x00}, //0
	{NT99141_PLL_CTRL3, 0x04}, //EXT_PCLK = CLK_SRC ,  PLL_PCLK = PLL_CKOUT/2, PLL_BYPASS ON

	//Some unknown shit
	{NT99141_READ_MODE0, 0x25}, //Flip Vertical, Even X+1 ,Even Y+1
	{NT99141_READ_MODE1, 0x24}, // ODD X - normal, ODD Y - normal

	//Resoulution
	{NT99141_X_ADDR_START_H, 0x00},{NT99141_X_ADDR_START_L, 0x04}, // X_Addr start /4
	{NT99141_Y_ADDR_START_H, 0x00},{NT99141_Y_ADDR_START_L, 0x04}, // Y addr start /4

	{NT99141_X_ADDR_END_H, 0x04},{NT99141_X_ADDR_END_L, 0x63}, //X Addr end /1123
	{NT99141_Y_ADDR_END_H, 0x02},{NT99141_Y_ADDR_END_L, 0xD3}, //Y Addr end /723

	{NT99141_LINE_LEN_H, 0x05}, {NT99141_LINE_LEN_L, 0x3C}, //Line len PIX clk   //1340
	{NT99141_FRAME_LEN_H, 0x02},{NT99141_FRAME_LEN_L, 0xEa}, //Frame Len  //746

	{NT99141_X_OUTPUT_SIZE_H, 0x03},{NT99141_X_OUTPUT_SIZE_L, 0xC0}, //Output Size X //960
   	{NT99141_Y_OUTPUT_SIZE_H, 0x02},{NT99141_Y_OUTPUT_SIZE_L, 0xD0}, //Output Size Y //720

	// More exposure controls?
	{NT99141_AE_DETCRANGE_UPPER, 0x3F},
	{NT99141_AE_DETCRANGE_LOWER, 0x31},

	{NT99141_AE_CONTROL0, 0x37}, //AEC Enabled, AGC Enabled, Anti_Flicker Enabled. AE speed is fast?
//	{NT99141_AE_CONTROL0, 0x30}, //AEC Dis, AGC Dis, Anti_Flicker Dis. AE speed is fast?
	{NT99141_AE_TARGET_LUM, 0x38},
	{NT99141_AE_CONVRANGE_UPPER, 0x3C},
	{NT99141_AE_CONVRANGE_LOWER, 0x34},

	{NT99141_MODE_CONTROL1, 0x7F}, //EVERYTHING IS ENABLED
	{NT99141_RESET_REGISTER, 0x06}, //Reset + Streaming + reserved??

	{NT99141_REG_ACTIVATE_CTRL, 0x01}, //Apply Settings
};

static struct reg_sequence nt99141_640x480_orig[] = {

	{NT99141_AEC_MIN_EXPLINE, 0x60},{NT99141_AEC_MAX_EXPLINE, 0x60},
	{0x32C1, 0x60},{0x32C2, 0x60}, // <?????
	{NT99141_AGC_MIN_LIMIT, 0x00},{NT99141_AGC_MAX_LIMIT, 0x20},
	{0x32C5, 0x20},{0x32C6, 0x20}, // <?????

	//Exposure ?
	{NT99141_AE_CONTROL1, 0x00},
	{NT99141_AEC_FLICKER_BASE_L, 0xB3},
	{NT99141_AE_EV_A_L, 0x60},
	{NT99141_AE_EV_B_L, 0x80},
	{NT99141_AE_EV_C_L, 0x80},
	{NT99141_AE_EV_D_L, 0x80},
	{NT99141_AE_EV_E_L, 0x80},

	{0x32DB, 0x76}, // < ???????

	/* Scaling output size */
	{NT99141_SCALER_OUT_SIZE_X_H, 0x02},{NT99141_SCALER_OUT_SIZE_X_L, 0x80}, //640
	{NT99141_SCALER_OUT_SIZE_Y_H, 0x01},{NT99141_SCALER_OUT_SIZE_Y_L, 0xE0}, //480

	/* Scaling factors */
	{NT99141_HSC_SCF_I, 0x00},{NT99141_HSC_SCF, 0x80},
	{NT99141_VSC_SCF_I, 0x00},{NT99141_VSC_SCF, 0x80},

	{NT99141_MODE_CONTROL0, 0x3E}, //Everything is enabled
	{NT99141_MODE_CONTROL1, 0x0F}, //Special Effects, Color corr, Edge Ench, Noise red enabled. Scaling down disabled ?

	/* Clocks */
	{NT99141_PLL_CTRL1, 0x07}, //7
	{NT99141_PLL_CTRL2, 0x00}, //0
	{NT99141_PLL_CTRL3, 0x14}, //EXT_PCLK = CLK_SRC ,  PLL_PCLK = PLL_CKOUT/2, PLL_BYPASS ON
	/*test clocks */

	//Some unknown shit
	{NT99141_READ_MODE0, 0x25}, //Flip Vertical, Even X+1 ,Even Y+1
	{NT99141_READ_MODE1, 0x24}, // ODD X - normal, ODD Y - normal

	//Resoulution
	{NT99141_X_ADDR_START_H, 0x00},{NT99141_X_ADDR_START_L, 0xA4}, // X_Addr start /164
	{NT99141_Y_ADDR_START_H, 0x00},{NT99141_Y_ADDR_START_L, 0x04}, // Y addr start /4

	{NT99141_X_ADDR_END_H, 0x04},{NT99141_X_ADDR_END_L, 0x63}, //X Addr end /1123
	{NT99141_Y_ADDR_END_H, 0x02},{NT99141_Y_ADDR_END_L, 0xD3}, //Y Addr end /723

	{NT99141_LINE_LEN_H, 0x05}, {NT99141_LINE_LEN_L, 0x3C}, //Line len PIX clk   //1340
	{NT99141_FRAME_LEN_H, 0x02},{NT99141_FRAME_LEN_L, 0xE1}, //Frame Len  //737

	{NT99141_X_OUTPUT_SIZE_H, 0x03},{NT99141_X_OUTPUT_SIZE_L, 0xC0}, //Output Size X //960
   	{NT99141_Y_OUTPUT_SIZE_H, 0x02},{NT99141_Y_OUTPUT_SIZE_L, 0xD0}, //Output Size Y //720

	// More exposure controls?
	{NT99141_AE_DETCRANGE_UPPER, 0x3F},
	{NT99141_AE_DETCRANGE_LOWER, 0x31},

	{NT99141_AE_CONTROL0, 0x87}, //AEC Enabled, AGC Enabled, Anti_Flicker Enabled. AE speed is fast?
	{NT99141_AE_TARGET_LUM, 0x38},
	{NT99141_AE_CONVRANGE_UPPER, 0x3C},
	{NT99141_AE_CONVRANGE_LOWER, 0x34},

	{NT99141_MODE_CONTROL1, 0x7F}, //EVERYTHING IS ENABLED
	{NT99141_RESET_REGISTER, 0x06}, //Reset + Streaming + reserved??

	{NT99141_REG_ACTIVATE_CTRL, 0x01}, //Apply Settings
};

static struct reg_sequence nt99141_1280x720[] = {
	{NT99141_AEC_MIN_EXPLINE, 0x60},{NT99141_AEC_MAX_EXPLINE, 0x60},
	{0x32C1, 0x60},{0x32C2, 0x60}, // << ??
	{NT99141_AGC_MIN_LIMIT, 0x00},{NT99141_AGC_MAX_LIMIT, 0x27},
	{0x32C5, 0x10},{0x32C6, 0x18}, // << ??

	{NT99141_AE_CONTROL1, 0x00},
	{NT99141_AEC_FLICKER_BASE_L, 0xB8},

	{NT99141_AE_EV_A_L, 0x60},
	{NT99141_AE_EV_B_L, 0x70},
	{NT99141_AE_EV_C_L, 0x70},
	{NT99141_AE_EV_D_L, 0x78},
	{NT99141_AE_EV_E_L, 0x78},

	{0x32DB, 0x7A}, // <<<< ????????


//------------- E X P O S U R E ------------------- !
/*
	{NT99141_AEC_MIN_EXPLINE, 0xEf},{NT99141_AEC_MAX_EXPLINE, 0xF0},
//	{0x32C1, 0xF0},{0x32C2, 0xF0}, // << ??
	{NT99141_AGC_MIN_LIMIT, 0x00},{NT99141_AGC_MAX_LIMIT, 0x27},
//	{0x32C5, 0xF0},{0x32C6, 0xF0}, // << ??

	{NT99141_AE_CONTROL1, 0xff},
	{NT99141_AEC_FLICKER_BASE_L, 0x00},

	{NT99141_AE_EV_A_L, 0x0},
	{NT99141_AE_EV_B_L, 0x0},
	{NT99141_AE_EV_C_L, 0x0},
	{NT99141_AE_EV_D_L, 0x0},
	{NT99141_AE_EV_E_L, 0x0},

//	{0x32DB, 0x7A}, // <<<< ????????

	{NT99141_AE_DETCRANGE_UPPER, 0x3F},{NT99141_AE_DETCRANGE_LOWER, 0x31},

//	{NT99141_AE_CONTROL0, 0x87},
//	{NT99141_AE_CONTROL0, 0x37}, // +AEC +AGC +ANTIFLi +
	{NT99141_AE_CONTROL0, 0x30}, // -AEC -AGC -ANTIFLi +
	{NT99141_AE_TARGET_LUM, 0x38},
	{NT99141_AE_CONVRANGE_UPPER, 0x3C}, {NT99141_AE_CONVRANGE_LOWER, 0x34},
*/
/// ----------------------

	//Clocks !
//	{NT99141_PLL_CTRL1, 0x24}, //PPL_M  36  --<clock multiplier
//	{NT99141_PLL_CTRL1, 0xB},
//	{NT99141_PLL_CTRL1, 0x22},

//	{NT99141_PLL_CTRL2, 0x20}, //PLL_N  - 2 | PLL_P - 0 | PLL_N - is clock_divider | PLL_P is another clock divider
//	{NT99141_PLL_CTRL2, 0x0},
//	{NT99141_PLL_CTRL2, 0x10},

//	{NT99141_PLL_CTRL3, 0x04}, //PCLK = PLL_CLKOUT / 2 | This register Divides clock By 2,4 or 8
//	{NT99141_PLL_CTRL3, 0x04},
//	{NT99141_PLL_CTRL3, 0x04},

//	{NT99141_READ_MODE0, 0x25},
//	{NT99141_READ_MODE1, 0x24},

//	{NT99141_MODE_CONTROL0, 0x3E}, //
//	{NT99141_MODE_CONTROL0, MODE_CONTROL0_LSC_EN | MODE_CONTROL0_GAMMA_EN |
//				MODE_CONTROL0_COLOR_GAIN_EN | MODE_CONTROL0_COLOR_ACCMU_EN |
//				MODE_CONTROL0_LUMA_ACCMU_EN},

//	{NT99141_MODE_CONTROL1, 0x7F},
//	{NT99141_MODE_CONTROL1, MODE_CONTROL1_SCALING_DOWN_EN |
//				MODE_CONTROL1_AE_EN |
//				MODE_CONTROL1_AWB_EN | MODE_CONTROL1_NOISE_REDUCT_EN |
//				MODE_CONTROL1_EDGE_ENHANCE_EN | MODE_CONTROL1_COLOR_CORR_EN |
//				MODE_CONTROL1_SPECIAL_EFF_EN},
//	{NT99141_MODE_CONTROL1, 0x0},

//	{NT99141_RESET_REGISTER, 0x06}, //Streaming + reserved??
	{NT99141_RESET_REGISTER, 0x02}, //Streaming
	{NT99141_REG_ACTIVATE_CTRL, 0x01},
};

static struct reg_sequence test_regs[] =
{
    //{0x3021, 0x60},
	{0x3109, 0x04}, //<<< ??????

	{NT99141_CALIBRATION_CONTROL0, 0x04}, //LSC_CLAMP_EN, CLAMP_EN ??
	{NT99141_CALIBRATION_CONTROL1, 0x02}, //Digital Black Level calibration en, 0bit = 0 Auto calibration
	//Black level calibration settings
	{NT99141_ABLC_THR_TOP, 0xFF},{NT99141_ABLC_THR_BOTTOM, 0x08},

	{NT99141_OPTICAL_BLACK_WEIGHT0, 0xE0}, //?

	{0x305F, 0x33}, //<< RESERVED ?!?!


	// Unknown
	{0x3100, 0x07},{0x3106, 0x03},
	{0x3108, 0x00},{0x3110, 0x22},
	{0x3111, 0x57},{0x3112, 0x22},
	{0x3113, 0x55},{0x3114, 0x05},
	{0x3135, 0x00},


//	{NT99141_PAD_CONFIG_PCLKT,0x01}, //4ma PCLK output driving !


	// Initial AWB Gain
	{NT99141_WB_GAIN_R_H, 0x01},{NT99141_WB_GAIN_R_L, 0x80},
	{NT99141_WB_GAIN_B_H, 0x01},{NT99141_WB_GAIN_B_L, 0x73},


	//CA Ratio
	{NT99141_COLOR_ACC_CTRL, 0x80},  //Enable color acc zone
	{0x3251, 0x03},{0x3252, 0xFF}, //
	{0x3253, 0x00},{0x3254, 0x03}, // This register are reserved, but maybe they are doing something?
	{0x3255, 0xFF},{0x3256, 0x00}, //
	{NT99141_CA_Y_TOP_THR, 0x50},


	// Gamma
	{NT99141_GAMMA_TAB0, 0x00},{NT99141_GAMMA_TAB1, 0x0C},{NT99141_GAMMA_TAB2, 0x18},{NT99141_GAMMA_TAB3, 0x32},{NT99141_GAMMA_TAB4, 0x44},
	{NT99141_GAMMA_TAB5, 0x54},{NT99141_GAMMA_TAB6, 0x70},{NT99141_GAMMA_TAB7, 0x88},{NT99141_GAMMA_TAB8, 0x9D},{NT99141_GAMMA_TAB9, 0xB0},
	{NT99141_GAMMA_TAB10, 0xCF},{NT99141_GAMMA_TAB11, 0xE2},{NT99141_GAMMA_TAB12, 0xEF},{NT99141_GAMMA_TAB13, 0xF7},{NT99141_GAMMA_TAB14, 0xFF},

	// Color Correction
	{NT99141_MATRIX_RR_H, 0x00},{NT99141_MATRIX_RR_L, 0x40},
	{NT99141_MATRIX_RG_H, 0x00},{NT99141_MATRIX_RG_L, 0x96},
	{NT99141_MATRIX_RB_H, 0x00},{NT99141_MATRIX_RB_L, 0x29},
	{NT99141_MATRIX_GR_H, 0x07},{NT99141_MATRIX_GR_L, 0xBA},
	{NT99141_MATRIX_GG_H, 0x06},{NT99141_MATRIX_GG_L, 0xF5},
	{NT99141_MATRIX_GB_H, 0x01},{NT99141_MATRIX_GB_L, 0x51},
	{NT99141_MATRIX_BR_H, 0x01},{NT99141_MATRIX_BR_L, 0x30},
	{NT99141_MATRIX_BG_H, 0x07},{NT99141_MATRIX_BG_L, 0x16},
	{NT99141_MATRIX_BB_H, 0x07},{NT99141_MATRIX_BB_L, 0xBA},


	// EExt  UNKNOWN REGS !
	{0x3326, 0x02},{0x32F6, 0x0F},{0x32F9, 0x42},{0x32FA, 0x24},{0x3325, 0x4A},
	{0x3330, 0x00},{0x3331, 0x0A},{0x3332, 0xFF},{0x3338, 0x30},{0x3339, 0x84},
	{0x333A, 0x48},{0x333F, 0x07},

	// Auto Function || UNKNOWN REGS
	{0x3360, 0x10},{0x3361, 0x18},{0x3362, 0x1f},{0x3363, 0x37},{0x3364, 0x80},
	{0x3365, 0x80},{0x3366, 0x68},{0x3367, 0x60},{0x3368, 0x30},{0x3369, 0x28},
	{0x336A, 0x20},{0x336B, 0x10},{0x336C, 0x00},{0x336D, 0x20},{0x336E, 0x1C},
	{0x336F, 0x18},{0x3370, 0x10},{0x3371, 0x38},{0x3372, 0x3C},{0x3373, 0x3F},
	{0x3374, 0x3F},{0x338A, 0x34},{0x338B, 0x7F},{0x338C, 0x10},{0x338D, 0x23},
	{0x338E, 0x7F},{0x338F, 0x14},{0x3375, 0x0A},{0x3376, 0x0C},{0x3377, 0x10},
	{0x3378, 0x14},

	// Integration Time ?!?
	{NT99141_INTEGRATION_TIME_H, 0x02}, {NT99141_INTEGRATION_TIME_L, 0xD0}, //720

	//Aply
	{NT99141_REG_ACTIVATE_CTRL, 0x01},

};

struct nt99141_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler        hdl;
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *mclk;
	u32 mclk_freq;

	struct regmap *regmap;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct gpio_desc *gp_gpio;

	int width;
	int height;
	int line_length;
	int frame_length;
	u32     cfmt_code;
};

static struct nt99141_dev *to_nt99141(const struct i2c_client *client)
{
        return container_of(i2c_get_clientdata(client), struct nt99141_dev,
                            sd);
}

#define PRECISION 10000
int calc_fraction(int original_size, int target_size) {
        int fraction;
        int tmp;

        tmp = original_size * PRECISION / target_size;
        tmp = tmp - PRECISION;
        fraction = (0xff + 1) * tmp / PRECISION;

        return fraction;
}

static void nt99141_power(struct nt99141_dev *sensor, int on) {
	int ret;
	int val[2];

	if(gpiod_get_value(sensor->pwdn_gpio) == on) {
		dev_err(&sensor->i2c_client->dev, "Setting power to %s!\n", on ? "enabled" : "disabled");
		gpiod_direction_output(sensor->pwdn_gpio, !on);
		usleep_range(10000, 15000);
		if(on) {
			ret = regmap_bulk_read(sensor->regmap, NT99141_CHIP_VERSION_H, val, 2);
			if(!ret) {
				dev_err(&sensor->i2c_client->dev, "Test read: 0x%x 0x%x!\n", val[0], val[1]);
			} else {
				dev_err(&sensor->i2c_client->dev, "Test read failed: %d\n", ret);
			}
		}
	} else {
		dev_err(&sensor->i2c_client->dev, "Not setting power to %s!\n", on ? "enabled" : "disabled");
	}
}

static void nt99141_reset(struct nt99141_dev *sensor, u32 val) {
	nt99141_power(sensor, 1);

	gpiod_direction_output(sensor->reset_gpio, 1);
	msleep(20);
	gpiod_set_value(sensor->reset_gpio, 0);
}

static int nt99141_regmap_write_u16(struct regmap *regmap, int reg, u16 value) {
	value = cpu_to_be16(value);
	return regmap_bulk_write(regmap, reg, &value, 2);
}

static int nt99141_set_pll(struct nt99141_dev *sensor) {
	int ret;

	//set clock for 70MHZ for now
//	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL1, 0x22);
//	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL2, 0x20);
//	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL3, 0x04);

	//set clock for 62MHZ for now
	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL1, 0x1E);
	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL2, 0x20);
	ret = regmap_write(sensor->regmap, NT99141_PLL_CTRL3, 0x04);

	return ret;
}

static int nt99141_set_resolution(struct nt99141_dev *sensor) {
	int ret;
	int fraction;

	dev_err(&sensor->i2c_client->dev, "Asking camera to set resolution w:%d h:%d\n", sensor->width, sensor->height);

	//Clocks
	ret = nt99141_set_pll(sensor);

	//Scaling
	if(sensor->width == NT99141_WIDTH_MAX && sensor->height == NT99141_HEIGHT_MAX) {
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_SCALER_OUT_SIZE_X_H, 0);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_SCALER_OUT_SIZE_Y_H, 0);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_HSC_SCF_I, 0);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_VSC_SCF_I, 0);
	} else {
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_SCALER_OUT_SIZE_X_H, sensor->width);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_SCALER_OUT_SIZE_Y_H, sensor->height);

		fraction = calc_fraction(NT99141_WIDTH_MAX, sensor->width);
		dev_err(&sensor->i2c_client->dev, "Calculated fraction:0x%x for width:%d\n", fraction, sensor->width);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_HSC_SCF_I, fraction);

		fraction = calc_fraction(NT99141_HEIGHT_MAX, sensor->height);
		dev_err(&sensor->i2c_client->dev, "Calculated fraction:0x%x for height:%d\n", fraction, sensor->height);
		ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_VSC_SCF_I, fraction);

		ret = regmap_update_bits(sensor->regmap, NT99141_MODE_CONTROL1, MODE_CONTROL1_SCALING_DOWN_EN, 0xff);
	}

	//Sensor array resolution
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_X_ADDR_START_H, 0x4);
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_Y_ADDR_START_H, 0x4);
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_X_ADDR_END_H, NT99141_WIDTH_MAX + 3);
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_Y_ADDR_END_H, NT99141_HEIGHT_MAX + 3);

	//Horizontal and vertical image size with blanks ?
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_LINE_LEN_H, 1660);
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_FRAME_LEN_H, 746);

	//Output size
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_X_OUTPUT_SIZE_H, NT99141_WIDTH_MAX);
	ret = nt99141_regmap_write_u16(sensor->regmap, NT99141_Y_OUTPUT_SIZE_H, NT99141_HEIGHT_MAX);

	//? Odd/even bytes increase ?
	ret = regmap_write(sensor->regmap, NT99141_READ_MODE0, 0x24);
	ret = regmap_write(sensor->regmap, NT99141_READ_MODE1, 0x24);

	ret = regmap_multi_reg_write(sensor->regmap, nt99141_1280x720, ARRAY_SIZE(nt99141_1280x720));
	return ret;
}

static int nt99141_set_params(struct nt99141_dev *sensor) {
        int ret;


        switch(sensor->cfmt_code) {
        case MEDIA_BUS_FMT_RGB565_2X8_LE:
		dev_err(&sensor->i2c_client->dev, "Setting to RGB565\n");
                ret = regmap_write(sensor->regmap, NT99141_OUTPUT_FORMAT, 0x11); // SHIT
		break;
        case MEDIA_BUS_FMT_YUYV8_2X8:
		dev_err(&sensor->i2c_client->dev, "Setting 1 to YUYV\n");
                ret = regmap_write(sensor->regmap, NT99141_OUTPUT_FORMAT, 1);
		break;
        case MEDIA_BUS_FMT_UYVY8_2X8:
		dev_err(&sensor->i2c_client->dev, "Setting 0 to UYVY\n");
                ret = regmap_write(sensor->regmap, NT99141_OUTPUT_FORMAT, 0);
		break;
        case MEDIA_BUS_FMT_YVYU8_2X8:
		dev_err(&sensor->i2c_client->dev, "Setting 3 to YVYU\n");
                ret = regmap_write(sensor->regmap, NT99141_OUTPUT_FORMAT, 3);
		break;
        case MEDIA_BUS_FMT_VYUY8_2X8:
		dev_err(&sensor->i2c_client->dev, "Setting 2 to VYUY\n");
                ret = regmap_write(sensor->regmap, NT99141_OUTPUT_FORMAT, 2);
		break;
        default:
                ret = -EINVAL;
        }

//	if(!ret)
//		ret = regmap_write(sensor->regmap, NT99141_REG_ACTIVATE_CTRL, 1);

        return ret;
}

static int nt99141_s_power(struct v4l2_subdev *sd, int on) {
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct nt99141_dev *sensor = to_nt99141(client);

	dev_err(&client->dev, "Request to %s power\n", on ? "enable" : "disable");
	nt99141_power(sensor, on);

	return 0;
}

static const char * const test_pattern_menu[] = {
	"Disabled",
	"Color bars",
	"PN10",
};

static u32 nt99141_codes[] = {
        MEDIA_BUS_FMT_YUYV8_2X8,
        MEDIA_BUS_FMT_UYVY8_2X8,
        MEDIA_BUS_FMT_YVYU8_2X8,
        MEDIA_BUS_FMT_VYUY8_2X8,
        MEDIA_BUS_FMT_RGB565_2X8_LE,
};

static int nt99141_get_fmt(struct v4l2_subdev *sd,
                struct v4l2_subdev_pad_config *cfg,
                struct v4l2_subdev_format *format)
{
        struct v4l2_mbus_framefmt *mf = &format->format;
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
        struct nt99141_dev *sensor = to_nt99141(client);

	dev_err(&client->dev, "Get FMT\n");

        if (format->pad)
                return -EINVAL;

	if(!sensor->width)
		sensor->width = NT99141_WIDTH_MAX;
	if(!sensor->height)
		sensor->height = NT99141_HEIGHT_MAX;

	mf->width	= sensor->width;
	mf->height	= sensor->height;
	mf->code        = sensor->cfmt_code;
//        mf->colorspace  = V4L2_COLORSPACE_REC709;
//        mf->colorspace  = V4L2_COLORSPACE_SRGB;
	mf->colorspace  = V4L2_COLORSPACE_SMPTE170M;
	mf->field	= V4L2_FIELD_NONE;
	mf->ycbcr_enc	= V4L2_MAP_YCBCR_ENC_DEFAULT(mf->colorspace);
	mf->xfer_func	= V4L2_MAP_XFER_FUNC_DEFAULT(mf->colorspace);

        return 0;
}

static int nt99141_set_fmt(struct v4l2_subdev *sd,
                struct v4l2_subdev_pad_config *cfg,
                struct v4l2_subdev_format *format)
{
        struct v4l2_mbus_framefmt *mf = &format->format;
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	struct nt99141_dev *sensor = to_nt99141(client);

	dev_err(&client->dev, "Request to set format to %d, width:%d, height:%d\n", mf->code, mf->width, mf->height);

        if (format->pad)
                return -EINVAL;

	if(mf->width > NT99141_WIDTH_MAX)
		mf->width = NT99141_WIDTH_MAX;
	if(mf->height > NT99141_WIDTH_MAX)
		mf->height = NT99141_HEIGHT_MAX;

        mf->field       = V4L2_FIELD_NONE;
//        mf->colorspace  = V4L2_COLORSPACE_REC709;
//        mf->colorspace  = V4L2_COLORSPACE_SRGB;
        mf->colorspace  = V4L2_COLORSPACE_SMPTE170M;
	mf->ycbcr_enc	= V4L2_MAP_YCBCR_ENC_DEFAULT(mf->colorspace);
	mf->xfer_func	= V4L2_MAP_XFER_FUNC_DEFAULT(mf->colorspace);

        switch (mf->code) {
        case MEDIA_BUS_FMT_RGB565_2X8_LE:
        case MEDIA_BUS_FMT_YUYV8_2X8:
        case MEDIA_BUS_FMT_UYVY8_2X8:
        case MEDIA_BUS_FMT_YVYU8_2X8:
        case MEDIA_BUS_FMT_VYUY8_2X8:
                break;
        default:
		mf->code = MEDIA_BUS_FMT_UYVY8_2X8;
//		mf->code = MEDIA_BUS_FMT_RGB565_2X8_LE;
                break;
        }

        if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		dev_err(&client->dev, "V4l2 SUBDEV FORMAT ACTIVE!\n");
		sensor->cfmt_code = mf->code;
		sensor->width = mf->width;
		sensor->height = mf->height;
		return 0;
	} else if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		dev_err(&client->dev, "V4l2 SUBDEV FORMAT TRY!\n");
	} else {
		dev_err(&client->dev, "V4l2 SUBDEV FORMAT WTF?!?!\n");
	}

	sensor->cfmt_code = mf->code;
	sensor->width = mf->width;
	sensor->height = mf->height;

        cfg->try_fmt = *mf;
        return 0;
}

static int nt99141_enum_mbus_code(struct v4l2_subdev *sd,
                struct v4l2_subdev_pad_config *cfg,
                struct v4l2_subdev_mbus_code_enum *code)
{
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	dev_err(&client->dev, "enum mbus\n");

        if (code->pad || code->index >= ARRAY_SIZE(nt99141_codes))
                return -EINVAL;

        code->code = nt99141_codes[code->index];
        return 0;
}

static int nt99141_get_selection(struct v4l2_subdev *sd,
                struct v4l2_subdev_pad_config *cfg,
                struct v4l2_subdev_selection *sel)
{
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	dev_err(&client->dev, "NT99141 Get Selection\n");

        if (sel->which != V4L2_SUBDEV_FORMAT_ACTIVE)
                return -EINVAL;

        switch (sel->target) {
        case V4L2_SEL_TGT_CROP_BOUNDS:
        case V4L2_SEL_TGT_CROP_DEFAULT:
        case V4L2_SEL_TGT_CROP:
                sel->r.left = 0;
                sel->r.top = 0;
                sel->r.width = UXGA_WIDTH;
                sel->r.height = UXGA_HEIGHT;
                return 0;
        default:
                return -EINVAL;
        }
}

static int nt99141_s_ctrl(struct v4l2_ctrl *ctrl) {
        struct v4l2_subdev *sd =
                &container_of(ctrl->handler, struct nt99141_dev, hdl)->sd;
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	struct nt99141_dev *sensor = to_nt99141(client);
	int val;

	dev_err(&client->dev, "Request to set control id:%d val:%d\n", ctrl->id, ctrl->val);

        switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		return regmap_update_bits(sensor->regmap, NT99141_READ_MODE0, 1, ctrl->val);
	case V4L2_CID_HFLIP:
		return regmap_update_bits(sensor->regmap, NT99141_READ_MODE0, 2, ctrl->val << 1);
	case V4L2_CID_TEST_PATTERN:
		val = ctrl->val ? ctrl->val + 1 : 0;
		return regmap_update_bits(sensor->regmap, NT99141_PATTERN_MODE, 3, val);
        }

	return -EINVAL;
}

static int nt99141_s_stream(struct v4l2_subdev *sd, int enable) {
	int ret = 0;
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	struct nt99141_dev *sensor = to_nt99141(client);

	dev_err(&client->dev, "Request to %s streaming\n", enable ? "start" : "stop");

	if(enable) {
		ret = nt99141_set_params(sensor);
		if(ret)
			return ret;

		ret = nt99141_set_resolution(sensor);
		if(ret)
			return ret;


		ret = regmap_multi_reg_write(sensor->regmap, test_regs, ARRAY_SIZE(test_regs));

                //ret = regmap_update_bits(sensor->regmap, NT99141_RESET_REGISTER, 0x80, 0x80); // SHIT
		ret = regmap_write(sensor->regmap, NT99141_PAD_CONFIG_PCLKT, PAD_CONFIG_PCLKT_4MA);

		ret = regmap_write(sensor->regmap, NT99141_MODE_CONTROL0, MODE_CONTROL0_LSC_EN | MODE_CONTROL0_GAMMA_EN |
				MODE_CONTROL0_COLOR_GAIN_EN | MODE_CONTROL0_COLOR_ACCMU_EN |
				MODE_CONTROL0_LUMA_ACCMU_EN);

		ret = regmap_update_bits(sensor->regmap, NT99141_MODE_CONTROL1,
				MODE_CONTROL1_AE_EN |
				MODE_CONTROL1_AWB_EN | MODE_CONTROL1_NOISE_REDUCT_EN |
				MODE_CONTROL1_EDGE_ENHANCE_EN | MODE_CONTROL1_COLOR_CORR_EN |
				MODE_CONTROL1_SPECIAL_EFF_EN, 0xff);

/*
		ret = regmap_write(sensor->regmap, NT99141_MODE_CONTROL1,
				MODE_CONTROL1_SCALING_DOWN_EN |
				MODE_CONTROL1_AE_EN |
				MODE_CONTROL1_AWB_EN | MODE_CONTROL1_NOISE_REDUCT_EN |
				MODE_CONTROL1_EDGE_ENHANCE_EN | MODE_CONTROL1_COLOR_CORR_EN |
				MODE_CONTROL1_SPECIAL_EFF_EN);
*/

		ret = regmap_write(sensor->regmap, NT99141_RESET_REGISTER, 0x02); //start streaming
		ret = regmap_write(sensor->regmap, NT99141_REG_ACTIVATE_CTRL, 1);

		/*restore controls */
		/* MOVE THIS TO POWER ON */
		ret = v4l2_ctrl_handler_setup(&sensor->hdl);
		if(ret)
			return ret;
	}

	return ret;
}

static const struct v4l2_ctrl_ops nt99141_ctrl_ops = {
        .s_ctrl = nt99141_s_ctrl,
};

static const struct v4l2_subdev_video_ops nt99141_video_ops = {
	.s_stream = nt99141_s_stream,
};

static const struct v4l2_subdev_core_ops nt99141_core_ops = {
        .s_power        = nt99141_s_power,
};

static const struct v4l2_subdev_pad_ops nt99141_pad_ops = {
	.enum_mbus_code = nt99141_enum_mbus_code,
        .get_selection  = nt99141_get_selection,
	.get_fmt        = nt99141_get_fmt,
	.set_fmt        = nt99141_set_fmt,
};

static const struct v4l2_subdev_ops nt99141_subdev_ops = {
        .core = &nt99141_core_ops,
        .video = &nt99141_video_ops,
        .pad = &nt99141_pad_ops,
};

static int nt99141_probe_dt(struct i2c_client *client,
                struct nt99141_dev *sensor) {
	struct device *dev = &client->dev;

	sensor->pwdn_gpio = devm_gpiod_get(dev, "powerdown",
						GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio)) {
		dev_err(dev, "failed to get powerdown gpio\n");
		return PTR_ERR(sensor->pwdn_gpio);
	}

	sensor->reset_gpio = devm_gpiod_get(dev, "reset",
						GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset_gpio)) {
		dev_err(dev, "failed to get reset gpio\n");
		return PTR_ERR(sensor->reset_gpio);
	}

	sensor->gp_gpio = devm_gpiod_get(dev, "gp",
						GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->gp_gpio)) {
		dev_err(dev, "failed to get gp gpio\n");
		return PTR_ERR(sensor->gp_gpio);
	}
	return 0;
}

static int nt99141_init(struct i2c_client *client, struct nt99141_dev *sensor) {

	int ret;
	u8 val[2];

	nt99141_reset(sensor, 0);

	ret = regmap_bulk_read(sensor->regmap, NT99141_CHIP_VERSION_H, val, 2);
	if(ret)
		return ret;

	if(val[0] != 0x14 || val[1] != 0x10)
		return -ENODEV;

	dev_info(&client->dev, "Novatek nt99141 Sensor\n");

	return ret;
}

static const struct regmap_config nt99141_regmap = {
	.reg_bits = 16,
	.val_bits = 8,

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/////////////////////////////////////////////

#define AEC_MIN_EXPLINE 1
#define AEC_MAX_EXPLINE 2
#define AGC_MIN_LIMIT	3
#define AGC_MAX_LIMIT	4
#define AE_CONTROL1	5
#define AEC_FLICKER_BASE_L 6
#define AE_EV_A_L 7
#define AE_EV_B_L 8
#define AE_EV_C_L 9
#define AE_EV_D_L 10
#define AE_EV_E_L 11

#define AE_DETCRANGE_UPPER 12
#define AE_DETCRANGE_LOWER 13
#define AE_CONTROL0 14
#define AE_TARGET_LUM 15
#define AE_CONVRANGE_UPPER 16
#define AE_CONVRANGE_LOWER 17

static int nt99141_x_ctrl(struct v4l2_ctrl *ctrl) {
        struct v4l2_subdev *sd =
                &container_of(ctrl->handler, struct nt99141_dev, hdl)->sd;
        struct i2c_client  *client = v4l2_get_subdevdata(sd);
	struct nt99141_dev *sensor = to_nt99141(client);
	int val;

	dev_err(&client->dev, "Request to set CUSTOM control id:%d val:%d\n", ctrl->id, ctrl->val);

        switch (ctrl->id) {
	case AEC_MIN_EXPLINE:
		dev_err(&client->dev, "AEC MIN EXPLINE !!\n");
		regmap_write(sensor->regmap, NT99141_AEC_MIN_EXPLINE, ctrl->val);
		break;
	case AEC_MAX_EXPLINE:
		dev_err(&client->dev, "AEC MAX EXPLINE !!\n");
		regmap_write(sensor->regmap, NT99141_AEC_MAX_EXPLINE, ctrl->val);
		break;
	case AGC_MIN_LIMIT:
		dev_err(&client->dev, "AGC MIN LIMIT!!\n");
		regmap_write(sensor->regmap, NT99141_AGC_MIN_LIMIT, ctrl->val);
		break;
	case AGC_MAX_LIMIT:
		dev_err(&client->dev, "AGC MAX LIMIT !!\n");
		regmap_write(sensor->regmap, NT99141_AGC_MAX_LIMIT, ctrl->val);
		break;

	case AE_CONTROL0:
		dev_err(&client->dev, "AE CONTROL0 !!\n");
		regmap_write(sensor->regmap, NT99141_AE_CONTROL0, ctrl->val);
		break;
	case AE_CONTROL1:
		dev_err(&client->dev, "AE CONTROL1 !!\n");
		regmap_write(sensor->regmap, NT99141_AE_CONTROL1, ctrl->val);
		break;
	case AEC_FLICKER_BASE_L:
		dev_err(&client->dev, "AEC_FLICKER_BASE_L !!\n");
		regmap_write(sensor->regmap, NT99141_AEC_FLICKER_BASE_L, ctrl->val);
		break;
	case AE_EV_A_L:
		dev_err(&client->dev, "AE_EV_A_L !!\n");
		regmap_write(sensor->regmap, NT99141_AE_EV_A_L, ctrl->val);
		break;
	case AE_EV_B_L:
		dev_err(&client->dev, "AE_EV_B_L !!\n");
		regmap_write(sensor->regmap, NT99141_AE_EV_B_L, ctrl->val);
		break;
	case AE_EV_C_L:
		dev_err(&client->dev, "AE_EV_C_L !!\n");
		regmap_write(sensor->regmap, NT99141_AE_EV_C_L, ctrl->val);
		break;
	case AE_EV_D_L:
		dev_err(&client->dev, "AE_EV_D_L !!\n");
		regmap_write(sensor->regmap, NT99141_AE_EV_D_L, ctrl->val);
		break;
	case AE_EV_E_L:
		dev_err(&client->dev, "AE_EV_E_L !!\n");
		regmap_write(sensor->regmap, NT99141_AE_EV_E_L, ctrl->val);
		break;
	case AE_DETCRANGE_UPPER:
		dev_err(&client->dev, "AE_DETCRANGE_UPPER !!\n");
		regmap_write(sensor->regmap, NT99141_AE_DETCRANGE_UPPER, ctrl->val);
		break;
	case AE_DETCRANGE_LOWER:
		dev_err(&client->dev, "AE_DETCRANGE_LOWER !!\n");
		regmap_write(sensor->regmap, NT99141_AE_DETCRANGE_LOWER, ctrl->val);
		break;
	case AE_CONVRANGE_UPPER:
		dev_err(&client->dev, "AE_CONVRANGE_UPPER !!\n");
		regmap_write(sensor->regmap, NT99141_AE_CONVRANGE_UPPER, ctrl->val);
		break;
	case AE_CONVRANGE_LOWER:
		dev_err(&client->dev, "AE_CONVRANGE_LOWER !!\n");
		regmap_write(sensor->regmap, NT99141_AE_CONVRANGE_LOWER, ctrl->val);
		break;
	case AE_TARGET_LUM:
		dev_err(&client->dev, "AE_TARGET_LUM !!\n");
		regmap_write(sensor->regmap, NT99141_AE_TARGET_LUM, ctrl->val);
		break;
	default:
		return -EINVAL;
	}

//	regmap_write(sensor->regmap, NT99141_REG_ACTIVATE_CTRL, 0x1);


	return 0;
};

static const struct v4l2_ctrl_ops nt99141_x_ops = {
        .s_ctrl = nt99141_x_ctrl,
};

static const struct v4l2_ctrl_config aec_min_expline = {
        .ops = &nt99141_x_ops,
        .id = AEC_MIN_EXPLINE,
        .name = "AEC_MIN_EXPLINE",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x40,
};

static const struct v4l2_ctrl_config aec_max_expline = {
        .ops = &nt99141_x_ops,
        .id = AEC_MAX_EXPLINE,
        .name = "AEC_MAX_EXPLINE",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x80,
};

static const struct v4l2_ctrl_config agc_min_limit = {
        .ops = &nt99141_x_ops,
        .id = AGC_MIN_LIMIT,
        .name = "AGC_MIN_LIMIT",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config agc_max_limit = {
        .ops = &nt99141_x_ops,
        .id = AGC_MAX_LIMIT,
        .name = "AGC_MAX_LIMIT",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x20,
};

static const struct v4l2_ctrl_config ae_control0 = {
        .ops = &nt99141_x_ops,
        .id = AE_CONTROL0,
        .name = "AE_CONTROL0",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config ae_control1 = {
        .ops = &nt99141_x_ops,
        .id = AE_CONTROL1,
        .name = "AE_CONTROL1",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config aec_flicker_base_l = {
        .ops = &nt99141_x_ops,
        .id = AEC_FLICKER_BASE_L,
        .name = "AEC_FLICKER_BASE_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x3c,
};

static const struct v4l2_ctrl_config ae_ev_a_l = {
        .ops = &nt99141_x_ops,
        .id = AE_EV_A_L,
        .name = "AE_EV_A_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x58,
};

static const struct v4l2_ctrl_config ae_ev_b_l = {
        .ops = &nt99141_x_ops,
        .id = AE_EV_B_L,
        .name = "AE_EV_B_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x68,
};

static const struct v4l2_ctrl_config ae_ev_c_l = {
        .ops = &nt99141_x_ops,
        .id = AE_EV_C_L,
        .name = "AE_EV_C_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x7C,
};

static const struct v4l2_ctrl_config ae_ev_d_l = {
        .ops = &nt99141_x_ops,
        .id = AE_EV_D_L,
        .name = "AE_EV_D_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x84,
};

static const struct v4l2_ctrl_config ae_ev_e_l = {
        .ops = &nt99141_x_ops,
        .id = AE_EV_E_L,
        .name = "AE_EV_E_L",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x98,
};

static const struct v4l2_ctrl_config ae_dectrange_upper = {
        .ops = &nt99141_x_ops,
        .id = AE_DETCRANGE_UPPER,
        .name = "AE_DETCRANGE_UPPER",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x48,
};

static const struct v4l2_ctrl_config ae_dectrange_lower = {
        .ops = &nt99141_x_ops,
        .id = AE_DETCRANGE_LOWER,
        .name = "AE_DETCRANGE_LOWER",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x38,
};

static const struct v4l2_ctrl_config ae_convrange_upper = {
        .ops = &nt99141_x_ops,
        .id = AE_CONVRANGE_UPPER,
        .name = "AE_CONVRANGE_UPPER",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x44,
};

static const struct v4l2_ctrl_config ae_convrange_lower = {
        .ops = &nt99141_x_ops,
        .id = AE_CONVRANGE_LOWER,
        .name = "AE_CONVRANGE_LOWER",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x3C,
};

static const struct v4l2_ctrl_config ae_target_lum = {
        .ops = &nt99141_x_ops,
        .id = AE_TARGET_LUM,
        .name = "AE_TARGET_LUM",
        .type = V4L2_CTRL_TYPE_INTEGER,
        .flags = V4L2_CTRL_FLAG_SLIDER,
	.min = 0,
        .max = 500,
        .step = 1,
	.def = 0x40,
};

/////////////////////////////////

static int nt99141_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct nt99141_dev *sensor;
	int ret;

	dev_err(&client->dev, "Probed nt99141\n");
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	endpoint = fwnode_graph_get_next_endpoint(
		of_fwnode_handle(client->dev.of_node), NULL);

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	sensor->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(sensor->mclk)) {
		dev_err(dev, "failed to get mclk\n");
		return PTR_ERR(sensor->mclk);
	}

	ret = clk_prepare_enable(sensor->mclk);
	if (ret)
		return ret;

        sensor->mclk_freq = clk_get_rate(sensor->mclk);
	if(sensor->mclk_freq < NT99141_MCLK_MIN || sensor->mclk_freq > NT99141_MCLK_MAX) {
		dev_err(dev, "mclk frequency out of range: %d Hz\n", sensor->mclk_freq);
		ret = -EINVAL;
		goto mclk_err;
	} else {
		dev_info(dev, "mclk frequency is:%d Hz\n", sensor->mclk_freq);
	}

	sensor->regmap = devm_regmap_init_i2c(client, &nt99141_regmap);
	if(IS_ERR(sensor->regmap)) {
		dev_err(dev, "Failed to register regmap\n");
		ret = PTR_ERR(sensor->regmap);
		goto mclk_err;
	}

	ret = nt99141_probe_dt(client, sensor);
	if(ret)
		goto mclk_err;

	ret = nt99141_init(client, sensor);
	if(ret) {
		dev_err(dev, "Failed to initialize sensor\n");
		goto mclk_err;
	}

	v4l2_i2c_subdev_init(&sensor->sd, client, &nt99141_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	v4l2_ctrl_handler_init(&sensor->hdl, 1);
	v4l2_ctrl_new_std(&sensor->hdl, &nt99141_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&sensor->hdl, &nt99141_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 1);

	v4l2_ctrl_new_std_menu_items(&sensor->hdl, &nt99141_ctrl_ops, V4L2_CID_TEST_PATTERN, ARRAY_SIZE(test_pattern_menu) - 1, 0, 0, test_pattern_menu);

	/// CUSTOM
/*
	v4l2_ctrl_new_custom(&sensor->hdl, &aec_min_expline, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &aec_max_expline, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &agc_min_limit, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &agc_max_limit, NULL);

	v4l2_ctrl_new_custom(&sensor->hdl, &ae_control0, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_control1, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &aec_flicker_base_l, NULL);

	v4l2_ctrl_new_custom(&sensor->hdl, &ae_ev_a_l, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_ev_b_l, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_ev_c_l, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_ev_d_l, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_ev_e_l, NULL);

	v4l2_ctrl_new_custom(&sensor->hdl, &ae_dectrange_upper, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_dectrange_lower, NULL);

	v4l2_ctrl_new_custom(&sensor->hdl, &ae_convrange_upper, NULL);
	v4l2_ctrl_new_custom(&sensor->hdl, &ae_convrange_lower, NULL);

	v4l2_ctrl_new_custom(&sensor->hdl, &ae_target_lum, NULL);
*/

	//// CUSTOM

	sensor->sd.ctrl_handler = &sensor->hdl;
	if (sensor->hdl.error) {
	ret = sensor->hdl.error;
		goto err_hdl;
        }

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
        if (ret < 0)
                goto err_hdl;

        ret = v4l2_async_register_subdev(&sensor->sd);
        if (ret < 0)
                goto err_videoprobe;

	// Go to powerdown mode
	nt99141_power(sensor, 0);

	dev_err(dev, "BIT 0: %lu, BIT 1: %lu, BIT 2: %lu, BIT 7: %lu\n", BIT(0), BIT(1), BIT(2), BIT(7));

	return 0;

err_videoprobe:
	media_entity_cleanup(&sensor->sd.entity);
err_hdl:
        v4l2_ctrl_handler_free(&sensor->hdl);
mclk_err:
	clk_disable_unprepare(sensor->mclk);

	return ret;
}

static int nt99141_remove(struct i2c_client *client)
{
        struct nt99141_dev *sensor = to_nt99141(client);
	dev_info(&client->dev, "Removed nt99141\n");

        v4l2_async_unregister_subdev(&sensor->sd);
        v4l2_ctrl_handler_free(&sensor->hdl);
        media_entity_cleanup(&sensor->sd.entity);
      	v4l2_device_unregister_subdev(&sensor->sd);
        clk_disable_unprepare(sensor->mclk);

	return 0;
}

static const struct i2c_device_id nt99141_id[] = {
	{"nt99141", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, nt99141_id);

static const struct of_device_id nt99141_dt_ids[] = {
	{ .compatible = "novatek,nt99141" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt99141_dt_ids);

static struct i2c_driver nt99141_i2c_driver = {
	.driver = {
		.name  = "nt99141",
		.of_match_table	= nt99141_dt_ids,
	},
	.id_table = nt99141_id,
	.probe    = nt99141_probe,
	.remove   = nt99141_remove,
};

module_i2c_driver(nt99141_i2c_driver);

MODULE_DESCRIPTION("NT99141 Camera Subdev Driver");
MODULE_LICENSE("GPL");
