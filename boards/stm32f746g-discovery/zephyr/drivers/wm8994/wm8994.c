/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/audio/codec.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wolfson_wm8994, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#include "wm8994.h"

#define DT_DRV_COMPAT wolfson_wm8994

struct wm8994_driver_config {
	struct i2c_dt_spec i2c;
};

struct wm8994_driver_data {
	bool configured;
	bool input_enabled;
	bool output_enabled;
};

#define DEV_CFG(dev)  ((const struct wm8994_driver_config *const)dev->config)
#define DEV_DATA(dev) ((struct wm8994_driver_data *)dev->data)

static int wm8994_write_reg(const struct device *dev, uint16_t reg, uint16_t val);
static int wm8994_read_reg(const struct device *dev, uint16_t reg, uint16_t *val);
static int wm8994_update_reg(const struct device *dev, uint16_t reg, uint16_t mask, uint16_t val);

static int wm8994_write_reg(const struct device *dev, uint16_t reg, uint16_t val)
{
	const struct wm8994_driver_config *const dev_cfg = DEV_CFG(dev);
	uint8_t data[4];
	int ret;

	/* WM8994 uses 16-bit register addresses and 16-bit data */
	data[0] = (reg >> 8) & 0xFF;
	data[1] = reg & 0xFF;
	data[2] = (val >> 8) & 0xFF;
	data[3] = val & 0xFF;

	ret = i2c_write_dt(&dev_cfg->i2c, data, 4);
	if (ret != 0) {
		LOG_ERR("I2C write to codec register 0x%04x failed: %d", reg, ret);
		return ret;
	}

	LOG_DBG("REG:0x%04x VAL:0x%04x", reg, val);
	return 0;
}

static int wm8994_read_reg(const struct device *dev, uint16_t reg, uint16_t *val)
{
	const struct wm8994_driver_config *const dev_cfg = DEV_CFG(dev);
	uint8_t reg_addr[2];
	uint8_t data[2];
	int ret;

	reg_addr[0] = (reg >> 8) & 0xFF;
	reg_addr[1] = reg & 0xFF;

	ret = i2c_write_read_dt(&dev_cfg->i2c, reg_addr, 2, data, 2);
	if (ret != 0) {
		LOG_ERR("I2C read from codec register 0x%04x failed: %d", reg, ret);
		return ret;
	}

	*val = ((uint16_t)data[0] << 8) | data[1];
	LOG_DBG("REG:0x%04x VAL:0x%04x", reg, *val);
	return 0;
}

static int wm8994_update_reg(const struct device *dev, uint16_t reg, uint16_t mask, uint16_t val)
{
	uint16_t reg_val = 0;
	int ret;

	ret = wm8994_read_reg(dev, reg, &reg_val);
	if (ret != 0) {
		return ret;
	}

	reg_val = (reg_val & ~mask) | (val & mask);
	return wm8994_write_reg(dev, reg, reg_val);
}

static int wm8994_soft_reset(const struct device *dev)
{
	/* Writing any value to the software reset register performs a reset */
	return wm8994_write_reg(dev, WM8994_REG_SW_RESET, 0x0000);
}

static int wm8994_configure_clocking(const struct device *dev, struct audio_codec_cfg *cfg)
{
	uint16_t aif1_rate;
	uint32_t sample_rate;
	int ret;

	/* Determine AIF1 sample rate register value (sample rate + ratio=256) */
	switch (cfg->dai_cfg.i2s.frame_clk_freq) {
	case 8000:
		aif1_rate = 0x0003;
		sample_rate = 8000;
		break;
	case 11025:
		aif1_rate = 0x0013;
		sample_rate = 11025;
		break;
	case 12000:
		aif1_rate = 0x0023;
		sample_rate = 12000;
		break;
	case 16000:
		aif1_rate = 0x0033;
		sample_rate = 16000;
		break;
	case 22050:
		aif1_rate = 0x0043;
		sample_rate = 22050;
		break;
	case 24000:
		aif1_rate = 0x0053;
		sample_rate = 24000;
		break;
	case 32000:
		aif1_rate = 0x0063;
		sample_rate = 32000;
		break;
	case 44100:
		aif1_rate = 0x0073;
		sample_rate = 44100;
		break;
	case 48000:
		aif1_rate = 0x0083;
		sample_rate = 48000;
		break;
	case 96000:
		aif1_rate = 0x00A3;
		sample_rate = 96000;
		break;
	default:
		LOG_ERR("Unsupported sample rate: %u", cfg->dai_cfg.i2s.frame_clk_freq);
		return -EINVAL;
	}

	/* AIF1 Sample Rate */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_RATE, aif1_rate);
	if (ret != 0) {
		return ret;
	}

	/* Enable the DSP processing clock for AIF1, Enable the core clock */
	ret = wm8994_write_reg(dev, WM8994_REG_CLOCKING_1, 0x000A);
	if (ret != 0) {
		return ret;
	}

	/* Enable AIF1 Clock, AIF1 Clock Source = MCLK1 pin */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_CLOCKING_1, 0x0001);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("WM8994: Clocking configured for %uHz", sample_rate);
	return 0;
}

