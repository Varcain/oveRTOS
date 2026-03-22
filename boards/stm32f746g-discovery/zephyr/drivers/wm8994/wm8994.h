/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef ZEPHYR_DRIVERS_AUDIO_WM8994_H_
#define ZEPHYR_DRIVERS_AUDIO_WM8994_H_

/*
 * WM8994 Register Addresses
 * Based on Wolfson/Cirrus Logic WM8994 datasheet
 */

/* Software Reset */
#define WM8994_REG_SW_RESET                     0x0000

/* Power Management */
#define WM8994_REG_POWER_MGMT_1                 0x0001
#define WM8994_REG_POWER_MGMT_2                 0x0002
#define WM8994_REG_POWER_MGMT_3                 0x0003
#define WM8994_REG_POWER_MGMT_4                 0x0004
#define WM8994_REG_POWER_MGMT_5                 0x0005
#define WM8994_REG_POWER_MGMT_6                 0x0006

/* Input Mixer */
#define WM8994_REG_INPUT_MIXER_1                0x0015
#define WM8994_REG_LEFT_LINE_IN_12_VOL          0x0018
#define WM8994_REG_LEFT_LINE_IN_34_VOL          0x0019
#define WM8994_REG_RIGHT_LINE_IN_12_VOL         0x001A
#define WM8994_REG_RIGHT_LINE_IN_34_VOL         0x001B
#define WM8994_REG_LEFT_OUTPUT_VOL              0x001C
#define WM8994_REG_RIGHT_OUTPUT_VOL             0x001D
#define WM8994_REG_LINE_OUTPUT_VOL              0x001E
#define WM8994_REG_HPOUT2_VOL                   0x001F
#define WM8994_REG_LEFT_OPGA_VOL                0x0020
#define WM8994_REG_RIGHT_OPGA_VOL               0x0021

/* SPKMIXL/R Attenuation */
#define WM8994_REG_SPKMIXL_ATT                  0x0022
#define WM8994_REG_SPKMIXR_ATT                  0x0023
#define WM8994_REG_SPKOUT_MIXERS                0x0024
#define WM8994_REG_CLASSD                       0x0025

/* Speaker Volume */
#define WM8994_REG_SPK_LEFT_VOL                 0x0026
#define WM8994_REG_SPK_RIGHT_VOL                0x0027

/* Input Mixer Volume */
#define WM8994_REG_INPUT_MIXER_2                0x0028
#define WM8994_REG_INPUT_MIXER_3                0x0029
#define WM8994_REG_INPUT_MIXER_4                0x002A
#define WM8994_REG_INPUT_MIXER_5                0x002B
#define WM8994_REG_INPUT_MIXER_6                0x002C

/* Output Mixer */
#define WM8994_REG_OUTPUT_MIXER_1               0x002D
#define WM8994_REG_OUTPUT_MIXER_2               0x002E
#define WM8994_REG_OUTPUT_MIXER_3               0x002F
#define WM8994_REG_OUTPUT_MIXER_4               0x0030
#define WM8994_REG_OUTPUT_MIXER_5               0x0031
#define WM8994_REG_OUTPUT_MIXER_6               0x0032
#define WM8994_REG_HPOUT2_MIXER                 0x0033
#define WM8994_REG_LINE_MIXER_1                 0x0034
#define WM8994_REG_LINE_MIXER_2                 0x0035
#define WM8994_REG_SPEAKER_MIXER                0x0036
#define WM8994_REG_ADDITIONAL_CTRL              0x0037
#define WM8994_REG_ANTIPOP_1                    0x0038
#define WM8994_REG_ANTIPOP_2                    0x0039
#define WM8994_REG_MICBIAS                      0x003A
#define WM8994_REG_LDO_1                        0x003B
#define WM8994_REG_LDO_2                        0x003C

/* Charge Pump */
#define WM8994_REG_CHARGE_PUMP_1                0x004C
#define WM8994_REG_CHARGE_PUMP_2                0x004D

