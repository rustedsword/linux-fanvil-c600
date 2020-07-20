/*
 * cx2070x codec driver
 *
 * Copyright: 2020 Aleksandrov Stanislav
 *
 * Copyright: 2017-2020 Arcturus Networks Inc.
 *                      by Oleksandr Zhadan
 *
 * Copyright: 2009/2010 Conexant Systems
 *                      by Simon Ho <simon.ho@conexant.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/firmware.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/pcm_params.h>

#include "cx2070x.h"

struct cx2070x_priv {
	struct regmap *regmap;
	int master[NUM_OF_DAI];
	u8 cached_rate[NUM_OF_DAI];
	struct device *dev;
};

#define CX2070X_RATES	(  \
      SNDRV_PCM_RATE_8000  \
    | SNDRV_PCM_RATE_11025 \
    | SNDRV_PCM_RATE_16000 \
    | SNDRV_PCM_RATE_22050 \
    | SNDRV_PCM_RATE_32000 \
    | SNDRV_PCM_RATE_44100 \
    | SNDRV_PCM_RATE_48000 \
    | SNDRV_PCM_RATE_88200 \
    | SNDRV_PCM_RATE_96000 )

#define CX2070X_FORMATS (     \
      SNDRV_PCM_FMTBIT_S16_LE \
    | SNDRV_PCM_FMTBIT_S24_LE \
    | SNDRV_PCM_FMTBIT_MU_LAW \
    | SNDRV_PCM_FMTBIT_A_LAW)

static bool cx2070x_volatile(struct device *dev, unsigned int reg)
{
	(void)dev;
	switch (reg) {
	case CXREG_ABCODE:
	case CXREG_UPDATE_CTR:
	case CXREG_DSP_INIT_NEWC:
	/* Not sure about this gpio register, but just in case */
	case CXREG_GPIO_CONTROL:
		return 1;
	default:
		return 0;
	}
}

const struct regmap_config cx2070x_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = CX2070X_REG_MAX,
	.volatile_reg = cx2070x_volatile,
	.cache_type = REGCACHE_RBTREE,
};

EXPORT_SYMBOL_GPL(cx2070x_regmap);

static int cx2070x_set_dai_sysclk(struct snd_soc_dai *dai, int clk_id, unsigned int freq, int dir) {
	int ret;
	struct snd_soc_component *component = dai->component;
	unsigned val = 0;

	dev_info(component->dev, "Set dat sysclk, frequency: %uHz, clock direction: %s\n", freq, dir == SND_SOC_CLOCK_OUT ? "output" : "input");
	ret = snd_soc_component_read(component, CXREG_I2S_OPTION, &val);
	if(ret)
		return ret;

	val &= ~0x10;
	if (dir == SND_SOC_CLOCK_OUT) {
		switch (freq) {
		case 2048000:
			val |= 0;
			break;
		case 4096000:
			val |= 1;
			break;
		case 5644000:
			val |= 2;
			break;
		case 6144000:
			val |= 3;
			break;
		case 8192000:
			val |= 4;
			break;
		case 11289000:
			val |= 5;
			break;
		case 12288000:
			val |= 6;
			break;
		case 24576000:
			val |= 10;
			break;
		case 22579000:
			val |= 11;
			break;
		default:
			dev_err(dai->dev, "Unsupported MCLK rate %uHz!\n", freq);
			return -EINVAL;
		}
		dev_err(dai->dev, "Selected rate %uHz!, writing 0x%x\n", freq, val | 0x10);
		val |= 0x10;	/*enable MCLK output */
	}

	ret = snd_soc_component_write(component, CXREG_I2S_OPTION, val);
	if(ret)
		return ret;

	return 0;
}

static int cx2070x_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt) {
	struct snd_soc_component *component = dai->component;
	struct cx2070x_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;

	uint8_t is_pcm = 0;
	uint8_t is_frame_invert = 0;
	uint8_t is_clk_invert = 0;
	uint8_t is_right_j = 0;
	uint8_t is_one_delay = 0;
	uint8_t val;

	dev_info(component->dev, "cx2070x Set dai format: %d\n", fmt);
	if (dai->id > NUM_OF_DAI) {
		dev_err(dai->dev, "Unknown dai configuration,dai->id = %d\n",
			dai->id);
		return -EINVAL;
	}

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		priv->master[dai->id] = 0;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		priv->master[dai->id] = 1;
		break;
	default:
		dev_err(dai->dev, "Unsupported master/slave configuration\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_B:
		/*PCM short frame sync */
		is_pcm = 1;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		/*PCM short frame sync with one cycle delay */
		is_pcm = 1;
		is_one_delay = 1;
		break;
	case SND_SOC_DAIFMT_I2S:
		is_one_delay = 1;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		is_right_j = 1;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	default:
		dev_err(dai->dev, "Unsupported dai format %d\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		if (is_pcm) {
			dev_err(dai->dev, "Can't support invert frame in PCM mode\n");
			return -EINVAL;
		}
		is_frame_invert = 1;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		is_clk_invert = 1;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		if (is_pcm) {
			dev_err(dai->dev, "Can't support invert frame in PCM mode\n");
			return -EINVAL;
		}
		is_frame_invert = 1;
		is_clk_invert = 1;
		break;
	}

	val =
		(is_one_delay << 7) | (is_right_j << 6) | (is_clk_invert << 3) |
		(is_clk_invert << 2) | (is_frame_invert << 1) | (is_pcm);

	dev_info(dai->dev, "Writing to dai_id: %d , port_control: 0x%x\n", dai->id, val);

	ret = snd_soc_component_update_bits(component,
					    dai->id ? CXREG_PORT2_CONTROL : CXREG_PORT1_CONTROL,
					    0xc0, val);

	if(ret < 0)
		return ret;

	return 0;
}

static const unsigned int cx2070x_rates_map[] = {
	8000, 11025, 16000, 22050,
	24000, 32000, 44100,
	48000, 88200, 96000
};

static bool cx2070x_find_rate(u8 *reg_val, const unsigned int rate) {
	unsigned int i;
	for(i = 0; i < ARRAY_SIZE(cx2070x_rates_map); i++) {
		if(cx2070x_rates_map[i] == rate) {
			*reg_val |= i;
			return true;
		}
	}
	return false;
}

static const unsigned int cx2070x_bitrates_map[] = {
	6144000, 4096000, 3072000, 2048000,
	1536000, 1024000, 768000,  512000,
	384000,  256000,  5644000, 2822000,
	1411000, 705000,  352000
};

static bool cx2070x_find_bitrate(u8 *reg_val, const unsigned int bitrate) {
	unsigned int i;
	for(i = 0; i < ARRAY_SIZE(cx2070x_bitrates_map); i++) {
		if(cx2070x_bitrates_map[i] == bitrate) {
			*reg_val = i;
			return true;
		}
	}
	return false;
}