static int wm8994_configure_dai(const struct device *dev, struct audio_codec_cfg *cfg)
{
	uint16_t aif1_ctrl1 = 0x4000; /* ADCL_SRC=1, ADCR_SRC=1 (from left/right ADC) */
	int ret;

	/* Configure data format - reference uses 0x4010 for I2S 16-bit */
	switch (cfg->dai_type) {
	case AUDIO_DAI_TYPE_I2S:
		aif1_ctrl1 |= WM8994_AIF1_FMT_I2S;
		break;
	case AUDIO_DAI_TYPE_LEFT_JUSTIFIED:
		aif1_ctrl1 |= WM8994_AIF1_FMT_LEFT_J;
		break;
	case AUDIO_DAI_TYPE_RIGHT_JUSTIFIED:
		aif1_ctrl1 |= WM8994_AIF1_FMT_RIGHT_J;
		break;
	default:
		LOG_ERR("Unsupported DAI type: %d", cfg->dai_type);
		return -EINVAL;
	}

	/* Configure word length */
	switch (cfg->dai_cfg.i2s.word_size) {
	case 16:
		aif1_ctrl1 |= WM8994_AIF1_WL_16BIT;
		break;
	case 20:
		aif1_ctrl1 |= WM8994_AIF1_WL_20BIT;
		break;
	case 24:
		aif1_ctrl1 |= WM8994_AIF1_WL_24BIT;
		break;
	case 32:
		aif1_ctrl1 |= WM8994_AIF1_WL_32BIT;
		break;
	default:
		LOG_ERR("Unsupported word size: %u", cfg->dai_cfg.i2s.word_size);
		return -EINVAL;
	}

	/* AIF1 Word Length = 16-bits, AIF1 Format = I2S (0x4010 for 16-bit I2S) */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_CTRL_1, aif1_ctrl1);
	if (ret != 0) {
		return ret;
	}

	/* Slave mode (STM32 SAI as master) */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_MASTER_SLAVE, 0x0000);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

