/*
 * ALSA SoC I2S Audio Layer for Broadcom BCM2708 SoC
 *
 * Author:	Florian Meier <florian.meier@koalo.de>
 *		Copyright 2013
 *
 * Based on
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	ALSA SoC I2S (McBSP) Audio Layer for TI DAVINCI processor
 *      Vladimir Barinov, <vbarinov@embeddedalley.com>
 *	Copyright (C) 2007 MontaVista Software, Inc., <source@mvista.com>
 *
 *	OMAP ALSA SoC DAI driver using McBSP port
 *	Copyright (C) 2008 Nokia Corporation
 *	Contact: Jarkko Nikula <jarkko.nikula@bitmer.com>
 *		 Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 *	Freescale SSI ALSA SoC Digital Audio Interface (DAI) driver
 *	Author: Timur Tabi <timur@freescale.com>
 *	Copyright 2007-2010 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "bcm2708-i2s.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <mach/gpio.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#include <asm/system_info.h>

/* Clock registers */
#define BCM2708_CLK_PCMCTL_REG  0x00
#define BCM2708_CLK_PCMDIV_REG  0x04

/* Clock register settings */
#define BCM2708_CLK_PASSWD		(0x5a000000)
#define BCM2708_CLK_PASSWD_MASK	(0xff000000)
#define BCM2708_CLK_MASH(v)		((v) << 9)
#define BCM2708_CLK_FLIP		BIT(8)
#define BCM2708_CLK_BUSY		BIT(7)
#define BCM2708_CLK_KILL		BIT(5)
#define BCM2708_CLK_ENAB		BIT(4)
#define BCM2708_CLK_SRC(v)		(v)

#define BCM2708_CLK_SHIFT		(12)
#define BCM2708_CLK_DIVI(v)		((v) << BCM2708_CLK_SHIFT)
#define BCM2708_CLK_DIVF(v)		(v)
#define BCM2708_CLK_DIVF_MASK		(0xFFF)

enum {
	BCM2708_CLK_MASH_0 = 0,
	BCM2708_CLK_MASH_1,
	BCM2708_CLK_MASH_2,
	BCM2708_CLK_MASH_3,
};

enum {
	BCM2708_CLK_SRC_GND = 0,
	BCM2708_CLK_SRC_OSC,
	BCM2708_CLK_SRC_DBG0,
	BCM2708_CLK_SRC_DBG1,
	BCM2708_CLK_SRC_PLLA,
	BCM2708_CLK_SRC_PLLC,
	BCM2708_CLK_SRC_PLLD,
	BCM2708_CLK_SRC_HDMI,
};

/* Most clocks are not useable (freq = 0) */
static const unsigned int bcm2708_clk_freq[BCM2708_CLK_SRC_HDMI+1] = {
	[BCM2708_CLK_SRC_GND]		= 0,
	[BCM2708_CLK_SRC_OSC]		= 19200000,
	[BCM2708_CLK_SRC_DBG0]		= 0,
	[BCM2708_CLK_SRC_DBG1]		= 0,
	[BCM2708_CLK_SRC_PLLA]		= 0,
	[BCM2708_CLK_SRC_PLLC]		= 0,
	[BCM2708_CLK_SRC_PLLD]		= 500000000,
	[BCM2708_CLK_SRC_HDMI]		= 0,
};

/* I2S registers */
#define BCM2708_I2S_CS_A_REG		0x00
#define BCM2708_I2S_FIFO_A_REG		0x04
#define BCM2708_I2S_MODE_A_REG		0x08
#define BCM2708_I2S_RXC_A_REG		0x0c
#define BCM2708_I2S_TXC_A_REG		0x10
#define BCM2708_I2S_DREQ_A_REG		0x14
#define BCM2708_I2S_INTEN_A_REG	0x18
#define BCM2708_I2S_INTSTC_A_REG	0x1c
#define BCM2708_I2S_GRAY_REG		0x20