static int cx2070x_setup_rates(struct snd_soc_component *component,
			       struct snd_soc_dai *dai,
			       u8 rate_val)
{
	unsigned s3_input_route, s4_input_route;
	int ret;

	/* To setup playback stream we need to find out
	 * to what input codec stream current dai is connected. */
	ret = snd_soc_component_read(component, CXREG_STREAM3_ROUTE, &s3_input_route);
	if(!ret)
		ret = snd_soc_component_read(component, CXREG_STREAM4_ROUTE, &s4_input_route);
	if(ret)
		return ret;

	s3_input_route = (s3_input_route & CX_STREAM34_ROUTE_MASK) >> 3;
	s4_input_route = (s4_input_route & CX_STREAM34_ROUTE_MASK) >> 3;

	/* Current dai may be connected to both codec streams at once */
	if((int)s3_input_route == dai->id) {
		ret = snd_soc_component_update_bits(component, CXREG_STREAM3_RATE, CX_RATE_MASK, rate_val);
		if(ret < 0)
			return ret;
	}
	if((int)s4_input_route == dai->id) {
		ret = snd_soc_component_update_bits(component, CXREG_STREAM4_RATE, CX_RATE_MASK, rate_val);
		if(ret < 0)
			return ret;
	}

	/* Setup capure streams */
	return snd_soc_component_update_bits(component,
					     dai->id ? CXREG_STREAM6_RATE : CXREG_STREAM5_RATE,
					     CX_RATE_MASK, rate_val);
}

static int cx2070x_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cx2070x_priv *priv = snd_soc_component_get_drvdata(component);
	u8 val = 0;
	u8 sample_size;
	u32 bit_rate;
	u32 frame_size;
	u32 num_ch = 2;
	int ret;

	dev_info(component->dev, "Set hw params for direction %s, "
				 "dai_id:%d, rate:%d, channels:%d, format:%d\n",
		 substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
		 dai->id, params_rate(params), params_channels(params), params_format(params));

	/*turn off bit clock output */
	ret = snd_soc_component_update_bits(component, CXREG_CLOCK_DIVIDER,
					    dai->id ? 0x0f << 4 : 0x0f,
					    dai->id ? 0x0f << 4 : 0xf);

	if(ret < 0)
		return ret;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_A_LAW:
		val |= 0 << 4;
		sample_size = 1;
		break;
	case SNDRV_PCM_FORMAT_MU_LAW:
		val |= 1 << 4;
		sample_size = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		val |= 2 << 4;
		sample_size = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		val |= 3 << 4;
		sample_size = 3;
		break;
	default:
		dev_warn(dai->dev, "Unsupported format %d\n", params_format(params));
		return -EINVAL;
	}

	if(!cx2070x_find_rate(&val, params_rate(params))) {
		dev_warn(dai->dev, "Unsupported sample rate %d\n", params_rate(params));
		return -EINVAL;
	}

	ret = cx2070x_setup_rates(component, dai, val);
	if(ret < 0)
		return ret;

	/* Store calculated rate register value in case of dynamic switching
	 * between dais */
	priv->cached_rate[dai->id] = val;

	/*set bit clock */
	frame_size = snd_soc_params_to_frame_size(params);

	/* TODO: This is hack, need to calculate proper parameters */
	if(params_channels(params) == 1)
		frame_size *= 2;

	bit_rate = frame_size * params_rate(params);

	dev_info(dai->dev, "bit rate at %uHz, master = %d\n", bit_rate,	priv->master[dai->id]);
	dev_info(dai->dev, "sample size = %d bytes, sample rate = %uHz, frame_size %u\n",  sample_size, params_rate(params), frame_size);

	val = sample_size - 1;
	if(val == 2) {
		dev_info(component->dev, "Updating val to 3\n");
		val = 3;
	}
	if (dai->id == 0) {
		/*TODO: only I2S mode is implemented. */
		val |= val << 2;

		ret = snd_soc_component_write(component, CXREG_PORT1_TX_FRAME, frame_size / 8 - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT1_RX_FRAME, frame_size / 8 - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT1_TX_SYNC, frame_size / num_ch - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT1_RX_SYNC, frame_size / num_ch - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT1_CONTROL2, val);
		if(ret)
			return ret;
	} else {
		/* This cannot be tested with cx20707 codec, since it has no second i2s connection =( */

		ret = snd_soc_component_write(component, CXREG_PORT2_FRAME, frame_size / 8 - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT2_SYNC, frame_size / num_ch - 1);
		if(!ret)
			ret = snd_soc_component_write(component, CXREG_PORT2_SAMPLE, val);
		if(ret)
			return ret;
	}

	if (!priv->master[dai->id]) {
		val = 0xf;
	} else {
		bit_rate /= 1000;
		bit_rate *= 1000;
		if(!cx2070x_find_bitrate(&val, bit_rate)) {
			dev_err(dai->dev, "Unsupported bit rate %uHz\n", bit_rate);
			return -EINVAL;
		}
	}

	ret = snd_soc_component_update_bits(component, CXREG_CLOCK_DIVIDER,
					    dai->id ? CX_CLKDIV_PORT2_MASK : CX_CLKDIV_PORT1_MASK,
					    dai->id ? val << 4 : val);
	if(ret < 0)
		return ret;

	return 0;
}

static const struct snd_soc_dai_ops cx2070x_dai_ops = {
	.set_sysclk = cx2070x_set_dai_sysclk,
	.set_fmt = cx2070x_set_dai_fmt,
	.hw_params = cx2070x_hw_params,
};

static struct snd_soc_dai_driver cx2070x_dais[] = {
{
	.name = DAI_DP1_NAME,
	.ops = &cx2070x_dai_ops,
	.capture = {
	    .stream_name = CAPTURE_STREAM_NAME_1,
	    .formats     = CX2070X_FORMATS,
	    .rates       = CX2070X_RATES,
	    .channels_min = 1,
	    .channels_max = 2,
	},
	.playback= {
	    .stream_name = PLAYBACK_STREAM_NAME_1,
	    .formats     = CX2070X_FORMATS,
	    .rates       = CX2070X_RATES,
	    .channels_min = 1,
	    .channels_max = 2,
	},
	.symmetric_rates = 1,
},
{
	.name = DAI_DP2_NAME,
	.ops = &cx2070x_dai_ops,
	.capture = {
	    .stream_name = CAPTURE_STREAM_NAME_2,
	    .formats     = CX2070X_FORMATS,
	    .rates       = CX2070X_RATES,
	    .channels_min = 1,
	    .channels_max = 2,
	},
	.playback= {
	    .stream_name = PLAYBACK_STREAM_NAME_2,
	    .formats     = CX2070X_FORMATS,
	    .rates       = CX2070X_RATES,
	    .channels_min = 1,
	    .channels_max = 2,
	},
	.symmetric_rates = 1,
},
};