static int wm8994_configure_output(const struct device *dev, uint16_t *power_mgnt_reg_1)
{
	int ret;

	/*
	 * Headphone output configuration based on STM32Cube reference driver.
	 * This follows the exact sequence from the known working implementation.
	 */

	/* Enable DAC1 (Left), Enable DAC1 (Right), enable AIF1DAC1 L&R */
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_5, 0x0303);
	if (ret != 0) {
		return ret;
	}

	/* Enable the AIF1 Timeslot 0 (Left) to DAC 1 (Left) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC1_LEFT_MIXER_ROUTING, 0x0001);
	if (ret != 0) {
		return ret;
	}

	/* Enable the AIF1 Timeslot 0 (Right) to DAC 1 (Right) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC1_RIGHT_MIXER_ROUTING, 0x0001);
	if (ret != 0) {
		return ret;
	}

	/* Disable the AIF1 Timeslot 1 (Left) to DAC 2 (Left) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC2_LEFT_MIXER_ROUTING, 0x0000);
	if (ret != 0) {
		return ret;
	}

	/* Disable the AIF1 Timeslot 1 (Right) to DAC 2 (Right) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC2_RIGHT_MIXER_ROUTING, 0x0000);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("WM8994: Output mixer configured");

	/* Select DAC1 (Left) to Left Headphone Output PGA (bit 8 = DAC1L_TO_MIXOUTL) */
	ret = wm8994_write_reg(dev, WM8994_REG_OUTPUT_MIXER_1, 0x0100);
	if (ret != 0) {
		return ret;
	}

	/* Select DAC1 (Right) to Right Headphone Output PGA (bit 8 = DAC1R_TO_MIXOUTR) */
	ret = wm8994_write_reg(dev, WM8994_REG_OUTPUT_MIXER_2, 0x0100);
	if (ret != 0) {
		return ret;
	}

	/* Trigger WM8994 Write Sequencer for headphone cold startup.
	 * This internal hardware sequence properly powers up the headphone
	 * output amplifiers in the correct order. Without this, the headphone
	 * outputs may never produce audio. */
	ret = wm8994_write_reg(dev, WM8994_REG_WRITE_SEQ_CTRL_1, 0x8100);
	if (ret != 0) {
		return ret;
	}

	/* Wait for the write sequencer to complete the headphone cold-start
	 * power-up sequence. 300ms matches the STM32Cube BSP; the sequencer
	 * sets WSEQ_BUSY (bit 0 of 0x0111) while running. */
	k_msleep(300);

	/* Soft un-Mute the AIF1 Timeslot 0 DAC1 path L&R (with unmute ramp) */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_DAC1_FILTERS_1, 0x0010);
	if (ret != 0) {
		return ret;
	}

	/* Enable SPKRVOL PGA, Enable SPKMIXR, Enable SPKLVOL PGA, Enable SPKMIXL */
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_3, 0x0300);
	if (ret != 0) {
		return ret;
	}

	/* Left Speaker Mixer Volume = 0dB */
	ret = wm8994_write_reg(dev, WM8994_REG_SPKMIXL_ATT, 0x0000);
	if (ret != 0) {
		return ret;
	}

	/* Speaker output mode = Class D, Right Speaker Mixer Volume = 0dB */
	ret = wm8994_write_reg(dev, WM8994_REG_SPKMIXR_ATT, 0x0000);
	if (ret != 0) {
		return ret;
	}

	/* Unmute DAC2 (Left) to Left Speaker Mixer (SPKMIXL) path,
	 * Unmute DAC2 (Right) to Right Speaker Mixer (SPKMIXR) path */
	ret = wm8994_write_reg(dev, WM8994_REG_SPEAKER_MIXER, 0x0300);
	if (ret != 0) {
		return ret;
	}

	/* Enable Class W, Class W Envelope Tracking = AIF1 Timeslot 0 */
	ret = wm8994_write_reg(dev, WM8994_REG_CLASS_W, 0x0005);
	if (ret != 0) {
		return ret;
	}

	/* Enable bias generator, Enable VMID, Enable HPOUT1 (Left) and Enable HPOUT1 (Right) */
	*power_mgnt_reg_1 = 0x0303;
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_1, *power_mgnt_reg_1);
	if (ret != 0) {
		return ret;
	}

	/* Enable HPOUT1 (Left) and HPOUT1 (Right) intermediate stages */
	ret = wm8994_write_reg(dev, WM8994_REG_ANALOGUE_HP_1, 0x0022);
	if (ret != 0) {
		return ret;
	}

	/* Enable Charge Pump */
	ret = wm8994_write_reg(dev, WM8994_REG_CHARGE_PUMP_1, 0x9F25);
	if (ret != 0) {
		return ret;
	}

	/* Charge pump needs 15ms to reach steady-state output voltage
	 * before the headphone output mixers are connected. */
	k_msleep(15);

	/* Route output mixers to headphone PGA (common output section) */
	ret = wm8994_write_reg(dev, WM8994_REG_OUTPUT_MIXER_1, 0x0001);
	if (ret != 0) {
		return ret;
	}

	ret = wm8994_write_reg(dev, WM8994_REG_OUTPUT_MIXER_2, 0x0001);
	if (ret != 0) {
		return ret;
	}

	/* Enable Left Output Mixer (MIXOUTL), Enable Right Output Mixer (MIXOUTR)
	 * idem for SPKOUTL and SPKOUTR */
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_3, 0x0030 | 0x0300);
	if (ret != 0) {
		return ret;
	}

	/* Enable DC Servo and trigger start-up mode on left and right channels */
	ret = wm8994_write_reg(dev, WM8994_REG_DC_SERVO_1, 0x0033);
	if (ret != 0) {
		return ret;
	}

	/* DC Servo convergence time. The servo measures and cancels the DC
	 * offset at the headphone outputs. 257ms is the value used by the
	 * STM32Cube BSP; actual time depends on output load capacitance. */
	k_msleep(257);

	/* Enable HPOUT1 (Left) and HPOUT1 (Right) intermediate and output stages. Remove clamps */
	ret = wm8994_write_reg(dev, WM8994_REG_ANALOGUE_HP_1, 0x00EE);
	if (ret != 0) {
		return ret;
	}

	/* Unmute DAC 1 (Left) */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC1_LEFT_VOL, 0x00C0);
	if (ret != 0) {
		return ret;
	}

	/* Unmute DAC 1 (Right) */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC1_RIGHT_VOL, 0x00C0);
	if (ret != 0) {
		return ret;
	}

	/* Unmute the AIF1 Timeslot 0 DAC path (with unmute ramp) */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_DAC1_FILTERS_1, 0x0010);
	if (ret != 0) {
		return ret;
	}

	/* Unmute DAC 2 (Left) */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC2_LEFT_VOL, 0x00C0);
	if (ret != 0) {
		return ret;
	}

	/* Unmute DAC 2 (Right) */
	ret = wm8994_write_reg(dev, WM8994_REG_DAC2_RIGHT_VOL, 0x00C0);
	if (ret != 0) {
		return ret;
	}

	/* Unmute the AIF1 Timeslot 1 DAC2 path (with unmute ramp) */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_DAC2_FILTERS_1, 0x0010);
	if (ret != 0) {
		return ret;
	}

	/* Set headphone volume: 0x3A = +1dB, unmuted, VU bit set. */
	ret = wm8994_write_reg(dev, WM8994_REG_LEFT_OUTPUT_VOL, 0x3A | 0x0140);
	if (ret != 0) {
		return ret;
	}

	ret = wm8994_write_reg(dev, WM8994_REG_RIGHT_OUTPUT_VOL, 0x3A | 0x0140);
	if (ret != 0) {
		return ret;
	}

	/* Set speaker volume */
	ret = wm8994_write_reg(dev, WM8994_REG_SPK_LEFT_VOL, 0x3F | 0x0140);
	if (ret != 0) {
		return ret;
	}

	ret = wm8994_write_reg(dev, WM8994_REG_SPK_RIGHT_VOL, 0x3F | 0x0140);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("WM8994: DAC enabled");
	return 0;
}