/* Class W */
#define WM8994_REG_CLASS_W                      0x0051

/* DC Servo */
#define WM8994_REG_DC_SERVO_1                   0x0054
#define WM8994_REG_DC_SERVO_2                   0x0055
#define WM8994_REG_DC_SERVO_4                   0x0057
#define WM8994_REG_DC_SERVO_READBACK            0x0058

/* Analogue HP */
#define WM8994_REG_ANALOGUE_HP_1                0x0060

/* Clocking */
#define WM8994_REG_AIF1_CLOCKING_1              0x0200
#define WM8994_REG_AIF1_CLOCKING_2              0x0201
#define WM8994_REG_AIF2_CLOCKING_1              0x0204
#define WM8994_REG_AIF2_CLOCKING_2              0x0205
#define WM8994_REG_CLOCKING_1                   0x0208
#define WM8994_REG_CLOCKING_2                   0x0209
#define WM8994_REG_AIF1_RATE                    0x0210
#define WM8994_REG_AIF2_RATE                    0x0211
#define WM8994_REG_RATE_STATUS                  0x0212

/* FLL1 */
#define WM8994_REG_FLL1_CTRL_1                  0x0220
#define WM8994_REG_FLL1_CTRL_2                  0x0221
#define WM8994_REG_FLL1_CTRL_3                  0x0222
#define WM8994_REG_FLL1_CTRL_4                  0x0223
#define WM8994_REG_FLL1_CTRL_5                  0x0224

/* FLL2 */
#define WM8994_REG_FLL2_CTRL_1                  0x0240
#define WM8994_REG_FLL2_CTRL_2                  0x0241
#define WM8994_REG_FLL2_CTRL_3                  0x0242
#define WM8994_REG_FLL2_CTRL_4                  0x0243
#define WM8994_REG_FLL2_CTRL_5                  0x0244

/* AIF1 Control */
#define WM8994_REG_AIF1_CTRL_1                  0x0300
#define WM8994_REG_AIF1_CTRL_2                  0x0301
#define WM8994_REG_AIF1_MASTER_SLAVE            0x0302
#define WM8994_REG_AIF1_BCLK                    0x0303
#define WM8994_REG_AIF1_ADC_LRCLK               0x0304
#define WM8994_REG_AIF1_DAC_LRCLK               0x0305
#define WM8994_REG_AIF1_DAC_DATA                0x0306
#define WM8994_REG_AIF1_ADC_DATA                0x0307

/* AIF2 Control */
#define WM8994_REG_AIF2_CTRL_1                  0x0310
#define WM8994_REG_AIF2_CTRL_2                  0x0311
#define WM8994_REG_AIF2_MASTER_SLAVE            0x0312
#define WM8994_REG_AIF2_BCLK                    0x0313
#define WM8994_REG_AIF2_ADC_LRCLK               0x0314
#define WM8994_REG_AIF2_DAC_LRCLK               0x0315
#define WM8994_REG_AIF2_DAC_DATA                0x0316
#define WM8994_REG_AIF2_ADC_DATA                0x0317

/* AIF1 ADC1 Filters */
#define WM8994_REG_AIF1_ADC1_LEFT_VOL           0x0400
#define WM8994_REG_AIF1_ADC1_RIGHT_VOL          0x0401
#define WM8994_REG_AIF1_DAC1_LEFT_VOL           0x0402
#define WM8994_REG_AIF1_DAC1_RIGHT_VOL          0x0403
#define WM8994_REG_AIF1_ADC2_LEFT_VOL           0x0404
#define WM8994_REG_AIF1_ADC2_RIGHT_VOL          0x0405
#define WM8994_REG_AIF1_DAC2_LEFT_VOL           0x0406
#define WM8994_REG_AIF1_DAC2_RIGHT_VOL          0x0407