/* I2S register settings */
#define BCM2708_I2S_STBY		BIT(25)
#define BCM2708_I2S_SYNC		BIT(24)
#define BCM2708_I2S_RXSEX		BIT(23)
#define BCM2708_I2S_RXF		BIT(22)
#define BCM2708_I2S_TXE		BIT(21)
#define BCM2708_I2S_RXD		BIT(20)
#define BCM2708_I2S_TXD		BIT(19)
#define BCM2708_I2S_RXR		BIT(18)
#define BCM2708_I2S_TXW		BIT(17)
#define BCM2708_I2S_CS_RXERR		BIT(16)
#define BCM2708_I2S_CS_TXERR		BIT(15)
#define BCM2708_I2S_RXSYNC		BIT(14)
#define BCM2708_I2S_TXSYNC		BIT(13)
#define BCM2708_I2S_DMAEN		BIT(9)
#define BCM2708_I2S_RXTHR(v)		((v) << 7)
#define BCM2708_I2S_TXTHR(v)		((v) << 5)
#define BCM2708_I2S_RXCLR		BIT(4)
#define BCM2708_I2S_TXCLR		BIT(3)
#define BCM2708_I2S_TXON		BIT(2)
#define BCM2708_I2S_RXON		BIT(1)
#define BCM2708_I2S_EN			(1)

#define BCM2708_I2S_CLKDIS		BIT(28)
#define BCM2708_I2S_PDMN		BIT(27)
#define BCM2708_I2S_PDME		BIT(26)
#define BCM2708_I2S_FRXP		BIT(25)
#define BCM2708_I2S_FTXP		BIT(24)
#define BCM2708_I2S_CLKM		BIT(23)
#define BCM2708_I2S_CLKI		BIT(22)
#define BCM2708_I2S_FSM		BIT(21)
#define BCM2708_I2S_FSI		BIT(20)
#define BCM2708_I2S_FLEN(v)		((v) << 10)
#define BCM2708_I2S_FSLEN(v)		(v)

#define BCM2708_I2S_CHWEX		BIT(15)
#define BCM2708_I2S_CHEN		BIT(14)
#define BCM2708_I2S_CHPOS(v)		((v) << 4)
#define BCM2708_I2S_CHWID(v)		(v)
#define BCM2708_I2S_CH1(v)		((v) << 16)
#define BCM2708_I2S_CH2(v)		(v)

#define BCM2708_I2S_TX_PANIC(v)	((v) << 24)
#define BCM2708_I2S_RX_PANIC(v)	((v) << 16)
#define BCM2708_I2S_TX(v)		((v) << 8)
#define BCM2708_I2S_RX(v)		(v)

#define BCM2708_I2S_INT_RXERR		BIT(3)
#define BCM2708_I2S_INT_TXERR		BIT(2)
#define BCM2708_I2S_INT_RXR		BIT(1)
#define BCM2708_I2S_INT_TXW		BIT(0)

/* I2S DMA interface */
#define BCM2708_I2S_FIFO_PHYSICAL_ADDR	0x7E203004
#define BCM2708_DMA_DREQ_PCM_TX		2
#define BCM2708_DMA_DREQ_PCM_RX		3

/* I2S pin configuration */
static int bcm2708_i2s_gpio=BCM2708_I2S_GPIO_AUTO;

/* General device struct */
struct bcm2708_i2s_dev {
	struct device				*dev;
	struct snd_dmaengine_dai_dma_data	dma_data[2];
	unsigned int				fmt;
	unsigned int				bclk_ratio;

	struct regmap *i2s_regmap;
	struct regmap *clk_regmap;
};

void bcm2708_i2s_set_gpio(int gpio) {
	bcm2708_i2s_gpio=gpio;
}
EXPORT_SYMBOL(bcm2708_i2s_set_gpio);


static void bcm2708_i2s_start_clock(struct bcm2708_i2s_dev *dev)
{
	/* Start the clock if in master mode */
	unsigned int master = dev->fmt & SND_SOC_DAIFMT_MASTER_MASK;

	switch (master) {
	case SND_SOC_DAIFMT_CBS_CFS:
	case SND_SOC_DAIFMT_CBS_CFM:
		regmap_update_bits(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG,
			BCM2708_CLK_PASSWD_MASK | BCM2708_CLK_ENAB,
			BCM2708_CLK_PASSWD | BCM2708_CLK_ENAB);
		break;
	default:
		break;
	}
}