static int wm8994_configure_input(const struct device *dev)
{
	int ret;

	/*
	 * Input configuration for LINE_IN (INPUT_LINE_1) based on STM32Cube reference.
	 * This configures the WM8994 to capture audio from the line-in jack.
	 * Order matches the proven working reference implementation.
	 */

	/* IN1LN_TO_IN1L, IN1LP_TO_VMID, IN1RN_TO_IN1R, IN1RP_TO_VMID */
	ret = wm8994_write_reg(dev, WM8994_REG_INPUT_MIXER_2, 0x0011);
	if (ret != 0) {
		return ret;
	}

	/* Unmute IN1L_TO_MIXINL, 0dB, NO output-to-input feedback.
	 * Bits[2:0] = MIXOUTL_MIXINL_VOL must be 0 to prevent output mixer
	 * feeding back into input mixer (old 0x0025 had feedback enabled). */
	ret = wm8994_write_reg(dev, WM8994_REG_INPUT_MIXER_3, 0x0020);
	if (ret != 0) {
		return ret;
	}

	/* Unmute IN1R_TO_MIXINR, 0dB, no feedback */
	ret = wm8994_write_reg(dev, WM8994_REG_INPUT_MIXER_4, 0x0020);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("WM8994: Input mixer configured");

	/* Enable AIF1ADC1 (Left), Enable AIF1ADC1 (Right)
	 * Enable Left ADC, Enable Right ADC */
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_4, 0x0303);
	if (ret != 0) {
		return ret;
	}

	/* Enable AIF1 DRC1 Signal Detect & DRC in AIF1ADC1 Left/Right Timeslot 0 */
	ret = wm8994_write_reg(dev, 0x0440, 0x00DB);
	if (ret != 0) {
		return ret;
	}

	/* Enable IN1L and IN1R, Disable IN2L and IN2R, Enable Thermal sensor & shutdown */
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_2, 0x6350);
	if (ret != 0) {
		return ret;
	}

	/* Enable the ADCL(Left) to AIF1 Timeslot 0 (Left) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_ADC1_LEFT_MIXER_ROUTING, 0x0002);
	if (ret != 0) {
		return ret;
	}

	/* Enable the ADCR(Right) to AIF1 Timeslot 0 (Right) mixer path */
	ret = wm8994_write_reg(dev, WM8994_REG_AIF1_ADC1_RIGHT_MIXER_ROUTING, 0x0002);
	if (ret != 0) {
		return ret;
	}

	/* GPIO1 pin configuration GP1_DIR = output, GP1_FN = AIF1 DRC1 signal detect */
	ret = wm8994_write_reg(dev, WM8994_REG_GPIO_1, 0x000D);
	if (ret != 0) {
		return ret;
	}

	/* ADC oversample enable - required for proper ADC operation */
	ret = wm8994_write_reg(dev, WM8994_REG_OVERSAMPLING, 0x0002);
	if (ret != 0) {
		return ret;
	}

	/* AIF ADC1 HPF enable, HPF cut = hifi mode fc=4Hz at fs=48kHz */
	ret = wm8994_write_reg(dev, 0x410, 0x1800);
	if (ret != 0) {
		return ret;
	}

	/* Disable mute on IN1L, IN1L Volume = 0dB (0x0B).
	 * Was +9dB (0x11) but hot signals clip at that level. */
	ret = wm8994_write_reg(dev, WM8994_REG_LEFT_LINE_IN_12_VOL, 0x000B);
	if (ret != 0) {
		return ret;
	}

	/* Disable mute on IN1R, IN1R Volume = 0dB (0x0B) */
	ret = wm8994_write_reg(dev, WM8994_REG_RIGHT_LINE_IN_12_VOL, 0x000B);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("WM8994: ADC enabled");
	return 0;
}