static int apply_dsp_change(struct snd_soc_component *component)
{
	unsigned int try_loop = 50;
	unsigned ret;
	unsigned int val = 0;

	ret = snd_soc_component_update_bits(component, CXREG_DSP_INIT_NEWC, 1, 1);
	if(ret < 0)
		return ret;

	for (; try_loop; try_loop--) {
		ret = snd_soc_component_read(component, CXREG_DSP_INIT_NEWC, &val);
		if(ret)
			return ret;

		if((val & 1) == 0)
			return 0;

		udelay(1);
	}

	dev_err(component->dev, "Failed to apply dsp parameters\n");
	return -EBUSY;
}

static int dsp_put(struct snd_kcontrol *kcontrol,
		   struct snd_ctl_elem_value *ucontrol)
{
	unsigned ret, ret2;
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);

	ret = snd_soc_put_volsw(kcontrol, ucontrol);
	if(ret < 0)
		return ret;

	ret2 = apply_dsp_change(component);
	if(ret2 < 0)
		return ret2;

	return ret;
}

/*
 * Mixer 0 and 1 Gain
 * min: 0x4a : -74 dB
 *      (1db Step)
 * max: 0x00 : 0 dB
 */
static const DECLARE_TLV_DB_SCALE(mix_in_tlv, -7400, 100, 0);

/*
 * Analog Microphone boost
 * min: 0x0
 *      ( 6 dB step )
 * max: 0x7 : 42 dB
 */
static const DECLARE_TLV_DB_SCALE(mic_boost_tlv, 0, 600, 0);

/*
 * ADC/DAC Volume
 * min : 0xB6 : -74 dB
 *       ( 1 dB step )
 * max : 0x05 : 5 dB
 */
static const DECLARE_TLV_DB_SCALE(main_tlv, -7400, 100, 0);

/*
 * Capture Volume
 * min : 0x1f : -35 dB
 *       ( 1.5 dB per step )
 * max : 0x00 : 12 dB
 */
static const DECLARE_TLV_DB_SCALE(line_tlv, -3500, 150, 0);

static const char *const classd_gain_texts[] = {
    "2.8W", "2.6W", "2.5W", "2.4W", "2.3W", "2.2W", "2.1W", "2.0W",
    "1.3W", "1.25W", "1.2W", "1.15W", "1.1W", "1.05W", "1.0W", "0.9W"
};

#define SOC_ENUM_SINGLE_ARRAY(xreg, xshift, xtexts_array) \
	SOC_ENUM_SINGLE(xreg, xshift, ARRAY_SIZE(xtexts_array), xtexts_array)

static const struct soc_enum classd_gain_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_CLASSD_GAIN, 0, classd_gain_texts);

static const char *mic_bias_texts[] = {"1.65V", "2.64V"};
static const struct soc_enum mic_bias_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_MIC_CONTROL, 3, mic_bias_texts);

static const struct snd_kcontrol_new hp_switch =
	SOC_DAPM_SINGLE("Switch", CXREG_OUTPUT_CONTROL, CX_OUTPUT_HP_SHIFT, 1, 0);
static const struct snd_kcontrol_new lineout_switch =
	SOC_DAPM_SINGLE("Switch", CXREG_OUTPUT_CONTROL, CX_OUTPUT_LO_SHIFT, 1, 0);
static const struct snd_kcontrol_new classd_switch =
	SOC_DAPM_SINGLE("Switch", CXREG_OUTPUT_CONTROL, CX_OUTPUT_SPEAKER_SHIFT, 1, 0);
static const struct snd_kcontrol_new function_gen_switch =
	SOC_DAPM_SINGLE("Switch", CXREG_DSP_ENABLE2, 5, 1, 0);

static const struct snd_kcontrol_new strm1_sel_mix[] = {
	SOC_DAPM_SINGLE("Line 1 Capture Switch", CXREG_INPUT_CONTROL, CX_INPUT_LINE1_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("Line 2 Capture Switch", CXREG_INPUT_CONTROL, CX_INPUT_LINE2_SHIFT, 1, 0),
	SOC_DAPM_SINGLE("Line 3 Capture Switch", CXREG_INPUT_CONTROL, CX_INPUT_LINE3_SHIFT, 1, 0),
};

static const struct snd_kcontrol_new lines_in_volumes[] = {
	SOC_DAPM_SINGLE_TLV("Volume", CXREG_LINE1_GAIN, 0, 32, 0, line_tlv),
	SOC_DAPM_SINGLE_TLV("Volume", CXREG_LINE2_GAIN, 0, 32, 0, line_tlv),
	SOC_DAPM_SINGLE_TLV("Volume", CXREG_LINE3_GAIN, 0, 32, 0, line_tlv),
};

static const struct snd_kcontrol_new left_mic_controls[] = {
	SOC_DAPM_SINGLE_TLV("Capture Volume", CXREG_MIC_CONTROL, 0, 7, 0, mic_boost_tlv),
	SOC_DAPM_SINGLE("Capture Switch", CXREG_VOLUME_MUTE, CX_MUTE_MIC_L_SHIFT, 1, 1),
};

static const struct snd_kcontrol_new mixer0_controls[] = {
	SOC_DAPM_SINGLE_TLV("Input 0 Volume", CXREG_MIX0_IN0_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 1 Volume", CXREG_MIX0_IN1_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 2 Volume", CXREG_MIX0_IN2_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 3 Volume", CXREG_MIX0_IN3_GAIN, 0, 74, 1, mix_in_tlv),
};

static const struct snd_kcontrol_new mixer1_controls[] = {
	SOC_DAPM_SINGLE_TLV("Input 0 Volume", CXREG_MIX1_IN0_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 1 Volume", CXREG_MIX1_IN1_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 2 Volume", CXREG_MIX1_IN2_GAIN, 0, 74, 1, mix_in_tlv),
	SOC_DAPM_SINGLE_TLV("Input 3 Volume", CXREG_MIX1_IN3_GAIN, 0, 74, 1, mix_in_tlv),
};

/* When dynamically changing stream3 or stream4 source
 * we need to update asrc settings for that stream */
static int cx2070x_input_streams_update_rate(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol,
					     unsigned int cx_stream_reg)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct cx2070x_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;
	u8 dai_id = ucontrol->value.integer.value[0];
	if(dai_id == 0 || dai_id == 1) {
		dev_info(component->dev, "stream%u changes source to dai_id:%u\n",
			 cx_stream_reg == CXREG_STREAM3_RATE ? 3 : 4, dai_id);

		ret = snd_soc_component_update_bits(component, cx_stream_reg,
						    CX_RATE_MASK, priv->cached_rate[dai_id]);
		if(ret < 0)
			return ret;
	}

	return snd_soc_dapm_put_enum_double(kcontrol, ucontrol);
}

static int cx2070x_stream3_update_rate(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	return cx2070x_input_streams_update_rate(kcontrol, ucontrol, CXREG_STREAM3_RATE);
}
static int cx2070x_stream4_update_rate(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	return cx2070x_input_streams_update_rate(kcontrol, ucontrol, CXREG_STREAM4_RATE);
}

static const char *const stream3_mux_txt[] =
	{"Digital 1", "Digital 2", "No input", "USB TX2", "SPDIF"};
static const struct soc_enum stream3_mux_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_STREAM3_ROUTE, 3, stream3_mux_txt);
static const struct snd_kcontrol_new stream3_mux =
	SOC_DAPM_ENUM_EXT("Stream 3 Mux", stream3_mux_enum,
			  snd_soc_dapm_get_enum_double, cx2070x_stream3_update_rate);