/* AIF1 DAC1 Filters */
#define WM8994_REG_AIF1_DAC1_FILTERS_1          0x0420
#define WM8994_REG_AIF1_DAC1_FILTERS_2          0x0421
#define WM8994_REG_AIF1_DAC2_FILTERS_1          0x0422
#define WM8994_REG_AIF1_DAC2_FILTERS_2          0x0423

/* AIF2 ADC Filters */
#define WM8994_REG_AIF2_ADC_LEFT_VOL            0x0500
#define WM8994_REG_AIF2_ADC_RIGHT_VOL           0x0501
#define WM8994_REG_AIF2_DAC_LEFT_VOL            0x0502
#define WM8994_REG_AIF2_DAC_RIGHT_VOL           0x0503

/* AIF2 DAC Filters */
#define WM8994_REG_AIF2_DAC_FILTERS_1           0x0520
#define WM8994_REG_AIF2_DAC_FILTERS_2           0x0521

/* DAC1 Mixer Volume */
#define WM8994_REG_DAC1_LEFT_MIXER_ROUTING      0x0601
#define WM8994_REG_DAC1_RIGHT_MIXER_ROUTING     0x0602
#define WM8994_REG_DAC2_MIXER_VOLUMES           0x0603
#define WM8994_REG_DAC2_LEFT_MIXER_ROUTING      0x0604
#define WM8994_REG_DAC2_RIGHT_MIXER_ROUTING     0x0605
#define WM8994_REG_AIF1_ADC1_LEFT_MIXER_ROUTING 0x0606
#define WM8994_REG_AIF1_ADC1_RIGHT_MIXER_ROUTING 0x0607
#define WM8994_REG_AIF1_ADC2_LEFT_MIXER_ROUTING 0x0608
#define WM8994_REG_AIF1_ADC2_RIGHT_MIXER_ROUTING 0x0609
#define WM8994_REG_DAC1_LEFT_VOL                0x0610
#define WM8994_REG_DAC1_RIGHT_VOL               0x0611
#define WM8994_REG_DAC2_LEFT_VOL                0x0612
#define WM8994_REG_DAC2_RIGHT_VOL               0x0613
#define WM8994_REG_DAC_SOFTMUTE                 0x0614
#define WM8994_REG_OVERSAMPLING                 0x0620
#define WM8994_REG_SIDETONE                     0x0621

/* GPIO */
#define WM8994_REG_GPIO_1                       0x0700
#define WM8994_REG_GPIO_2                       0x0701
#define WM8994_REG_GPIO_3                       0x0702
#define WM8994_REG_GPIO_4                       0x0703
#define WM8994_REG_GPIO_5                       0x0704
#define WM8994_REG_GPIO_6                       0x0705
#define WM8994_REG_GPIO_7                       0x0706
#define WM8994_REG_GPIO_8                       0x0707
#define WM8994_REG_GPIO_9                       0x0708
#define WM8994_REG_GPIO_10                      0x0709
#define WM8994_REG_GPIO_11                      0x070A
#define WM8994_REG_PULL_CTRL_1                  0x0720
#define WM8994_REG_PULL_CTRL_2                  0x0721

/* Interrupt */
#define WM8994_REG_IRQ_STATUS_1                 0x0730
#define WM8994_REG_IRQ_STATUS_2                 0x0731
#define WM8994_REG_IRQ_RAW_STATUS_2             0x0732
#define WM8994_REG_IRQ_STATUS_1_MASK            0x0738
#define WM8994_REG_IRQ_STATUS_2_MASK            0x0739
#define WM8994_REG_IRQ_CTRL                     0x0740
#define WM8994_REG_IRQ_DEBOUNCE                 0x0748

/* Write Sequencer */
#define WM8994_REG_WRITE_SEQ_CTRL_1             0x0110
#define WM8994_REG_WRITE_SEQ_CTRL_2             0x0111

/* Chip Revision */
#define WM8994_REG_CHIP_REVISION                0x0100

/*
 * Register bit definitions
 */