static void bcm2708_i2s_stop_clock(struct bcm2708_i2s_dev *dev)
{
	uint32_t clkreg;
	int timeout = 1000;

	/* Stop clock */
	regmap_update_bits(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG,
			BCM2708_CLK_PASSWD_MASK | BCM2708_CLK_ENAB,
			BCM2708_CLK_PASSWD);

	/* Wait for the BUSY flag going down */
	while (--timeout) {
		regmap_read(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG, &clkreg);
		if (!(clkreg & BCM2708_CLK_BUSY))
			break;
	}

	if (!timeout) {
		/* KILL the clock */
		dev_err(dev->dev, "I2S clock didn't stop. Kill the clock!\n");
		regmap_update_bits(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG,
			BCM2708_CLK_KILL | BCM2708_CLK_PASSWD_MASK,
			BCM2708_CLK_KILL | BCM2708_CLK_PASSWD);
	}
}

static void bcm2708_i2s_clear_fifos(struct bcm2708_i2s_dev *dev,
				    bool tx, bool rx)
{
	int timeout = 1000;
	uint32_t syncval;
	uint32_t csreg;
	uint32_t i2s_active_state;
	uint32_t clkreg;
	uint32_t clk_active_state;
	uint32_t off;
	uint32_t clr;

	off =  tx ? BCM2708_I2S_TXON : 0;
	off |= rx ? BCM2708_I2S_RXON : 0;

	clr =  tx ? BCM2708_I2S_TXCLR : 0;
	clr |= rx ? BCM2708_I2S_RXCLR : 0;

	/* Backup the current state */
	regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &csreg);
	i2s_active_state = csreg & (BCM2708_I2S_RXON | BCM2708_I2S_TXON);

	regmap_read(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG, &clkreg);
	clk_active_state = clkreg & BCM2708_CLK_ENAB;

	/* Start clock if not running */
	if (!clk_active_state) {
		regmap_update_bits(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG,
			BCM2708_CLK_PASSWD_MASK | BCM2708_CLK_ENAB,
			BCM2708_CLK_PASSWD | BCM2708_CLK_ENAB);
	}

	/* Stop I2S module */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, off, 0);

	/*
	 * Clear the FIFOs
	 * Requires at least 2 PCM clock cycles to take effect
	 */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, clr, clr);

	/* Wait for 2 PCM clock cycles */

	/*
	 * Toggle the SYNC flag. After 2 PCM clock cycles it can be read back
	 * FIXME: This does not seem to work for slave mode!
	 */
	regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &syncval);
	syncval &= BCM2708_I2S_SYNC;

	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_SYNC, ~syncval);

	/* Wait for the SYNC flag changing it's state */
	while (--timeout) {
		regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &csreg);
		if ((csreg & BCM2708_I2S_SYNC) != syncval)
			break;
	}

	if (!timeout)
		dev_err(dev->dev, "I2S SYNC error!\n");

	/* Stop clock if it was not running before */
	if (!clk_active_state)
		bcm2708_i2s_stop_clock(dev);

	/* Restore I2S state */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_RXON | BCM2708_I2S_TXON, i2s_active_state);
}

static int bcm2708_i2s_set_dai_fmt(struct snd_soc_dai *dai,
				      unsigned int fmt)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	dev->fmt = fmt;
	return 0;
}

static int bcm2708_i2s_set_dai_bclk_ratio(struct snd_soc_dai *dai,
				      unsigned int ratio)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	dev->bclk_ratio = ratio;
	return 0;
}


