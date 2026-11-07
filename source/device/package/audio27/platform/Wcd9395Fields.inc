/* SPDX-License-Identifier: GPL-2.0-only */
/* Register sequence adapted from Qualcomm WCD939x driver, OnePlus SM8850
 * 5ab2a689ff87d7d28c511f1762cf41c1b90d965a. Register definitions/fields checked
 * against wcd939x-registers.h and wcd939x-reg-{masks,shifts}.h. Playback-only
 * headphone/EAR settings are omitted from this microphone-only sequence.
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
static const WcdStep bias_setup[]={
    {0x3001,0x80,0x80,0}, /* BIAS.ANALOG_BIAS_EN */
    {0x3001,0x40,0x40,10}, /* BIAS.PRECHRG_EN */
    {0x3001,0x40,0x00,0}, /* BIAS.PRECHRG_EN */
    {0x345b,0x02,0x02,0}, /* CDC_DMIC_CTL.CLK_SCALE_EN */
    {0x31e1,0xf0,0x10,0}, /* TXFE_ICTRL_STG2CASC_ULP.ICTRL_SCBIAS_ULP0P6M */
    {0x31e1,0x0f,0x04,0}, /* TXFE_ICTRL_STG2CASC_ULP.ICTRL_STG2CASC_ULP */
    {0x31df,0x1f,0x08,0}, /* TXFE_ICTRL_STG2MAIN_ULP.ICTRL_STG2MAIN_ULP */
    {0x306b,0xe0,0xe0,0}, /* TEST_CTL_1.NOISE_FILT_RES_VAL */
    {0x306e,0xe0,0xe0,0}, /* MICB2_TEST_CTL_1.NOISE_FILT_RES_VAL */
    {0x3071,0xe0,0xe0,0}, /* MICB3_TEST_CTL_1.NOISE_FILT_RES_VAL */
    {0x3074,0xe0,0xe0,0}, /* MICB4_TEST_CTL_1.NOISE_FILT_RES_VAL */
    {0x308f,0x01,0x00,0}, /* TEST_BLK_EN2.TXFE2_MBHC_CLKRST_EN */
    {0x3409,0x10,0x10,0}, /* CDC_DIG_CLK_CTL.TXD0_CLK_EN */
    {0x3409,0x20,0x20,0}, /* CDC_DIG_CLK_CTL.TXD1_CLK_EN */
    {0x3409,0x40,0x40,0}, /* CDC_DIG_CLK_CTL.TXD2_CLK_EN */
    {0x3409,0x80,0x80,0}, /* CDC_DIG_CLK_CTL.TXD3_CLK_EN */
    {0x3408,0x10,0x10,0}, /* CDC_ANA_CLK_CTL.ANA_TX_DIV2_CLK_EN */
    {0x3417,0x01,0x01,0}, /* CDC_ANA_TX_CLK_CTL.ANA_TXSCBIAS_CLK_EN */
    {0x306c,0x07,0x01,0}, /* TEST_CTL_2.IBIAS_LDO_DRIVER */
    {0x306f,0x07,0x01,0}, /* MICB2_TEST_CTL_2.IBIAS_LDO_DRIVER */
    {0x3072,0x07,0x01,0}, /* MICB3_TEST_CTL_2.IBIAS_LDO_DRIVER */
    {0x3075,0x07,0x01,0}, /* MICB4_TEST_CTL_2.IBIAS_LDO_DRIVER */
    {0x3408,0x08,0x08,0}, /* CDC_ANA_CLK_CTL.ANA_TX_CLK_EN */
    {0x3408,0x10,0x10,0}, /* CDC_ANA_CLK_CTL.ANA_TX_DIV2_CLK_EN */
    {0x3457,0x02,0x02,0}, /* CDC_REQ_CTL.FS_RATE_4P8 */
    {0x3457,0x01,0x00,0}, /* CDC_REQ_CTL.NO_NOTCH */
};