/* Power Management 1 (0x0001) */
#define WM8994_SPKOUTL_ENA                      BIT(12)
#define WM8994_SPKOUTR_ENA                      BIT(11)
#define WM8994_HPOUT2_ENA                       BIT(4)
#define WM8994_HPOUT1L_ENA                      BIT(9)
#define WM8994_HPOUT1R_ENA                      BIT(8)
#define WM8994_MICB2_ENA                        BIT(5)
#define WM8994_MICB1_ENA                        BIT(4)
#define WM8994_VMID_SEL_MASK                    (0x3 << 0)
#define WM8994_VMID_SEL_NORMAL                  (0x1 << 0)
#define WM8994_BIAS_ENA                         BIT(0)

/* Power Management 2 (0x0002) */
#define WM8994_TSHUT_ENA                        BIT(14)
#define WM8994_TSHUT_OPDIS                      BIT(13)
#define WM8994_OPCLK_ENA                        BIT(9)
#define WM8994_MIXINL_ENA                       BIT(9)
#define WM8994_MIXINR_ENA                       BIT(8)
#define WM8994_IN2L_ENA                         BIT(7)
#define WM8994_IN2R_ENA                         BIT(6)
#define WM8994_IN1L_ENA                         BIT(5)
#define WM8994_IN1R_ENA                         BIT(4)

/* Power Management 3 (0x0003) */
#define WM8994_LINEOUT1N_ENA                    BIT(13)
#define WM8994_LINEOUT1P_ENA                    BIT(12)
#define WM8994_LINEOUT2N_ENA                    BIT(11)
#define WM8994_LINEOUT2P_ENA                    BIT(10)
#define WM8994_SPKRVOL_ENA                      BIT(9)
#define WM8994_SPKLVOL_ENA                      BIT(8)
#define WM8994_MIXOUTLVOL_ENA                   BIT(7)
#define WM8994_MIXOUTRVOL_ENA                   BIT(6)
#define WM8994_MIXOUTL_ENA                      BIT(5)
#define WM8994_MIXOUTR_ENA                      BIT(4)

/* Power Management 4 (0x0004) */
#define WM8994_AIF2ADCL_ENA                     BIT(13)
#define WM8994_AIF2ADCR_ENA                     BIT(12)
#define WM8994_AIF1ADC2L_ENA                    BIT(11)
#define WM8994_AIF1ADC2R_ENA                    BIT(10)
#define WM8994_AIF1ADC1L_ENA                    BIT(9)
#define WM8994_AIF1ADC1R_ENA                    BIT(8)
#define WM8994_DMIC2L_ENA                       BIT(5)
#define WM8994_DMIC2R_ENA                       BIT(4)
#define WM8994_DMIC1L_ENA                       BIT(1)
#define WM8994_DMIC1R_ENA                       BIT(0)

/* Power Management 5 (0x0005) */
#define WM8994_AIF2DACL_ENA                     BIT(13)
#define WM8994_AIF2DACR_ENA                     BIT(12)
#define WM8994_AIF1DAC2L_ENA                    BIT(11)
#define WM8994_AIF1DAC2R_ENA                    BIT(10)
#define WM8994_AIF1DAC1L_ENA                    BIT(9)
#define WM8994_AIF1DAC1R_ENA                    BIT(8)
#define WM8994_DAC2L_ENA                        BIT(3)
#define WM8994_DAC2R_ENA                        BIT(2)
#define WM8994_DAC1L_ENA                        BIT(1)
#define WM8994_DAC1R_ENA                        BIT(0)
#define WM8994_ADC1L_ENA                        BIT(7)
#define WM8994_ADC1R_ENA                        BIT(6)

/* Power Management 6 (0x0006) */
#define WM8994_AIF3_TRI                         BIT(5)
#define WM8994_AIF3_ADCDAT_SRC_MASK             (0x3 << 3)
#define WM8994_AIF2_ADCDAT_SRC                  BIT(2)
#define WM8994_AIF2_DACDAT_SRC                  BIT(1)
#define WM8994_AIF1_DACDAT_SRC                  BIT(0)