static int bcm2708_i2s_set_function(unsigned offset, int function)
{
	#define GPIOFSEL(x)  (0x00+(x)*4)
	void __iomem *gpio = __io_address(GPIO_BASE);
	unsigned alt = function <= 3 ? function + 4: function == 4 ? 3 : 2;
	unsigned gpiodir;
	unsigned gpio_bank = offset / 10;
	unsigned gpio_field_offset = (offset - 10 * gpio_bank) * 3;

	if (offset >= BCM2708_NR_GPIOS)
		return -EINVAL;

	gpiodir = readl(gpio + GPIOFSEL(gpio_bank));
	gpiodir &= ~(7 << gpio_field_offset);
	gpiodir |= alt << gpio_field_offset;
	writel(gpiodir, gpio + GPIOFSEL(gpio_bank));
	return 0;
}

static void bcm2708_i2s_setup_gpio(void)
{
	/*
	 * This is the common way to handle the GPIO pins for
	 * the Raspberry Pi.
	 * TODO Better way would be to handle
	 * this in the device tree!
	 */
	int pin,pinconfig,startpin,alt;

	/* SPI is on different GPIOs on different boards */
        /* for Raspberry Pi B+, this is pin GPIO18-21, for original on 28-31 */
	if (bcm2708_i2s_gpio==BCM2708_I2S_GPIO_AUTO) {	
		if ((system_rev & 0xffffff) >= 0x10) {
			/* Model B+ */
			pinconfig=BCM2708_I2S_GPIO_PIN18;
		} else {
			/* original */
			pinconfig=BCM2708_I2S_GPIO_PIN28;
		}
	} else {
		pinconfig=bcm2708_i2s_gpio;
	}

	if (pinconfig==BCM2708_I2S_GPIO_PIN18) {
		startpin=18;
		alt=BCM2708_I2S_GPIO_PIN18_ALT;
	} else if (pinconfig==BCM2708_I2S_GPIO_PIN28) {
		startpin=28;
		alt=BCM2708_I2S_GPIO_PIN28_ALT;
	} else {
		printk(KERN_INFO "Can't configure I2S GPIOs, unknown pin mode for I2S: %i\n",pinconfig);
		return;
	}	

	/* configure I2S pins to correct ALT mode */
	for (pin = startpin; pin <= startpin+3; pin++) {
		bcm2708_i2s_set_function(pin, alt);
	}
}