static const char *const stream4_mux_txt[] =
	{"Digital 1", "Digital 2", "USB"};
static const struct soc_enum stream4_mux_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_STREAM4_ROUTE, 3, stream4_mux_txt);
static const struct snd_kcontrol_new stream4_mux =
	SOC_DAPM_ENUM_EXT("Stream 4 Mux", stream4_mux_enum,
			  snd_soc_dapm_get_enum_double, cx2070x_stream4_update_rate);

static const char *diff_control_text[] = {"Single Ended", "Differential"};
static const struct soc_enum line1_diff_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_INPUT_CONTROL, CX_INPUT_LINE1_DIFF_SHIFT, diff_control_text);
static const struct soc_enum lineout_diff_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_OUTPUT_CONTROL, CX_OUTPUT_LO_DIFF_SHIFT, diff_control_text);
static const struct soc_enum mono_diff_enum =
	SOC_ENUM_SINGLE_ARRAY(CXREG_OUTPUT_CONTROL, CX_OUTPUT_MONO_DIFF_SHIFT, diff_control_text);

static const char *const dsp_input_mux_txt[] = {
	"None", "Stream 1", "Stream 2", "Stream 3", "Stream 4", "Scale Out",
	"Voice Out", "Voice Out Debug", "Function Gen", "Mixer1 Out"
};