/* AIF1 Clocking 1 (0x0200) */
#define WM8994_AIF1CLK_SRC_MASK                 (0x3 << 0)
#define WM8994_AIF1CLK_SRC_MCLK1                (0x0 << 0)
#define WM8994_AIF1CLK_SRC_MCLK2                (0x1 << 0)
#define WM8994_AIF1CLK_SRC_FLL1                 (0x2 << 0)
#define WM8994_AIF1CLK_SRC_FLL2                 (0x3 << 0)
#define WM8994_AIF1CLK_INV                      BIT(2)
#define WM8994_AIF1CLK_DIV                      BIT(1)
#define WM8994_AIF1CLK_ENA                      BIT(0)

/* AIF2 Clocking 1 (0x0204) */
#define WM8994_AIF2CLK_SRC_MASK                 (0x3 << 0)
#define WM8994_AIF2CLK_SRC_MCLK1                (0x0 << 0)
#define WM8994_AIF2CLK_SRC_MCLK2                (0x1 << 0)
#define WM8994_AIF2CLK_SRC_FLL1                 (0x2 << 0)
#define WM8994_AIF2CLK_SRC_FLL2                 (0x3 << 0)
#define WM8994_AIF2CLK_INV                      BIT(2)
#define WM8994_AIF2CLK_DIV                      BIT(1)
#define WM8994_AIF2CLK_ENA                      BIT(0)

/* Clocking 1 (0x0208) */
#define WM8994_SYSCLK_SRC                       BIT(0)
#define WM8994_DSP_FS2CLK_ENA                   BIT(3)
#define WM8994_DSP_FS1CLK_ENA                   BIT(2)
#define WM8994_DSP_FSINTCLK_ENA                 BIT(1)
#define WM8994_AIF2DSPCLK_ENA                   BIT(1)
#define WM8994_SYSDSPCLK_ENA                    BIT(0)

/* Clocking 2 (0x0209) */
#define WM8994_TOCLK_ENA                        BIT(0)
#define WM8994_OPCLK_DIV_MASK                   (0x7 << 0)

/* AIF1 Rate (0x0210) */
#define WM8994_AIF1_SR_MASK                     (0xF << 4)
#define WM8994_AIF1CLK_RATE_MASK                (0xF << 0)

/* AIF2 Rate (0x0211) */
#define WM8994_AIF2_SR_MASK                     (0xF << 4)
#define WM8994_AIF2CLK_RATE_MASK                (0xF << 0)

/* AIF1 Control 1 (0x0300) */
#define WM8994_AIF1_BCLK_INV                    BIT(8)
#define WM8994_AIF1_LRCLK_INV                   BIT(7)
#define WM8994_AIF1_WL_MASK                     (0x3 << 5)
#define WM8994_AIF1_WL_16BIT                    (0x0 << 5)
#define WM8994_AIF1_WL_20BIT                    (0x1 << 5)
#define WM8994_AIF1_WL_24BIT                    (0x2 << 5)
#define WM8994_AIF1_WL_32BIT                    (0x3 << 5)
#define WM8994_AIF1_FMT_MASK                    (0x3 << 3)
#define WM8994_AIF1_FMT_RIGHT_J                 (0x0 << 3)
#define WM8994_AIF1_FMT_LEFT_J                  (0x1 << 3)
#define WM8994_AIF1_FMT_I2S                     (0x2 << 3)
#define WM8994_AIF1_FMT_DSP                     (0x3 << 3)

/* AIF1 Master/Slave (0x0302) */
#define WM8994_AIF1_TRI                         BIT(15)
#define WM8994_AIF1_MSTR                        BIT(14)
#define WM8994_AIF1_CLK_FRC                     BIT(13)
#define WM8994_AIF1_LRCLK_FRC                   BIT(12)