static int bcm2708_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);

	unsigned int sampling_rate = params_rate(params);
	unsigned int data_length, data_delay, bclk_ratio;
	unsigned int ch1pos, ch2pos, mode, format;
	unsigned int mash = BCM2708_CLK_MASH_1;
	unsigned int divi, divf, target_frequency;
	int clk_src = -1;
	unsigned int master = dev->fmt & SND_SOC_DAIFMT_MASTER_MASK;
	bool bit_master =	(master == SND_SOC_DAIFMT_CBS_CFS
					|| master == SND_SOC_DAIFMT_CBS_CFM);

	bool frame_master =	(master == SND_SOC_DAIFMT_CBS_CFS
					|| master == SND_SOC_DAIFMT_CBM_CFS);
	uint32_t csreg;

	/*
	 * If a stream is already enabled,
	 * the registers are already set properly.
	 */
	regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &csreg);

	if (csreg & (BCM2708_I2S_TXON | BCM2708_I2S_RXON))
		return 0;


	bcm2708_i2s_setup_gpio();

	/*
	 * Adjust the data length according to the format.
	 * We prefill the half frame length with an integer
	 * divider of 2400 as explained at the clock settings.
	 * Maybe it is overwritten there, if the Integer mode
	 * does not apply.
	 */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		data_length = 16;
		bclk_ratio = 50;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		data_length = 24;
		bclk_ratio = 50;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		data_length = 32;
		bclk_ratio = 100;
		break;
	default:
		return -EINVAL;
	}

	/* If bclk_ratio already set, use that one. */
	if (dev->bclk_ratio)
		bclk_ratio = dev->bclk_ratio;

	/*
	 * Clock Settings
	 *
	 * The target frequency of the bit clock is
	 *	sampling rate * frame length
	 *
	 * Integer mode:
	 * Sampling rates that are multiples of 8000 kHz
	 * can be driven by the oscillator of 19.2 MHz
	 * with an integer divider as long as the frame length
	 * is an integer divider of 19200000/8000=2400 as set up above.
	 * This is no longer possible if the sampling rate
	 * is too high (e.g. 192 kHz), because the oscillator is too slow.
	 *
	 * MASH mode:
	 * For all other sampling rates, it is not possible to
	 * have an integer divider. Approximate the clock
	 * with the MASH module that induces a slight frequency
	 * variance. To minimize that it is best to have the fastest
	 * clock here. That is PLLD with 500 MHz.
	 */
	target_frequency = sampling_rate * bclk_ratio;
	clk_src = BCM2708_CLK_SRC_OSC;
	mash = BCM2708_CLK_MASH_0;

	if (bcm2708_clk_freq[clk_src] % target_frequency == 0
			&& bit_master && frame_master) {
		divi = bcm2708_clk_freq[clk_src] / target_frequency;
		divf = 0;
	} else {
		uint64_t dividend;

		if (!dev->bclk_ratio) {
			/*
			 * Overwrite bclk_ratio, because the
			 * above trick is not needed or can
			 * not be used.
			 */
			bclk_ratio = 2 * data_length;
		}

		target_frequency = sampling_rate * bclk_ratio;

		clk_src = BCM2708_CLK_SRC_PLLD;
		mash = BCM2708_CLK_MASH_1;

		dividend = bcm2708_clk_freq[clk_src];
		dividend <<= BCM2708_CLK_SHIFT;
		do_div(dividend, target_frequency);
		divi = dividend >> BCM2708_CLK_SHIFT;
		divf = dividend & BCM2708_CLK_DIVF_MASK;
	}

	/* Clock should only be set up here if CPU is clock master */
	if (((dev->fmt & SND_SOC_DAIFMT_MASTER_MASK) == SND_SOC_DAIFMT_CBS_CFS) ||
	    ((dev->fmt & SND_SOC_DAIFMT_MASTER_MASK) == SND_SOC_DAIFMT_CBS_CFM)) {
		/* Set clock divider */
		regmap_write(dev->clk_regmap, BCM2708_CLK_PCMDIV_REG, BCM2708_CLK_PASSWD
				| BCM2708_CLK_DIVI(divi)
				| BCM2708_CLK_DIVF(divf));

		/* Setup clock, but don't start it yet */
		regmap_write(dev->clk_regmap, BCM2708_CLK_PCMCTL_REG, BCM2708_CLK_PASSWD
				| BCM2708_CLK_MASH(mash)
				| BCM2708_CLK_SRC(clk_src));
	}

	/* Setup the frame format */
	format = BCM2708_I2S_CHEN;

	if (data_length >= 24)
		format |= BCM2708_I2S_CHWEX;

	format |= BCM2708_I2S_CHWID((data_length-8)&0xf);

	switch (dev->fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		data_delay = 1;
		break;
	default:
		/*
		 * TODO
		 * Others are possible but are not implemented at the moment.
		 */
		dev_err(dev->dev, "%s:bad format\n", __func__);
		return -EINVAL;
	}

	ch1pos = data_delay;
	ch2pos = bclk_ratio / 2 + data_delay;

	switch (params_channels(params)) {
	case 2:
		format = BCM2708_I2S_CH1(format) | BCM2708_I2S_CH2(format);
		format |= BCM2708_I2S_CH1(BCM2708_I2S_CHPOS(ch1pos));
		format |= BCM2708_I2S_CH2(BCM2708_I2S_CHPOS(ch2pos));
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Set format for both streams.
	 * We cannot set another frame length
	 * (and therefore word length) anyway,
	 * so the format will be the same.
	 */
	regmap_write(dev->i2s_regmap, BCM2708_I2S_RXC_A_REG, format);
	regmap_write(dev->i2s_regmap, BCM2708_I2S_TXC_A_REG, format);

	/* Setup the I2S mode */
	mode = 0;

	if (data_length <= 16) {
		/*
		 * Use frame packed mode (2 channels per 32 bit word)
		 * We cannot set another frame length in the second stream
		 * (and therefore word length) anyway,
		 * so the format will be the same.
		 */
		mode |= BCM2708_I2S_FTXP | BCM2708_I2S_FRXP;
	}

	mode |= BCM2708_I2S_FLEN(bclk_ratio - 1);
	mode |= BCM2708_I2S_FSLEN(bclk_ratio / 2);

	/* Master or slave? */
	switch (dev->fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		/* CPU is master */
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
		/*
		 * CODEC is bit clock master
		 * CPU is frame master
		 */
		mode |= BCM2708_I2S_CLKM;
		break;
	case SND_SOC_DAIFMT_CBS_CFM:
		/*
		 * CODEC is frame master
		 * CPU is bit clock master
		 */
		mode |= BCM2708_I2S_FSM;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		/* CODEC is master */
		mode |= BCM2708_I2S_CLKM;
		mode |= BCM2708_I2S_FSM;
		break;
	default:
		dev_err(dev->dev, "%s:bad master\n", __func__);
		return -EINVAL;
	}

	/*
	 * Invert clocks?
	 *
	 * The BCM approach seems to be inverted to the classical I2S approach.
	 */
	switch (dev->fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		/* None. Therefore, both for BCM */
		mode |= BCM2708_I2S_CLKI;
		mode |= BCM2708_I2S_FSI;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		/* Both. Therefore, none for BCM */
		break;
	case SND_SOC_DAIFMT_NB_IF:
		/*
		 * Invert only frame sync. Therefore,
		 * invert only bit clock for BCM
		 */
		mode |= BCM2708_I2S_CLKI;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		/*
		 * Invert only bit clock. Therefore,
		 * invert only frame sync for BCM
		 */
		mode |= BCM2708_I2S_FSI;
		break;
	default:
		return -EINVAL;
	}

	regmap_write(dev->i2s_regmap, BCM2708_I2S_MODE_A_REG, mode);

	/* Setup the DMA parameters */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_RXTHR(1)
			| BCM2708_I2S_TXTHR(1)
			| BCM2708_I2S_DMAEN, 0xffffffff);

	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_DREQ_A_REG,
			  BCM2708_I2S_TX_PANIC(0x10)
			| BCM2708_I2S_RX_PANIC(0x30)
			| BCM2708_I2S_TX(0x30)
			| BCM2708_I2S_RX(0x20), 0xffffffff);

	/* Clear FIFOs */
	bcm2708_i2s_clear_fifos(dev, true, true);

	return 0;
}