#define CX2070X_DSP_INPUT_ENUM(_wname, _reg , _mux_enum)       \
	static const struct soc_enum _reg##dsp_input_mux_enum =    \
		SOC_ENUM_SINGLE_ARRAY(_reg, 0, dsp_input_mux_txt);     \
										    \
	static const struct snd_kcontrol_new _mux_enum =           \
		SOC_DAPM_ENUM(_wname, _reg##dsp_input_mux_enum);       \

CX2070X_DSP_INPUT_ENUM("Mix0Input 0 Mux", CXREG_MIX0IN0_SOURCE, mix0in0_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix0Input 1 Mux", CXREG_MIX0IN1_SOURCE, mix0in1_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix0Input 2 Mux", CXREG_MIX0IN2_SOURCE, mix0in2_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix0Input 3 Mux", CXREG_MIX0IN3_SOURCE, mix0in3_input_mux)

CX2070X_DSP_INPUT_ENUM("Mix1Input 0 Mux", CXREG_MIX1IN0_SOURCE, mix1in0_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix1Input 1 Mux", CXREG_MIX1IN1_SOURCE, mix1in1_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix1Input 2 Mux", CXREG_MIX1IN2_SOURCE, mix1in2_input_mux)
CX2070X_DSP_INPUT_ENUM("Mix1Input 3 Mux", CXREG_MIX1IN3_SOURCE, mix1in3_input_mux)

CX2070X_DSP_INPUT_ENUM("VoiceIn Mux", CXREG_VOICEIN0_SOURCE, voiice_input_mux)

CX2070X_DSP_INPUT_ENUM("Stream 5 Mux", CXREG_I2S1OUTIN_SOURCE, stream5_input_mux)
CX2070X_DSP_INPUT_ENUM("Stream 6 Mux", CXREG_I2S2OUTIN_SOURCE, stream6_input_mux)
CX2070X_DSP_INPUT_ENUM("Stream 7 Mux", CXREG_DACIN_SOURCE, stream7_input_mux)
CX2070X_DSP_INPUT_ENUM("Stream 8 Mux", CXREG_DACSUBIN_SOURCE, stream8_input_mux)
CX2070X_DSP_INPUT_ENUM("Stream 9 Mux", CXREG_USBOUT_SOURCE, stream9_input_mux)

static const struct snd_soc_dapm_widget cx2070x_dapm_widgets[]= {
	/* External interfaces */
	SND_SOC_DAPM_AIF_IN("DP1IN", PLAYBACK_STREAM_NAME_1, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("DP2IN", PLAYBACK_STREAM_NAME_2, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("USBTX2IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("SPDIFIN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("USBIN", NULL, 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_AIF_OUT("SPDIFOUT", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("USBOUT",   NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("DP1OUT",   CAPTURE_STREAM_NAME_1, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("DP2OUT",   CAPTURE_STREAM_NAME_2, 0, SND_SOC_NOPM, 0, 0),

	/* Left codec side: inputs */
	SND_SOC_DAPM_INPUT("LINEIN1"),
	SND_SOC_DAPM_INPUT("LINEIN2"),
	SND_SOC_DAPM_INPUT("LINEIN3"),
	SND_SOC_DAPM_SWITCH("Line 1 Capture", SND_SOC_NOPM, 0, 0, &lines_in_volumes[0]),
	SND_SOC_DAPM_SWITCH("Line 2 Capture", SND_SOC_NOPM, 0, 0, &lines_in_volumes[1]),
	SND_SOC_DAPM_SWITCH("Line 3 Capture", SND_SOC_NOPM, 0, 0, &lines_in_volumes[2]),
	SOC_MIXER_NAMED_CTL_ARRAY("Stream 1 Mixer", CXREG_DSP_INIT_NEWC, 1, 0, strm1_sel_mix),
	SND_SOC_DAPM_ADC("Stream 1 ADC", NULL, SND_SOC_NOPM, 0, 0),

	/* On some cx2070x codecs there should be a second microphone
	 * possibly connected to the right channel of ADC2 */
	SND_SOC_DAPM_INPUT("MICIN"),
	SOC_MIXER_ARRAY("Mic", SND_SOC_NOPM, 0, 0, left_mic_controls),
	SND_SOC_DAPM_MIXER("Stream 2 Mixer", CXREG_DSP_INIT_NEWC, 2, 0, 0, 0),
	SND_SOC_DAPM_ADC("Stream 2 ADC", NULL, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MUX("Stream 3 Mux", CXREG_DSP_INIT_NEWC, 3, 0, &stream3_mux),
	SND_SOC_DAPM_MUX("Stream 4 Mux", CXREG_DSP_INIT_NEWC, 4, 0, &stream4_mux),

	SND_SOC_DAPM_SIGGEN("TONE"),
	SND_SOC_DAPM_SWITCH("Function Generator", SND_SOC_NOPM, 0, 0, &function_gen_switch),

	/* Central codec side: Playback DSP, Voice DSP and simple mixer */
	SND_SOC_DAPM_MUX("Mix0Input 0 Mux", SND_SOC_NOPM, 0, 0, &mix0in0_input_mux),
	SND_SOC_DAPM_MUX("Mix0Input 1 Mux", SND_SOC_NOPM, 0, 0, &mix0in1_input_mux),
	SND_SOC_DAPM_MUX("Mix0Input 2 Mux", SND_SOC_NOPM, 0, 0, &mix0in2_input_mux),
	SND_SOC_DAPM_MUX("Mix0Input 3 Mux", SND_SOC_NOPM, 0, 0, &mix0in3_input_mux),
	SOC_MIXER_ARRAY("Mixer 0 Mixer", SND_SOC_NOPM, 0, 0, mixer0_controls),
	SND_SOC_DAPM_PGA("Playback DSP", SND_SOC_NOPM, 0, 0, 0, 0),
	SND_SOC_DAPM_PGA("Playback DSP Out", SND_SOC_NOPM, 0, 0, 0, 0),

	SND_SOC_DAPM_MUX("Mix1Input 0 Mux", SND_SOC_NOPM, 0, 0, &mix1in0_input_mux),
	SND_SOC_DAPM_MUX("Mix1Input 1 Mux", SND_SOC_NOPM, 0, 0, &mix1in1_input_mux),
	SND_SOC_DAPM_MUX("Mix1Input 2 Mux", SND_SOC_NOPM, 0, 0, &mix1in2_input_mux),
	SND_SOC_DAPM_MUX("Mix1Input 3 Mux", SND_SOC_NOPM, 0, 0, &mix1in3_input_mux),
	SOC_MIXER_ARRAY("Mixer 1 Mixer", SND_SOC_NOPM, 0, 0, mixer1_controls),

	SND_SOC_DAPM_MUX("VoiceIn Mux", SND_SOC_NOPM, 0, 0, &voiice_input_mux),
	SND_SOC_DAPM_PGA("Voice DSP", SND_SOC_NOPM, 0, 0, 0, 0),
	SND_SOC_DAPM_PGA("Voice DSP Out", SND_SOC_NOPM, 0, 0, 0, 0),

	/* Right codec side: outputs */
	SND_SOC_DAPM_MUX("Stream 5 Mux", CXREG_DSP_INIT_NEWC, 5, 0, &stream5_input_mux),
	SND_SOC_DAPM_MUX("Stream 6 Mux", CXREG_DSP_INIT_NEWC, 6, 0, &stream6_input_mux),

	/* Stream 7 */
	SND_SOC_DAPM_MUX("Stream 7 Mux", CXREG_DSP_INIT_NEWC, 7, 0, &stream7_input_mux),

	SND_SOC_DAPM_DAC("Left DAC1", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC("Right DAC2", NULL, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_PGA("Left DAC1 PGA", SND_SOC_NOPM, 0, 0, 0, 0),
	SND_SOC_DAPM_PGA("Right DAC2 PGA", SND_SOC_NOPM, 0, 0, 0, 0),

	SND_SOC_DAPM_SWITCH("Headphone", SND_SOC_NOPM, 0, 0, &hp_switch),
	SND_SOC_DAPM_SWITCH("Line Out",  SND_SOC_NOPM, 0, 0, &lineout_switch),
	SND_SOC_DAPM_SUPPLY("Class D", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("Speaker",   SND_SOC_NOPM, 0, 0, &classd_switch),

	/* Perhaps there should be a switch to spdif out ? */
	SND_SOC_DAPM_PGA("SPDIF Out", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Stream 8 */
	SND_SOC_DAPM_MUX("Stream 8 Mux", CXREG_DSP_INIT, 0, 0, &stream8_input_mux),
	SND_SOC_DAPM_DAC("Mono DAC3", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_PGA("Mono Output", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Stream 9 */
	SND_SOC_DAPM_MUX("Stream 9 Mux", SND_SOC_NOPM, 0, 0, &stream9_input_mux),
	SND_SOC_DAPM_PGA("USB Out", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Outputs */
	SND_SOC_DAPM_OUTPUT("HPOUTL"),
	SND_SOC_DAPM_OUTPUT("HPOUTR"),

	SND_SOC_DAPM_OUTPUT("SPKOUTL"),
	SND_SOC_DAPM_OUTPUT("SPKOUTR"),

	SND_SOC_DAPM_OUTPUT("LINEOUTL"),
	SND_SOC_DAPM_OUTPUT("LINEOUTR"),

	SND_SOC_DAPM_OUTPUT("MONOOUT"),
};

#define CX2070X_DSP_MUX_ROUTES(widget)  \
	{widget, "Stream 1", "Stream 1 ADC"}, \
	{widget, "Stream 2", "Stream 2 ADC"}, \
	{widget, "Stream 3", "Stream 3 Mux"}, \
	{widget, "Stream 4", "Stream 4 Mux"}, \
	{widget, "Function Gen","Function Generator"}

#define CX2070X_OUTPUT_SOURCE_MUX_ROUTES(_wname)	\
	{_wname, "Stream 1", "Stream 1 ADC"},			\
	{_wname, "Stream 2", "Stream 2 ADC"},			\
	{_wname, "Stream 3", "Stream 3 Mux"},			\
	{_wname, "Stream 4", "Stream 4 Mux"},			\
	{_wname, "Scale Out", "Playback DSP Out"},				\
	{_wname, "Voice Out", "Voice DSP Out"},			\
	{_wname, "Voice Out Debug", "Voice DSP Out"},			\
	{_wname, "Function Gen", "Function Generator"},	\
	{_wname, "Mixer1 Out", "Mixer 1 Mixer"}

static const struct snd_soc_dapm_route cx2070x_dapm_routes[] = {
	/* stream 1 */
	{"Line 1 Capture", "Volume", "LINEIN1"},
	{"Line 1 Capture", NULL, "LINEIN1"},
	{"Line 2 Capture", "Volume", "LINEIN2"},
	{"Line 2 Capture", NULL, "LINEIN2"},
	{"Line 3 Capture", "Volume", "LINEIN3"},
	{"Line 3 Capture", NULL, "LINEIN3"},

	{"Stream 1 Mixer", "Line 1 Capture Switch", "Line 1 Capture"},
	{"Stream 1 Mixer", "Line 2 Capture Switch", "Line 2 Capture"},
	{"Stream 1 Mixer", "Line 3 Capture Switch", "Line 3 Capture"},
	{"Stream 1 ADC", NULL, "Stream 1 Mixer"},

	/* stream 2 */
	{"Mic", "Capture Volume", "MICIN"},
	{"Mic", "Capture Switch", "MICIN"},
	{"Stream 2 Mixer", NULL, "Mic"},
	{"Stream 2 ADC", NULL, "Stream 2 Mixer"},

	/* stream 3 */
	{"Stream 3 Mux", "Digital 1", "DP1IN"},
	{"Stream 3 Mux", "Digital 2", "DP2IN"},
	{"Stream 3 Mux", "USB TX2", "USBTX2IN"},
	{"Stream 3 Mux", "SPDIF", "SPDIFIN"},

	/* stream 4 */
	{"Stream 4 Mux", "Digital 1", "DP1IN"},
	{"Stream 4 Mux", "Digital 2", "DP2IN"},
	{"Stream 4 Mux", "USB", "USBIN"},

	/* Function Generator */
	{"Function Generator", "Switch", "TONE"},

	/* Mixer 0 + Playback DSP */
	CX2070X_DSP_MUX_ROUTES("Mix0Input 0 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix0Input 1 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix0Input 2 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix0Input 3 Mux"),
	{"Mixer 0 Mixer", "Input 0 Volume", "Mix0Input 0 Mux"},
	{"Mixer 0 Mixer", "Input 1 Volume", "Mix0Input 1 Mux"},
	{"Mixer 0 Mixer", "Input 2 Volume", "Mix0Input 2 Mux"},
	{"Mixer 0 Mixer", "Input 3 Volume", "Mix0Input 3 Mux"},
	{"Playback DSP", NULL, "Mixer 0 Mixer"},
	{"Playback DSP Out", NULL, "Playback DSP"},

	/* Mixer 1 - Simple Mixer */
	CX2070X_DSP_MUX_ROUTES("Mix1Input 0 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix1Input 1 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix1Input 2 Mux"),
	CX2070X_DSP_MUX_ROUTES("Mix1Input 3 Mux"),
	{"Mixer 1 Mixer", "Input 0 Volume", "Mix1Input 0 Mux"},
	{"Mixer 1 Mixer", "Input 1 Volume", "Mix1Input 1 Mux"},
	{"Mixer 1 Mixer", "Input 2 Volume", "Mix1Input 2 Mux"},
	{"Mixer 1 Mixer", "Input 3 Volume", "Mix1Input 3 Mux"},

	/* Voice Processing */
	CX2070X_DSP_MUX_ROUTES("VoiceIn Mux"),
	{"Voice DSP", NULL, "VoiceIn Mux"},
	{"Voice DSP Out", NULL, "Voice DSP"},

	/* Stream 5 */
	CX2070X_OUTPUT_SOURCE_MUX_ROUTES("Stream 5 Mux"),
	{"DP1OUT", NULL, "Stream 5 Mux"},

	/* Stream 6 */
	CX2070X_OUTPUT_SOURCE_MUX_ROUTES("Stream 6 Mux"),
	{"DP2OUT", NULL, "Stream 6 Mux"},

	/* Stream 7 */
	CX2070X_OUTPUT_SOURCE_MUX_ROUTES("Stream 7 Mux"),
	/* Perhaps there should be a switch
	 * between spdif out and other outputs? */
	{"SPDIF Out", NULL, "Stream 7 Mux"},

	{"Left DAC1", NULL, "Stream 7 Mux"},
	{"Right DAC2", NULL, "Stream 7 Mux"},

	{"Left DAC1 PGA",  NULL, "Left DAC1"},
	{"Right DAC2 PGA", NULL, "Right DAC2"},

	{"Headphone", "Switch", "Left DAC1 PGA"},
	{"Headphone", "Switch", "Right DAC2 PGA"},
	{"HPOUTL", NULL, "Headphone"},
	{"HPOUTR", NULL, "Headphone"},

	{"Speaker", "Switch", "Left DAC1 PGA"},
	{"Speaker", "Switch", "Right DAC2 PGA"},
	{"SPKOUTL", NULL, "Speaker"},
	{"SPKOUTL", NULL, "Class D"},
	{"SPKOUTR", NULL, "Speaker"},
	{"SPKOUTR", NULL, "Class D"},

	{"Line Out", "Switch", "Left DAC1 PGA"},
	{"Line Out", "Switch", "Right DAC2 PGA"},
	{"LINEOUTL", NULL, "Line Out"},
	{"LINEOUTR", NULL, "Line Out"},

	/* Stream 8 */
	CX2070X_OUTPUT_SOURCE_MUX_ROUTES("Stream 8 Mux"),
	{"Mono DAC3", NULL, "Stream 8 Mux"},
	{"Mono Output", NULL, "Mono DAC3"},
	{"MONOOUT", NULL, "Mono Output"},

	/* Stream 9 */
	CX2070X_OUTPUT_SOURCE_MUX_ROUTES("Stream 9 Mux"),
	{"USB Out", NULL, "Stream 9 Mux"},
	{"USBOUT", NULL, "USB Out"},
};

/* This is fanvil c600 specific thing, it switching microphone source */
static const char *mic_route_texts[] = {"Handset", "Hands Free", "Headset"};
static const struct soc_enum mic_route_enum =
    SOC_ENUM_SINGLE_ARRAY(CXREG_GPIO_CONTROL, 2, mic_route_texts);

/* When changing codec settings we should apply configuration.
 * these macros are the same as SOC_SINGLE/SOC_DOUBLE, but always calls dsp_put for write */
#define CX_SOC_SINGLE(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
    .info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
    .put = dsp_put, \
    .private_value = SOC_SINGLE_VALUE(reg, shift, max, invert, 0) }

#define CX_SOC_DOUBLE(xname, reg, shift_left, shift_right, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
    .info = snd_soc_info_volsw, .get = snd_soc_get_volsw, \
    .put = dsp_put, \
    .private_value = SOC_DOUBLE_VALUE(reg, shift_left, shift_right, \
		      max, invert, 0) }

static const struct snd_kcontrol_new cx2070x_snd_controls[] = {
	/* TODO: on some cx2070x variants a microphone can be attached to Line 3
	 * TODO: on some cx2070x variants there also digital microhone input available
	 * for stream 1 and stream 2 through mux, which is also conflicts with second i2s port */

	/* Volume controls for all three input Lines.
	 */
	SOC_DOUBLE_R_S_TLV("Line Capture Volume",
			   CXREG_ADC1L_GAIN, CXREG_ADC1R_GAIN, 0, -74, 5, 7, 0, main_tlv),
	CX_SOC_DOUBLE("Line Capture Switch",
		      CXREG_VOLUME_MUTE, CX_MUTE_AUX_R_SHIFT, CX_MUTE_AUX_L_SHIFT, 1, 1),

	SOC_DOUBLE_R_S_TLV("Stream 2 ADC Capture Volume",
			   CXREG_ADC2L_GAIN, CXREG_ADC2R_GAIN, 0, -74, 5, 7, 0, main_tlv),

	/* cx2070x has a single dual channel ADC which
	 * controls playback volume for Line Out, Speaker and Headphone at the same time */
	SOC_DOUBLE_R_S_TLV("Master Playback Volume",
			   CXREG_DAC1_GAIN, CXREG_DAC2_GAIN, 0, -74, 5, 7, 0, main_tlv),
	CX_SOC_DOUBLE("Master Playback Switch", CXREG_VOLUME_MUTE,
		      CX_MUTE_SPEAKER_R_SHIFT, CX_MUTE_SPEAKER_L_SHIFT, 1, 1),

	SOC_SINGLE_S8_TLV("Mono Output Playback Volume", CXREG_DAC3_GAIN, -74, 5, main_tlv),
	CX_SOC_SINGLE("Mono Output Playback Switch",
		      CXREG_VOLUME_MUTE, CX_MUTE_MONO_SHIFT, 1, 1),

	SOC_ENUM("Mic Bias Voltage", mic_bias_enum),

	CX_SOC_SINGLE("Sidetone Switch", CXREG_DSP_ENABLE,			7, 1, 0),
	/* What is the Right mic ? */
	CX_SOC_SINGLE("Use Right Mic Switch", CXREG_DSP_ENABLE,			6, 1, 0),
	CX_SOC_SINGLE("Playback Noise Reduction Switch", CXREG_DSP_ENABLE,	5, 1, 0),
	CX_SOC_SINGLE("Mic AGC Switch", CXREG_DSP_ENABLE,			4, 1, 0),
	CX_SOC_SINGLE("Beam forming Switch", CXREG_DSP_ENABLE,			3, 1, 0),
	CX_SOC_SINGLE("Capture Noise Reduction Switch", CXREG_DSP_ENABLE,	2, 1, 0),
	CX_SOC_SINGLE("LEC Switch", CXREG_DSP_ENABLE,				1, 1, 0),
	CX_SOC_SINGLE("AEC Switch", CXREG_DSP_ENABLE,				0, 1, 0),

	CX_SOC_SINGLE("Stream 7 Mono Enable Switch", CXREG_DSP_ENABLE2,		6, 1, 0),
	CX_SOC_SINGLE("LA Switch", CXREG_DSP_ENABLE2,				4, 1, 0),
	CX_SOC_SINGLE("3D Switch", CXREG_DSP_ENABLE2,				3, 1, 0),
	CX_SOC_SINGLE("DRC Switch", CXREG_DSP_ENABLE2,				2, 1, 0),
	CX_SOC_SINGLE("Subwoofer Switch", CXREG_DSP_ENABLE2,			1, 1, 0),
	CX_SOC_SINGLE("Equalizer Switch", CXREG_DSP_ENABLE2,			0, 1, 0),

	SOC_ENUM("Class D Gain", classd_gain_enum),

	CX_SOC_SINGLE("Auto Output Switch", CXREG_OUTPUT_CONTROL, CX_OUTPUT_AUTO_SHIFT, 1, 0),
	SOC_ENUM("Mono Output Mode", mono_diff_enum),
	SOC_ENUM("Line Out Mode", lineout_diff_enum),
	CX_SOC_SINGLE("Class D PWM Switch", CXREG_OUTPUT_CONTROL, CX_OUTPUT_CLASSD_PWM_SHIFT, 1, 0),
	CX_SOC_SINGLE("Class D Mono Switch", CXREG_OUTPUT_CONTROL,CX_OUTPUT_CLASSD_MONO_SHIFT, 1, 0),

	CX_SOC_SINGLE("Auto Input Switch", CXREG_INPUT_CONTROL, CX_INPUT_AUTO_SHIFT, 1, 0),
	SOC_ENUM("Line 1 Mode", line1_diff_enum),

	/* This is fanvil C600 specific */
	/* handset mic en: CXREG_GPIO_CONTROL & 0xf3 : bit(2) and bit(3) are 0
	 * headphone mic: CXREG_GPIO_CONTROL & 0xf3 | 8 : BIT(3) set
	 * handsfree_mic: CXREG_GPIO_CONTROL & 0xf3 | 4 : BIT(2) */
	SOC_ENUM("Fanvil Mic Select", mic_route_enum),
};

static int cx2070x_stream_event(struct snd_soc_component *component, int event) {
	unsigned int ret;
	/*
	 * There are many registers on cx2070x codec family, some are
	 * require configuration update, some are not.
	 * But since I do not have a datasheet let's always
	 * apply configuration when something changes in the stream
	 */
	ret = apply_dsp_change(component);
	if(ret < 0)
		return ret;
	return 0;
}

static const struct snd_soc_component_driver cx2070x_driver = {
	.controls               = cx2070x_snd_controls,
	.num_controls           = ARRAY_SIZE(cx2070x_snd_controls),
	.dapm_widgets           = cx2070x_dapm_widgets,
	.num_dapm_widgets       = ARRAY_SIZE(cx2070x_dapm_widgets),
	.dapm_routes            = cx2070x_dapm_routes,
	.num_dapm_routes        = ARRAY_SIZE(cx2070x_dapm_routes),
	.stream_event           = cx2070x_stream_event,

	.endianness             = 1,
	.non_legacy_dai_naming  = 1,
};

static int cx2070x_wait_for_device(struct cx2070x_priv *priv) {
	int i;
	int ret;
	unsigned int val;

	for(i = 0; i < 100; i++) {
		/* We should read ABORT_CODE until 0x1 is returned */
		ret = regmap_read(priv->regmap, CXREG_ABCODE, &val);
		if(val == 0x01 && !ret)
			return 0;

		usleep_range(1000, 2000);
	}
	return -EINVAL;
}

static int cx2070x_reset(struct cx2070x_priv *priv, struct gpio_desc *reset) {
	int ret;
	gpiod_direction_output(reset, 1);
	gpiod_direction_output(reset, 0);
	mdelay(10);
	gpiod_set_value(reset, 1);

	ret = cx2070x_wait_for_device(priv);
	if(ret)
		return ret;

	/* We have to wait for chip reset here, but i don't know for how long =\ */
	msleep(200);
	return ret;
}

struct srecord {
    /* S record type */
    u8 type;

    /* decoded addr and its length in bytes */
    u8 addr[4];
    int addr_len;

    /* decoded data part and its length in bytes */
    u8 data[256];
    u8 data_len;

    /* length of address + data + checksum */
    u8 srec_len;

    /* total size */
    int size;
};

/*
 * Will parse binary and decode srecord data in provided struct *srecord
 */

static int decode_srecord(const u8 *binary, struct srecord *srecord, int size) {
    int ret;

    if(binary[0] != 'S')
        return -EINVAL;

    /* Extract S-Type */
    srecord->type = hex_to_bin(binary[1]);

    /* Only S3 and S7 Types are important */
    switch(srecord->type){
    case 3:
        srecord->addr_len = srecord->type + 1;
        break;
    case 7:
        srecord->addr_len = 11 - srecord->type;
        break;
    default:
        return -EINVAL;
    }

    /* Extract address + data + checksum length */
    ret = hex2bin(&srecord->srec_len, binary + 2, 1);
    if(ret || !srecord->srec_len)
        return -EINVAL;

    /* Check for line end */
    if(binary[4 + srecord->srec_len * 2] != 0x0d || binary[5 + srecord->srec_len * 2] != 0x0a)
        return -EINVAL;

    /* Data part length */
    srecord->data_len = srecord->srec_len - srecord->addr_len - 1;

    /* Extract address */
    ret = hex2bin(srecord->addr, binary + 4, srecord->addr_len);
    if(ret)
        return -EINVAL;

    /* Extract data */
    ret = hex2bin(srecord->data, binary + 4 + (srecord->addr_len * 2), srecord->data_len);
    if(ret)
        return -EINVAL;

    srecord->size = 6 + (srecord->srec_len * 2);
    return 0;
}

static int cx2070x_read_firmware_version(struct cx2070x_priv *priv) {
    int ret;
    u8 fw_version[6];
    u8 patch_info[3];
    /* Read registers 0x1001-0x1006 */
    ret = regmap_bulk_read(priv->regmap, CXREG_FIRMWARE_VER_LO, fw_version, ARRAY_SIZE(fw_version));
    if(ret)
	return ret;

    /* Read registers 0x1584-0x1586 */
    ret = regmap_bulk_read(priv->regmap, CXREG_PATCH_HI, patch_info, ARRAY_SIZE(patch_info));
    if(ret)
	return ret;

    dev_info(priv->dev, "Conexant CX2070%u codec. "
			"Firmware version:%u.%02u, "
			"Patch Version:%u.%u, "
			"Release type:%u\n",
			fw_version[4],
			fw_version[1], fw_version[0],
			fw_version[3], fw_version[2],
			fw_version[5]);

    dev_info(priv->dev, "Firmware patch version:%u.%u.%u\n",
	     patch_info[2], patch_info[1], patch_info[0]);

    return ret;
}

static int download_firmware(struct cx2070x_priv *priv) {
    int ret;
    struct device *dev = priv->dev;
    const struct firmware *firmware_p;
    int pos = 0;
    struct srecord srec = {0};
    u32 reg;

    int ramloader_loaded = 0;
    u8 patch_data[100];
    u32 val;

#ifdef DEBUG
    int count = 1;
    char addr_buf[20];
    char data_buf[200];
#endif

    ret = request_firmware(&firmware_p, "cx2070x.srec", dev);
    if(ret) {
        dev_err(dev, "Failed to load firmware\n");
        return ret;
    }

    while(pos < firmware_p->size) {
        ret = decode_srecord(firmware_p->data + pos, &srec, firmware_p->size - pos);
        if(ret)
            goto release_firmware;

        pos += srec.size;
        reg = be32_to_cpup((__be32 *)(srec.addr));

#ifdef DEBUG

        bin2hex(addr_buf, srec.addr, srec.addr_len);
        addr_buf[srec.addr_len * 2] = '\0';

        bin2hex(data_buf, srec.data, srec.data_len);
        data_buf[srec.data_len * 2] = '\0';

        dev_err(dev, "Srecord %d: type:%d, addr:%s, reg:0x%x, addr_len:%d, data:%s, data_len:%d, srec_len:%d size:%d\n",
                count, srec.type,
                addr_buf, reg, srec.addr_len,
                data_buf, srec.data_len,
                srec.srec_len, srec.size);

        ++count;
#endif

        /*
         * All S3 record types until first S7 is ram loader. When we receive first S7 we
         * should read ABORT_CODE And Ignore all other S7s
         *
         * Then, write everything until address is less than 0x80000000
         * when regs become >=0x8000000 start writing patch data
         */

        if(srec.type == 7 && !ramloader_loaded) {
            ret = cx2070x_wait_for_device(priv);
            if(ret)
                goto release_firmware;
            ramloader_loaded = 1;
        }

        if(srec.type == 3) {
            if(reg < 0x80000000) {
                ret = regmap_bulk_write(priv->regmap, reg, srec.data, srec.data_len);
                if(ret)
                    goto release_firmware;
            } else {
                patch_data[0] = srec.addr[3];
                patch_data[1] = srec.addr[2];
                patch_data[2] = srec.addr[1];
                patch_data[3] = srec.data_len - 1;
                memcpy(patch_data + 4, srec.data, srec.data_len);
#ifdef DEBUG
                bin2hex(data_buf, patch_data, srec.data_len + 4);
                data_buf[(srec.data_len + 4) * 2] = '\0';

                dev_err(dev, "^^^^ Magic! reg: 0x%x, data %s \n", 0x02fc, data_buf);
#endif
                ret = regmap_bulk_write(priv->regmap, 0x02fc, patch_data, srec.data_len + 4);
                if(ret)
                    goto release_firmware;

                ret = regmap_write(priv->regmap, 0x0400, 0x85);
                if(ret)
                    goto release_firmware;

                ret = regmap_read(priv->regmap, 0x0400, &val);
                if(ret)
                    goto release_firmware;

                if(val != 0x05) {
                    ret = -ENODEV;
                    goto release_firmware;
                }
            }
        }
    }

    ret = regmap_write(priv->regmap, CXREG_ABCODE, 0);
    if(ret)
        goto release_firmware;

    ret = regmap_read(priv->regmap, CXREG_ABCODE, &val);
    if(ret)
        goto release_firmware;

    if(val != 0x01) {
        ret = -ENODEV;
        goto release_firmware;
    }

    dev_err(dev, "Firmware Loaded\n");

    return cx2070x_read_firmware_version(priv);

release_firmware:
    release_firmware(firmware_p);
    return ret;
}

int cx2070x_probe(struct device *dev, struct regmap *regmap)
{
	struct cx2070x_priv *priv;
	struct gpio_desc *reset;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if(!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);

	priv->regmap = regmap;
	priv->dev = dev;
	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if(!reset) {
		dev_err(dev, "Failed to obtain reset gpio\n");
		return -EINVAL;
	}

	ret = cx2070x_reset(priv, reset);
	devm_gpiod_put(dev, reset);
	if(ret)
		return ret;

	ret = cx2070x_read_firmware_version(priv);
	if(ret)
		return ret;

	/* Without this we can not change gpio register bits 2 and 3
	 * for mic route selection on fanvil c600.
	 * Maybe this switches gpio direction to input from output?
	 * Bit(5) controls Bit(2)
	 * Bit(6) control  Bit(3)
	 * also Bit(7) controls Bit(4), looks like it uses handset amplifier? */
	regmap_update_bits(regmap, CXREG_GPIO_CONTROL,
			   BIT(7) | BIT(6) | BIT(5), 0);

	//    ret = download_firmware(priv);
	//    if(ret)
	//        return ret;

	/* cx20707 has only single i2s/pcm interface */
	ret = devm_snd_soc_register_component(dev, &cx2070x_driver, cx2070x_dais, ARRAY_SIZE(cx2070x_dais));
	if(ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(cx2070x_probe);

MODULE_DESCRIPTION("Conexant cx2070x codec driver");
MODULE_AUTHOR("Aleksandrov Stanislav <lightofmysoul@gmail.com>");
MODULE_LICENSE("GPL v2");




