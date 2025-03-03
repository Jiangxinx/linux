// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2019 Intel Corporation.

/*
 * Intel SOF Machine Driver with Realtek rt5682 Codec
 */
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/rt5682.h>
#include <sound/soc-acpi.h>
#include "../../codecs/rt5682.h"
#include "../../codecs/hdac_hdmi.h"

#define DUAL_CHANNEL 2
#define QUAD_CHANNEL 4
#define NAME_SIZE 32

#define SOF_RT5682_SSP(quirk)		((quirk) & GENMASK(2, 0))
#define SOF_RT5682_SSP_MASK		(GENMASK(2, 0))
#define SOF_RT5682_MCLK_EN		BIT(3)
#define SOF_RT5682_MCLK_24MHZ		BIT(4)

/* Default: MCLK on, MCLK 19.2M, SSP0  */
static unsigned long sof_rt5682_quirk = SOF_RT5682_MCLK_EN |
					SOF_RT5682_SSP(0);

static int is_legacy_cpu;

static struct snd_soc_jack sof_hdmi[3];

struct sof_hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

struct sof_card_private {
	struct snd_soc_jack sof_headset;
	struct list_head hdmi_pcm_list;
};

static int sof_rt5682_quirk_cb(const struct dmi_system_id *id)
{
	sof_rt5682_quirk = (unsigned long)id->driver_data;
	return 1;
}

static const struct dmi_system_id sof_rt5682_quirk_table[] = {
	{
		.callback = sof_rt5682_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "WhiskeyLake Client"),
		},
		.driver_data = (void *)(SOF_RT5682_MCLK_EN |
					SOF_RT5682_MCLK_24MHZ |
					SOF_RT5682_SSP(1)),
	},
	{
		.callback = sof_rt5682_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Hatch"),
		},
		.driver_data = (void *)(SOF_RT5682_MCLK_EN |
					SOF_RT5682_MCLK_24MHZ |
					SOF_RT5682_SSP(0)),
	},
	{
		.callback = sof_rt5682_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Ice Lake Client"),
		},
		.driver_data = (void *)(SOF_RT5682_MCLK_EN |
					SOF_RT5682_SSP(0)),
	},
	{}
};

static int sof_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct sof_hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	/* dai_link id is 1:1 mapped to the PCM device */
	pcm->device = rtd->dai_link->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

static int sof_rt5682_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component = rtd->codec_dai->component;
	struct snd_soc_jack *jack;
	int ret;

	/* need to enable ASRC function for 24MHz mclk rate */
	if ((sof_rt5682_quirk & SOF_RT5682_MCLK_EN) &&
	    (sof_rt5682_quirk & SOF_RT5682_MCLK_24MHZ)) {
		rt5682_sel_asrc_clk_src(component, RT5682_DA_STEREO1_FILTER,
					RT5682_CLK_SEL_I2S1_ASRC);
	}

	/*
	 * Headset buttons map to the google Reference headset.
	 * These can be configured by userspace.
	 */
	ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
				    SND_JACK_HEADSET | SND_JACK_BTN_0 |
				    SND_JACK_BTN_1 | SND_JACK_BTN_2 |
				    SND_JACK_BTN_3,
				    &ctx->sof_headset, NULL, 0);
	if (ret) {
		dev_err(rtd->dev, "Headset Jack creation failed: %d\n", ret);
		return ret;
	}

	jack = &ctx->sof_headset;

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);
	ret = snd_soc_component_set_jack(component, jack, NULL);

	if (ret) {
		dev_err(rtd->dev, "Headset Jack call-back failed: %d\n", ret);
		return ret;
	}

	return ret;
};