static int wm8994_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct wm8994_driver_data *data = DEV_DATA(dev);
	int ret;
	bool input_needed;
	uint16_t power_mgnt_reg_1 = 0;

	LOG_INF("Configuring WM8994 codec");

	if (cfg->dai_type >= AUDIO_DAI_TYPE_INVALID) {
		LOG_ERR("Invalid DAI type");
		return -EINVAL;
	}

	/* Determine if input is needed */
	input_needed = (cfg->dai_route == AUDIO_ROUTE_CAPTURE ||
			cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE);

	/* Perform soft reset */
	ret = wm8994_soft_reset(dev);
	if (ret != 0) {
		LOG_ERR("Soft reset failed");
		return ret;
	}

	/* Post-reset settling time. The WM8994 datasheet requires a brief
	 * wait after soft reset before register access is reliable. */
	k_msleep(10);

	/* WM8994 Errata Work-Arounds */
	ret = wm8994_write_reg(dev, 0x102, 0x0003);
	if (ret != 0) {
		return ret;
	}
	ret = wm8994_write_reg(dev, 0x817, 0x0000);
	if (ret != 0) {
		return ret;
	}
	ret = wm8994_write_reg(dev, 0x102, 0x0000);
	if (ret != 0) {
		return ret;
	}

	/* Enable VMID soft start (fast), Start-up Bias Current Enabled */
	ret = wm8994_write_reg(dev, WM8994_REG_ANTIPOP_2, 0x006C);
	if (ret != 0) {
		return ret;
	}

	/* Enable bias generator, Enable VMID
	 * If input is needed, also enable Microphone bias 1 generator */
	if (input_needed) {
		power_mgnt_reg_1 = 0x0013; /* MICB1_ENA + VMID + BIAS_ENA */
	} else {
		power_mgnt_reg_1 = 0x0003; /* VMID + BIAS_ENA only */
	}
	ret = wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_1, power_mgnt_reg_1);
	if (ret != 0) {
		return ret;
	}

	/* VMID soft-start settling time. The reference voltage generator
	 * ramps up slowly to avoid audible pops; 50ms allows it to reach
	 * a stable operating point before enabling signal paths. */
	k_msleep(50);

	if (cfg->dai_route == AUDIO_ROUTE_BYPASS) {
		data->configured = true;
		return 0;
	}

	/* Configure clocking */
	ret = wm8994_configure_clocking(dev, cfg);
	if (ret != 0) {
		LOG_ERR("Clock configuration failed");
		return ret;
	}

	/* Configure DAI */
	ret = wm8994_configure_dai(dev, cfg);
	if (ret != 0) {
		LOG_ERR("DAI configuration failed");
		return ret;
	}

	/* Configure output if needed */
	if (cfg->dai_route == AUDIO_ROUTE_PLAYBACK ||
	    cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE) {
		ret = wm8994_configure_output(dev, &power_mgnt_reg_1);
		if (ret != 0) {
			LOG_ERR("Output configuration failed");
			return ret;
		}
	}

	/* Configure input if needed */
	if (cfg->dai_route == AUDIO_ROUTE_CAPTURE ||
	    cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE) {
		ret = wm8994_configure_input(dev);
		if (ret != 0) {
			LOG_ERR("Input configuration failed");
			return ret;
		}
	}

	data->configured = true;
	LOG_INF("WM8994 codec configured successfully");

	return 0;
}