static int bcm2708_i2s_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	uint32_t cs_reg;

	bcm2708_i2s_start_clock(dev);

	/*
	 * Clear both FIFOs if the one that should be started
	 * is not empty at the moment. This should only happen
	 * after overrun. Otherwise, hw_params would have cleared
	 * the FIFO.
	 */
	regmap_read(dev->i2s_regmap, BCM2708_I2S_CS_A_REG, &cs_reg);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK
			&& !(cs_reg & BCM2708_I2S_TXE))
		bcm2708_i2s_clear_fifos(dev, true, false);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE
			&& (cs_reg & BCM2708_I2S_RXD))
		bcm2708_i2s_clear_fifos(dev, false, true);

	return 0;
}

static void bcm2708_i2s_stop(struct bcm2708_i2s_dev *dev,
		struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	uint32_t mask;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		mask = BCM2708_I2S_RXON;
	else
		mask = BCM2708_I2S_TXON;

	regmap_update_bits(dev->i2s_regmap,
			BCM2708_I2S_CS_A_REG, mask, 0);

	/* Stop also the clock when not SND_SOC_DAIFMT_CONT */
	if (!dai->active && !(dev->fmt & SND_SOC_DAIFMT_CONT))
		bcm2708_i2s_stop_clock(dev);
}