static int sof_rt5682_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int clk_id, clk_freq, pll_out, ret;

	if (sof_rt5682_quirk & SOF_RT5682_MCLK_EN) {
		clk_id = RT5682_PLL1_S_MCLK;
		if (sof_rt5682_quirk & SOF_RT5682_MCLK_24MHZ)
			clk_freq = 24000000;
		else
			clk_freq = 19200000;
	} else {
		clk_id = RT5682_PLL1_S_BCLK1;
		clk_freq = params_rate(params) * 50;
	}

	pll_out = params_rate(params) * 512;

	ret = snd_soc_dai_set_pll(codec_dai, 0, clk_id, clk_freq, pll_out);
	if (ret < 0)
		dev_err(rtd->dev, "snd_soc_dai_set_pll err = %d\n", ret);

	/* Configure sysclk for codec */
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5682_SCLK_S_PLL1,
				     pll_out, SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(rtd->dev, "snd_soc_dai_set_sysclk err = %d\n", ret);

	/*
	 * slot_width should equal or large than data length, set them
	 * be the same
	 */
	ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x0, 0x0, 2,
				       params_width(params));
	if (ret < 0) {
		dev_err(rtd->dev, "set TDM slot err:%d\n", ret);
		return ret;
	}

	return ret;
}

static struct snd_soc_ops sof_rt5682_ops = {
	.hw_params = sof_rt5682_hw_params,
};

static struct snd_soc_dai_link_component platform_component[] = {
	{
		/* name might be overridden during probe */
		.name = "0000:00:1f.3"
	}
};

static int sof_card_late_probe(struct snd_soc_card *card)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *component = NULL;
	char jack_name[NAME_SIZE];
	struct sof_hdmi_pcm *pcm;
	int err = 0;
	int i = 0;

	/* HDMI is not supported by SOF on Baytrail/CherryTrail */
	if (is_legacy_cpu)
		return 0;

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			 "HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					    SND_JACK_AVOUT, &sof_hdmi[i],
					    NULL, 0);

		if (err)
			return err;

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
					  &sof_hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}

static const struct snd_kcontrol_new sof_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
};

static const struct snd_soc_dapm_widget sof_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route sof_map[] = {
	/* HP jack connectors - unknown if we have jack detection */
	{ "Headphone Jack", NULL, "HPOL" },
	{ "Headphone Jack", NULL, "HPOR" },

	/* other jacks */
	{ "IN1P", NULL, "Headset Mic" },

};

/* sof audio machine driver for rt5682 codec */
static struct snd_soc_card sof_audio_card_rt5682 = {
	.name = "sof_rt5682",
	.owner = THIS_MODULE,
	.controls = sof_controls,
	.num_controls = ARRAY_SIZE(sof_controls),
	.dapm_widgets = sof_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sof_widgets),
	.dapm_routes = sof_map,
	.num_dapm_routes = ARRAY_SIZE(sof_map),
	.fully_routed = true,
	.late_probe = sof_card_late_probe,
};

static const struct x86_cpu_id legacy_cpi_ids[] = {
	{ X86_VENDOR_INTEL, 6, INTEL_FAM6_ATOM_SILVERMONT }, /* Baytrail */
	{ X86_VENDOR_INTEL, 6, INTEL_FAM6_ATOM_AIRMONT }, /* Cherrytrail */
	{}
};