/* AIF2 Control 1 (0x0310) */
#define WM8994_AIF2_BCLK_INV                    BIT(8)
#define WM8994_AIF2_LRCLK_INV                   BIT(7)
#define WM8994_AIF2_WL_MASK                     (0x3 << 5)
#define WM8994_AIF2_WL_16BIT                    (0x0 << 5)
#define WM8994_AIF2_WL_20BIT                    (0x1 << 5)
#define WM8994_AIF2_WL_24BIT                    (0x2 << 5)
#define WM8994_AIF2_WL_32BIT                    (0x3 << 5)
#define WM8994_AIF2_FMT_MASK                    (0x3 << 3)
#define WM8994_AIF2_FMT_RIGHT_J                 (0x0 << 3)
#define WM8994_AIF2_FMT_LEFT_J                  (0x1 << 3)
#define WM8994_AIF2_FMT_I2S                     (0x2 << 3)
#define WM8994_AIF2_FMT_DSP                     (0x3 << 3)

/* AIF2 Master/Slave (0x0312) */
#define WM8994_AIF2_TRI                         BIT(15)
#define WM8994_AIF2_MSTR                        BIT(14)
#define WM8994_AIF2_CLK_FRC                     BIT(13)
#define WM8994_AIF2_LRCLK_FRC                   BIT(12)

/* DAC1 Mixer Routing (0x0601) */
#define WM8994_AIF2DACL_TO_DAC1L                BIT(2)
#define WM8994_AIF1DAC2L_TO_DAC1L               BIT(1)
#define WM8994_AIF1DAC1L_TO_DAC1L               BIT(0)

/* DAC1 Right Mixer Routing (0x0602) */
#define WM8994_AIF2DACR_TO_DAC1R                BIT(2)
#define WM8994_AIF1DAC2R_TO_DAC1R               BIT(1)
#define WM8994_AIF1DAC1R_TO_DAC1R               BIT(0)

/* AIF1 ADC1 Left Mixer Routing (0x0606) */
#define WM8994_AIF2DACL_TO_AIF1ADC1L            BIT(2)
#define WM8994_ADCL_TO_AIF1ADC1L                BIT(1)

/* AIF1 ADC1 Right Mixer Routing (0x0607) */
#define WM8994_AIF2DACR_TO_AIF1ADC1R            BIT(2)
#define WM8994_ADCR_TO_AIF1ADC1R                BIT(1)

/* Volume control defaults */
#define WM8994_DAC_VOLUME_DEFAULT               0xC0
#define WM8994_HP_VOLUME_DEFAULT                0x2D
#define WM8994_SPK_VOLUME_DEFAULT               0x39
#define WM8994_LINE_VOLUME_DEFAULT              0x1F

/* Sample rate values for AIF1_SR[3:0] */
#define WM8994_SR_8K                            0x0
#define WM8994_SR_11_025K                       0x1
#define WM8994_SR_12K                           0x2
#define WM8994_SR_16K                           0x3
#define WM8994_SR_22_05K                        0x4
#define WM8994_SR_24K                           0x5
#define WM8994_SR_32K                           0x6
#define WM8994_SR_44_1K                         0x7
#define WM8994_SR_48K                           0x8
#define WM8994_SR_88_2K                         0x9
#define WM8994_SR_96K                           0xA

/* Clock divider values for AIF1CLK_RATE[3:0] */
#define WM8994_CLK_RATE_1                       0x0
#define WM8994_CLK_RATE_2                       0x1
#define WM8994_CLK_RATE_3                       0x2
#define WM8994_CLK_RATE_4                       0x3
#define WM8994_CLK_RATE_6                       0x4
#define WM8994_CLK_RATE_8                       0x5
#define WM8994_CLK_RATE_12                      0x6
#define WM8994_CLK_RATE_16                      0x7

#endif /* ZEPHYR_DRIVERS_AUDIO_WM8994_H_ */