static void wm8994_start_output(const struct device *dev)
{
	LOG_DBG("Start output");
}

static void wm8994_stop_output(const struct device *dev)
{
	LOG_DBG("Stop output");
}

static int wm8994_start(const struct device *dev, audio_dai_dir_t dir)
{
	struct wm8994_driver_data *data = DEV_DATA(dev);

	LOG_DBG("WM8994 start called with dir: %d", dir);

	if (dir == AUDIO_DAI_DIR_TX || dir == AUDIO_DAI_DIR_TXRX) {
		LOG_DBG("Starting output");
		wm8994_start_output(dev);
		data->output_enabled = true;
	}

	if (dir == AUDIO_DAI_DIR_RX || dir == AUDIO_DAI_DIR_TXRX) {
		if (data->input_enabled) {
			LOG_DBG("Input already started");
			return 0;
		}

		LOG_DBG("Starting input");
		/* Input is configured during wm8994_configure_input, just mark as enabled */
		data->input_enabled = true;
	}

	return 0;
}

static int wm8994_stop(const struct device *dev, audio_dai_dir_t dir)
{
	struct wm8994_driver_data *data = DEV_DATA(dev);

	if (dir == AUDIO_DAI_DIR_TX || dir == AUDIO_DAI_DIR_TXRX) {
		LOG_DBG("Stop output");
		wm8994_stop_output(dev);
		data->output_enabled = false;
	}

	if (dir == AUDIO_DAI_DIR_RX || dir == AUDIO_DAI_DIR_TXRX) {
		if (!data->input_enabled) {
			LOG_DBG("Input already stopped");
			return 0;
		}

		LOG_DBG("Stop input");

		/* Disable AIF1ADC1 (Left), Disable AIF1ADC1 (Right) */
		wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_4, 0);

		/* Disable the ADCL(Left) to AIF1 Timeslot 0 (Left) mixer path */
		wm8994_write_reg(dev, WM8994_REG_AIF1_ADC1_LEFT_MIXER_ROUTING, 0);

		/* Disable the ADCR(Right) to AIF1 Timeslot 0 (Right) mixer path */
		wm8994_write_reg(dev, WM8994_REG_AIF1_ADC1_RIGHT_MIXER_ROUTING, 0);

		/* Disable Left ADC, Disable Right ADC */
		wm8994_write_reg(dev, WM8994_REG_POWER_MGMT_5, 0);

		data->input_enabled = false;
	}

	return 0;
}