static struct snd_soc_dai_link *sof_card_dai_links_create(struct device *dev,
							  int ssp_port,
							  int dmic_num,
							  int hdmi_num)
{
	struct snd_soc_dai_link *links;
	int i, id = 0;

	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     (1 + dmic_num + hdmi_num), GFP_KERNEL);
	/* SSP */
	links[id].name = devm_kasprintf(dev, GFP_KERNEL,
					"SSP%d-Codec", ssp_port);
	links[id].id = id;
	links[id].codec_name = "i2c-10EC5682:00";
	links[id].codec_dai_name = "rt5682-aif1";
	links[id].platforms = platform_component;
	links[id].num_platforms = ARRAY_SIZE(platform_component);
	links[id].init = sof_rt5682_codec_init;
	links[id].ops = &sof_rt5682_ops;
	links[id].nonatomic = true;
	links[id].dpcm_playback = 1;
	links[id].dpcm_capture = 1;
	links[id].no_pcm = 1;
	if (is_legacy_cpu) {
		links[id].cpu_dai_name = devm_kasprintf(dev, GFP_KERNEL,
							"ssp%d-port", ssp_port);
	} else {
		links[id].cpu_dai_name = devm_kasprintf(dev, GFP_KERNEL,
							"SSP%d Pin", ssp_port);
	}
	id++;

	/* dmic */
	for (i = 1; i <= dmic_num; i++) {
		links[id].name = devm_kasprintf(dev, GFP_KERNEL,
						"dmic%02d", i);
		links[id].id = id;
		links[id].cpu_dai_name = devm_kasprintf(dev, GFP_KERNEL,
							"DMIC%02d Pin", i);
		links[id].codec_name = "dmic-codec";
		links[id].codec_dai_name = "dmic-hifi";
		links[id].platforms = platform_component;
		links[id].num_platforms = ARRAY_SIZE(platform_component);
		links[id].ignore_suspend = 1;
		links[id].dpcm_capture = 1;
		links[id].no_pcm = 1;
		id++;
	}

	/* HDMI */
	for (i = 1; i <= hdmi_num; i++) {
		links[id].name = devm_kasprintf(dev, GFP_KERNEL,
						"iDisp%d", i);
		links[id].id = id;
		links[id].cpu_dai_name = devm_kasprintf(dev, GFP_KERNEL,
							"iDisp%d Pin", i);
		links[id].codec_name = "ehdaudio0D2";
		links[id].codec_dai_name = devm_kasprintf(dev, GFP_KERNEL,
							  "intel-hdmi-hifi%d",
							  i);
		links[id].platforms = platform_component;
		links[id].num_platforms = ARRAY_SIZE(platform_component);
		links[id].init = sof_hdmi_init;
		links[id].dpcm_playback = 1;
		links[id].no_pcm = 1;
		id++;
	}

	return links;
}

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_dai_link *dai_links;
	struct snd_soc_acpi_mach *mach;
	struct sof_card_private *ctx;
	int dmic_num, hdmi_num;
	int ret;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return -ENOMEM;

	if (x86_match_cpu(legacy_cpi_ids)) {
		is_legacy_cpu = 1;
		dmic_num = 0;
		hdmi_num = 0;
		/* default quirk for legacy cpu */
		sof_rt5682_quirk = SOF_RT5682_SSP(2);
	} else {
		dmic_num = 1;
		hdmi_num = 3;
	}

	dmi_check_system(sof_rt5682_quirk_table);

	dev_dbg(&pdev->dev, "sof_rt5682_quirk = %lx\n",
		sof_rt5682_quirk);

	dai_links = sof_card_dai_links_create(&pdev->dev, sof_rt5682_quirk &
					      SOF_RT5682_SSP_MASK,
					      dmic_num, hdmi_num);
	sof_audio_card_rt5682.dai_link = dai_links;
	sof_audio_card_rt5682.num_links = 1 + dmic_num + hdmi_num;

	INIT_LIST_HEAD(&ctx->hdmi_pcm_list);

	sof_audio_card_rt5682.dev = &pdev->dev;
	mach = (&pdev->dev)->platform_data;

	/* set platform name for each dailink */
	ret = snd_soc_fixup_dai_links_platform_name(&sof_audio_card_rt5682,
						    mach->mach_params.platform);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(&sof_audio_card_rt5682, ctx);

	return devm_snd_soc_register_card(&pdev->dev,
					  &sof_audio_card_rt5682);
}

static struct platform_driver sof_audio = {
	.probe = sof_audio_probe,
	.driver = {
		.name = "sof_rt5682",
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(sof_audio)

/* Module information */
MODULE_DESCRIPTION("SOF Audio Machine driver");
MODULE_AUTHOR("Bard Liao <bard.liao@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:sof_rt5682");