static int bcm2708_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	uint32_t mask;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		bcm2708_i2s_start_clock(dev);

		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			mask = BCM2708_I2S_RXON;
		else
			mask = BCM2708_I2S_TXON;

		regmap_update_bits(dev->i2s_regmap,
				BCM2708_I2S_CS_A_REG, mask, mask);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		bcm2708_i2s_stop(dev, substream, dai);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int bcm2708_i2s_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);

	if (dai->active)
		return 0;

	/* Should this still be running stop it */
	bcm2708_i2s_stop_clock(dev);

	/* Enable PCM block */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_EN, BCM2708_I2S_EN);

	/*
	 * Disable STBY.
	 * Requires at least 4 PCM clock cycles to take effect.
	 */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_STBY, BCM2708_I2S_STBY);

	return 0;
}

static void bcm2708_i2s_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);

	bcm2708_i2s_stop(dev, substream, dai);

	/* If both streams are stopped, disable module and clock */
	if (dai->active)
		return;

	/* Disable the module */
	regmap_update_bits(dev->i2s_regmap, BCM2708_I2S_CS_A_REG,
			BCM2708_I2S_EN, 0);

	/*
	 * Stopping clock is necessary, because stop does
	 * not stop the clock when SND_SOC_DAIFMT_CONT
	 */
	bcm2708_i2s_stop_clock(dev);
}

static const struct snd_soc_dai_ops bcm2708_i2s_dai_ops = {
	.startup	= bcm2708_i2s_startup,
	.shutdown	= bcm2708_i2s_shutdown,
	.prepare	= bcm2708_i2s_prepare,
	.trigger	= bcm2708_i2s_trigger,
	.hw_params	= bcm2708_i2s_hw_params,
	.set_fmt	= bcm2708_i2s_set_dai_fmt,
	.set_bclk_ratio	= bcm2708_i2s_set_dai_bclk_ratio
};

static int bcm2708_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct bcm2708_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);

	dai->playback_dma_data = &dev->dma_data[SNDRV_PCM_STREAM_PLAYBACK];
	dai->capture_dma_data = &dev->dma_data[SNDRV_PCM_STREAM_CAPTURE];

	return 0;
}

static struct snd_soc_dai_driver bcm2708_i2s_dai = {
	.name	= "bcm2708-i2s",
	.probe	= bcm2708_i2s_dai_probe,
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates =	SNDRV_PCM_RATE_8000_384000,
		.formats =	SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE
		},
	.capture = {
		.channels_min = 2,
		.channels_max = 2,
		.rates =	SNDRV_PCM_RATE_8000_384000,
		.formats =	SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE
		},
	.ops = &bcm2708_i2s_dai_ops,
	.symmetric_rates = 1
};

static bool bcm2708_i2s_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BCM2708_I2S_CS_A_REG:
	case BCM2708_I2S_FIFO_A_REG:
	case BCM2708_I2S_INTSTC_A_REG:
	case BCM2708_I2S_GRAY_REG:
		return true;
	default:
		return false;
	};
}

static bool bcm2708_i2s_precious_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BCM2708_I2S_FIFO_A_REG:
		return true;
	default:
		return false;
	};
}

static bool bcm2708_clk_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case BCM2708_CLK_PCMCTL_REG:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config bcm2708_regmap_config[] = {
	{
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
		.max_register = BCM2708_I2S_GRAY_REG,
		.precious_reg = bcm2708_i2s_precious_reg,
		.volatile_reg = bcm2708_i2s_volatile_reg,
		.cache_type = REGCACHE_RBTREE,
		.name = "i2s",
	},
	{
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
		.max_register = BCM2708_CLK_PCMDIV_REG,
		.volatile_reg = bcm2708_clk_volatile_reg,
		.cache_type = REGCACHE_RBTREE,
		.name = "clk",
	},
};

static const struct snd_soc_component_driver bcm2708_i2s_component = {
	.name		= "bcm2708-i2s-comp",
};

static const struct snd_pcm_hardware bcm2708_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_JOINT_DUPLEX,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min	= 32,
	.period_bytes_max	= 64 * PAGE_SIZE,
	.periods_min		= 2,
	.periods_max		= 255,
	.buffer_bytes_max	= 128 * PAGE_SIZE,
};

static const struct snd_dmaengine_pcm_config bcm2708_dmaengine_pcm_config = {
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.pcm_hardware = &bcm2708_pcm_hardware,
	.prealloc_buffer_size = 256 * PAGE_SIZE,
};