static int wm8994_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	uint16_t vol;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		/* Convert to WM8994 volume range (0-63 for headphone) */
		vol = (uint16_t)MIN(val.vol, 0x3F);
		if (channel == AUDIO_CHANNEL_FRONT_LEFT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_write_reg(dev, WM8994_REG_LEFT_OUTPUT_VOL, BIT(8) | vol);
		}
		if (channel == AUDIO_CHANNEL_FRONT_RIGHT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_write_reg(dev, WM8994_REG_RIGHT_OUTPUT_VOL, BIT(8) | vol);
		}
		return 0;

	case AUDIO_PROPERTY_OUTPUT_MUTE:
		if (val.mute) {
			wm8994_write_reg(dev, WM8994_REG_AIF1_DAC1_FILTERS_1, BIT(9));
		} else {
			wm8994_write_reg(dev, WM8994_REG_AIF1_DAC1_FILTERS_1, 0x0000);
		}
		return 0;

	case AUDIO_PROPERTY_INPUT_VOLUME:
		/* Convert to WM8994 volume range (0-31 for line input) */
		vol = (uint16_t)MIN(val.vol, 0x1F);
		if (channel == AUDIO_CHANNEL_FRONT_LEFT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_write_reg(dev, WM8994_REG_LEFT_LINE_IN_12_VOL, BIT(8) | vol);
		}
		if (channel == AUDIO_CHANNEL_FRONT_RIGHT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_write_reg(dev, WM8994_REG_RIGHT_LINE_IN_12_VOL, BIT(8) | vol);
		}
		return 0;

	case AUDIO_PROPERTY_INPUT_MUTE:
		if (channel == AUDIO_CHANNEL_FRONT_LEFT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_update_reg(dev, WM8994_REG_LEFT_LINE_IN_12_VOL, BIT(7),
					  val.mute ? BIT(7) : 0);
		}
		if (channel == AUDIO_CHANNEL_FRONT_RIGHT || channel == AUDIO_CHANNEL_ALL) {
			wm8994_update_reg(dev, WM8994_REG_RIGHT_LINE_IN_12_VOL, BIT(7),
					  val.mute ? BIT(7) : 0);
		}
		return 0;

	default:
		return -EINVAL;
	}
}

static int wm8994_apply_properties(const struct device *dev)
{
	/* Update VU bits to apply volume settings */
	wm8994_update_reg(dev, WM8994_REG_LEFT_OUTPUT_VOL, BIT(8), BIT(8));
	wm8994_update_reg(dev, WM8994_REG_RIGHT_OUTPUT_VOL, BIT(8), BIT(8));

	return 0;
}

static int wm8994_route_input(const struct device *dev, audio_channel_t channel, uint32_t input)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(input);

	/* WM8994 input routing is configured during initialization */
	return 0;
}

static const struct audio_codec_api wm8994_driver_api = {
	.configure = wm8994_configure,
	.start_output = wm8994_start_output,
	.stop_output = wm8994_stop_output,
	.set_property = wm8994_set_property,
	.apply_properties = wm8994_apply_properties,
	.route_input = wm8994_route_input,
	.start = wm8994_start,
	.stop = wm8994_stop,
};

static int wm8994_init(const struct device *dev)
{
	const struct wm8994_driver_config *const dev_cfg = DEV_CFG(dev);
	struct wm8994_driver_data *data = DEV_DATA(dev);

	if (!i2c_is_ready_dt(&dev_cfg->i2c)) {
		LOG_ERR("I2C bus is not ready");
		return -ENODEV;
	}

	/* Initialize state flags */
	data->input_enabled = false;
	data->output_enabled = false;
	data->configured = false;

	LOG_INF("WM8994 codec initialized");
	return 0;
}

#define WM8994_INIT(n)                                                                             \
	static struct wm8994_driver_data wm8994_data_##n;                                          \
	static const struct wm8994_driver_config wm8994_config_##n = {                             \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, wm8994_init, NULL, &wm8994_data_##n, &wm8994_config_##n,          \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY, &wm8994_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WM8994_INIT)