static int bcm2708_i2s_probe(struct platform_device *pdev)
{
	struct bcm2708_i2s_dev *dev;
	int i;
	int ret;
	struct regmap *regmap[2];
	struct resource *mem[2];

	/* Request both ioareas */
	for (i = 0; i <= 1; i++) {
		void __iomem *base;

		mem[i] = platform_get_resource(pdev, IORESOURCE_MEM, i);
		base = devm_ioremap_resource(&pdev->dev, mem[i]);
		if (IS_ERR(base))
			return PTR_ERR(base);

		regmap[i] = devm_regmap_init_mmio(&pdev->dev, base,
					    &bcm2708_regmap_config[i]);
		if (IS_ERR(regmap[i])) {
			dev_err(&pdev->dev, "I2S probe: regmap init failed\n");
			return PTR_ERR(regmap[i]);
		}
	}

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev),
			   GFP_KERNEL);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	dev->i2s_regmap = regmap[0];
	dev->clk_regmap = regmap[1];

	/* Set the DMA address */
	dev->dma_data[SNDRV_PCM_STREAM_PLAYBACK].addr =
		(dma_addr_t)BCM2708_I2S_FIFO_PHYSICAL_ADDR;

	dev->dma_data[SNDRV_PCM_STREAM_CAPTURE].addr =
		(dma_addr_t)BCM2708_I2S_FIFO_PHYSICAL_ADDR;

	/* Set the DREQ */
	dev->dma_data[SNDRV_PCM_STREAM_PLAYBACK].slave_id =
		BCM2708_DMA_DREQ_PCM_TX;
	dev->dma_data[SNDRV_PCM_STREAM_CAPTURE].slave_id =
		BCM2708_DMA_DREQ_PCM_RX;

	/* Set the bus width */
	dev->dma_data[SNDRV_PCM_STREAM_PLAYBACK].addr_width =
		DMA_SLAVE_BUSWIDTH_4_BYTES;
	dev->dma_data[SNDRV_PCM_STREAM_CAPTURE].addr_width =
		DMA_SLAVE_BUSWIDTH_4_BYTES;

	/* Set burst */
	dev->dma_data[SNDRV_PCM_STREAM_PLAYBACK].maxburst = 2;
	dev->dma_data[SNDRV_PCM_STREAM_CAPTURE].maxburst = 2;

	/* BCLK ratio - use default */
	dev->bclk_ratio = 0;

	/* Store the pdev */
	dev->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, dev);

	ret = snd_soc_register_component(&pdev->dev,
			&bcm2708_i2s_component, &bcm2708_i2s_dai, 1);

	if (ret) {
		dev_err(&pdev->dev, "Could not register DAI: %d\n", ret);
		ret = -ENOMEM;
		return ret;
	}

	ret = snd_dmaengine_pcm_register(&pdev->dev,
				&bcm2708_dmaengine_pcm_config,
				SND_DMAENGINE_PCM_FLAG_COMPAT);
	if (ret) {
		dev_err(&pdev->dev, "Could not register PCM: %d\n", ret);
		snd_soc_unregister_component(&pdev->dev);
		return ret;
	}

	return 0;
}

static int bcm2708_i2s_remove(struct platform_device *pdev)
{
	snd_dmaengine_pcm_unregister(&pdev->dev);
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id bcm2708_i2s_of_match[] = {
	{ .compatible = "brcm,bcm2708-i2s", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2708_i2s_of_match);

static struct platform_driver bcm2708_i2s_driver = {
	.probe		= bcm2708_i2s_probe,
	.remove		= bcm2708_i2s_remove,
	.driver		= {
		.name	= "bcm2708-i2s",
		.owner	= THIS_MODULE,
		.of_match_table = bcm2708_i2s_of_match,
	},
};

module_platform_driver(bcm2708_i2s_driver);

MODULE_ALIAS("platform:bcm2708-i2s");
MODULE_DESCRIPTION("BCM2708 I2S interface");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_LICENSE("GPL v2");
