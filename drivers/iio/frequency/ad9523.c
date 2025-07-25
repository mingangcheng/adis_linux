// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD9523 SPI Low Jitter Clock Generator
 *
 * Copyright 2012 Analog Devices Inc.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/rational.h>

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/frequency/ad9523.h>

#define AD9523_READ	(1 << 15)
#define AD9523_WRITE	(0 << 15)
#define AD9523_CNT(x)	(((x) - 1) << 13)
#define AD9523_ADDR(x)	((x) & 0xFFF)

#define AD9523_R1B	(1 << 16)
#define AD9523_R2B	(2 << 16)
#define AD9523_R3B	(3 << 16)
#define AD9523_TRANSF_LEN(x)			((x) >> 16)

#define AD9523_SERIAL_PORT_CONFIG		(AD9523_R1B | 0x0)
#define AD9523_VERSION_REGISTER			(AD9523_R1B | 0x2)
#define AD9523_PART_REGISTER			(AD9523_R1B | 0x3)
#define AD9523_READBACK_CTRL			(AD9523_R1B | 0x4)

#define AD9523_EEPROM_CUSTOMER_VERSION_ID	(AD9523_R2B | 0x6)

#define AD9523_PLL1_REF_A_DIVIDER		(AD9523_R2B | 0x11)
#define AD9523_PLL1_REF_B_DIVIDER		(AD9523_R2B | 0x13)
#define AD9523_PLL1_REF_TEST_DIVIDER		(AD9523_R1B | 0x14)
#define AD9523_PLL1_FEEDBACK_DIVIDER		(AD9523_R2B | 0x17)
#define AD9523_PLL1_CHARGE_PUMP_CTRL		(AD9523_R2B | 0x19)
#define AD9523_PLL1_INPUT_RECEIVERS_CTRL	(AD9523_R1B | 0x1A)
#define AD9523_PLL1_REF_CTRL			(AD9523_R1B | 0x1B)
#define AD9523_PLL1_MISC_CTRL			(AD9523_R1B | 0x1C)
#define AD9523_PLL1_LOOP_FILTER_CTRL		(AD9523_R1B | 0x1D)

#define AD9523_PLL2_CHARGE_PUMP			(AD9523_R1B | 0xF0)
#define AD9523_PLL2_FEEDBACK_DIVIDER_AB		(AD9523_R1B | 0xF1)
#define AD9523_PLL2_CTRL			(AD9523_R1B | 0xF2)
#define AD9523_PLL2_VCO_CTRL			(AD9523_R1B | 0xF3)
#define AD9523_PLL2_VCO_DIVIDER			(AD9523_R1B | 0xF4)
#define AD9523_PLL2_LOOP_FILTER_CTRL		(AD9523_R2B | 0xF6)
#define AD9523_PLL2_R2_DIVIDER			(AD9523_R1B | 0xF7)

#define AD9523_CHANNEL_CLOCK_DIST(ch)		(AD9523_R3B | (0x192 + 3 * ch))

#define AD9523_PLL1_OUTPUT_CTRL			(AD9523_R1B | 0x1BA)
#define AD9523_PLL1_OUTPUT_CHANNEL_CTRL		(AD9523_R1B | 0x1BB)

#define AD9523_READBACK_0			(AD9523_R1B | 0x22C)
#define AD9523_READBACK_1			(AD9523_R1B | 0x22D)

#define AD9523_STATUS_SIGNALS			(AD9523_R3B | 0x232)
#define AD9523_POWER_DOWN_CTRL			(AD9523_R1B | 0x233)
#define AD9523_IO_UPDATE			(AD9523_R1B | 0x234)

#define AD9523_EEPROM_DATA_XFER_STATUS		(AD9523_R1B | 0xB00)
#define AD9523_EEPROM_ERROR_READBACK		(AD9523_R1B | 0xB01)
#define AD9523_EEPROM_CTRL1			(AD9523_R1B | 0xB02)
#define AD9523_EEPROM_CTRL2			(AD9523_R1B | 0xB03)

/* AD9523_SERIAL_PORT_CONFIG */

#define AD9523_SER_CONF_SDO_ACTIVE		(1 << 7)
#define AD9523_SER_CONF_SOFT_RESET		(1 << 5)

/* AD9523_READBACK_CTRL */
#define AD9523_READBACK_CTRL_READ_BUFFERED	(1 << 0)

/* AD9523_PLL1_CHARGE_PUMP_CTRL */
#define AD9523_PLL1_CHARGE_PUMP_CURRENT_nA(x)	(((x) / 500) & 0x7F)
#define AD9523_PLL1_CHARGE_PUMP_TRISTATE	(1 << 7)
#define AD9523_PLL1_CHARGE_PUMP_MODE_NORMAL	(3 << 8)
#define AD9523_PLL1_CHARGE_PUMP_MODE_PUMP_DOWN	(2 << 8)
#define AD9523_PLL1_CHARGE_PUMP_MODE_PUMP_UP	(1 << 8)
#define AD9523_PLL1_CHARGE_PUMP_MODE_TRISTATE	(0 << 8)
#define AD9523_PLL1_BACKLASH_PW_MIN		(0 << 10)
#define AD9523_PLL1_BACKLASH_PW_LOW		(1 << 10)
#define AD9523_PLL1_BACKLASH_PW_HIGH		(2 << 10)
#define AD9523_PLL1_BACKLASH_PW_MAX		(3 << 10)

/* AD9523_PLL1_INPUT_RECEIVERS_CTRL */
#define AD9523_PLL1_REF_TEST_RCV_EN		(1 << 7)
#define AD9523_PLL1_REFB_DIFF_RCV_EN		(1 << 6)
#define AD9523_PLL1_REFA_DIFF_RCV_EN		(1 << 5)
#define AD9523_PLL1_REFB_RCV_EN			(1 << 4)
#define AD9523_PLL1_REFA_RCV_EN			(1 << 3)
#define AD9523_PLL1_REFA_REFB_PWR_CTRL_EN	(1 << 2)
#define AD9523_PLL1_OSC_IN_CMOS_NEG_INP_EN	(1 << 1)
#define AD9523_PLL1_OSC_IN_DIFF_EN		(1 << 0)

/* AD9523_PLL1_REF_CTRL */
#define AD9523_PLL1_BYPASS_REF_TEST_DIV_EN	(1 << 7)
#define AD9523_PLL1_BYPASS_FEEDBACK_DIV_EN	(1 << 6)
#define AD9523_PLL1_ZERO_DELAY_MODE_INT		(1 << 5)
#define AD9523_PLL1_ZERO_DELAY_MODE_EXT		(0 << 5)
#define AD9523_PLL1_OSC_IN_PLL_FEEDBACK_EN	(1 << 4)
#define AD9523_PLL1_ZD_IN_CMOS_NEG_INP_EN	(1 << 3)
#define AD9523_PLL1_ZD_IN_DIFF_EN		(1 << 2)
#define AD9523_PLL1_REFB_CMOS_NEG_INP_EN	(1 << 1)
#define AD9523_PLL1_REFA_CMOS_NEG_INP_EN	(1 << 0)

/* AD9523_PLL1_MISC_CTRL */
#define AD9523_PLL1_REFB_INDEP_DIV_CTRL_EN	(1 << 7)
#define AD9523_PLL1_OSC_CTRL_FAIL_VCC_BY2_EN	(1 << 6)
#define AD9523_PLL1_REF_MODE(x)			((x) << 2)
#define AD9523_PLL1_BYPASS_REFB_DIV		(1 << 1)
#define AD9523_PLL1_BYPASS_REFA_DIV		(1 << 0)

/* AD9523_PLL1_LOOP_FILTER_CTRL */
#define AD9523_PLL1_LOOP_FILTER_RZERO(x)	((x) & 0xF)

/* AD9523_PLL2_CHARGE_PUMP */
#define AD9523_PLL2_CHARGE_PUMP_CURRENT_nA(x)	((x) / 3500)

/* AD9523_PLL2_FEEDBACK_DIVIDER_AB */
#define AD9523_PLL2_FB_NDIV_A_CNT(x)		(((x) & 0x3) << 6)
#define AD9523_PLL2_FB_NDIV_B_CNT(x)		(((x) & 0x3F) << 0)
#define AD9523_PLL2_FB_NDIV(a, b)		(4 * (b) + (a))

/* AD9523_PLL2_CTRL */
#define AD9523_PLL2_CHARGE_PUMP_MODE_NORMAL	(3 << 0)
#define AD9523_PLL2_CHARGE_PUMP_MODE_PUMP_DOWN	(2 << 0)
#define AD9523_PLL2_CHARGE_PUMP_MODE_PUMP_UP	(1 << 0)
#define AD9523_PLL2_CHARGE_PUMP_MODE_TRISTATE	(0 << 0)
#define AD9523_PLL2_BACKLASH_PW_MIN		(0 << 2)
#define AD9523_PLL2_BACKLASH_PW_LOW		(1 << 2)
#define AD9523_PLL2_BACKLASH_PW_HIGH		(2 << 2)
#define AD9523_PLL2_BACKLASH_PW_MAX		(3 << 1)
#define AD9523_PLL2_BACKLASH_CTRL_EN		(1 << 4)
#define AD9523_PLL2_FREQ_DOUBLER_EN		(1 << 5)
#define AD9523_PLL2_LOCK_DETECT_PWR_DOWN_EN	(1 << 7)

/* AD9523_PLL2_VCO_CTRL */
#define AD9523_PLL2_VCO_CALIBRATE		(1 << 1)
#define AD9523_PLL2_FORCE_VCO_MIDSCALE		(1 << 2)
#define AD9523_PLL2_FORCE_REFERENCE_VALID	(1 << 3)
#define AD9523_PLL2_FORCE_RELEASE_SYNC		(1 << 4)

/* AD9523_PLL2_VCO_DIVIDER */
#define AD9523_PLL2_VCO_DIV_M1(x)		((((x) - 3) & 0x3) << 0)
#define AD9523_PLL2_VCO_DIV_M2(x)		((((x) - 3) & 0x3) << 4)
#define AD9523_PLL2_VCO_DIV_M1_PWR_DOWN_EN	(1 << 2)
#define AD9523_PLL2_VCO_DIV_M2_PWR_DOWN_EN	(1 << 6)

/* AD9523_PLL2_LOOP_FILTER_CTRL */
#define AD9523_PLL2_LOOP_FILTER_CPOLE1(x)	(((x) & 0x7) << 0)
#define AD9523_PLL2_LOOP_FILTER_RZERO(x)	(((x) & 0x7) << 3)
#define AD9523_PLL2_LOOP_FILTER_RPOLE2(x)	(((x) & 0x7) << 6)
#define AD9523_PLL2_LOOP_FILTER_RZERO_BYPASS_EN	(1 << 8)

/* AD9523_PLL2_R2_DIVIDER */
#define AD9523_PLL2_R2_DIVIDER_VAL(x)		(((x) & 0x1F) << 0)

/* AD9523_CHANNEL_CLOCK_DIST */
#define AD9523_CLK_DIST_DIV_PHASE(x)		(((x) & 0x3F) << 18)
#define AD9523_CLK_DIST_DIV_PHASE_REV(x)	((ret >> 18) & 0x3F)
#define AD9523_CLK_DIST_DIV(x)			((((x) - 1) & 0x3FF) << 8)
#define AD9523_CLK_DIST_DIV_REV(x)		(((ret >> 8) & 0x3FF) + 1)
#define AD9523_CLK_DIST_INV_DIV_OUTPUT_EN	(1 << 7)
#define AD9523_CLK_DIST_IGNORE_SYNC_EN		(1 << 6)
#define AD9523_CLK_DIST_PWR_DOWN_EN		(1 << 5)
#define AD9523_CLK_DIST_LOW_PWR_MODE_EN		(1 << 4)
#define AD9523_CLK_DIST_DRIVER_MODE(x)		(((x) & 0xF) << 0)

/* AD9523_PLL1_OUTPUT_CTRL */
#define AD9523_PLL1_OUTP_CTRL_VCO_DIV_SEL_CH6_M2	(1 << 7)
#define AD9523_PLL1_OUTP_CTRL_VCO_DIV_SEL_CH5_M2	(1 << 6)
#define AD9523_PLL1_OUTP_CTRL_VCO_DIV_SEL_CH4_M2	(1 << 5)
#define AD9523_PLL1_OUTP_CTRL_CMOS_DRV_WEAK		(1 << 4)
#define AD9523_PLL1_OUTP_CTRL_OUTPUT_DIV_1		(0 << 0)
#define AD9523_PLL1_OUTP_CTRL_OUTPUT_DIV_2		(1 << 0)
#define AD9523_PLL1_OUTP_CTRL_OUTPUT_DIV_4		(2 << 0)
#define AD9523_PLL1_OUTP_CTRL_OUTPUT_DIV_8		(4 << 0)
#define AD9523_PLL1_OUTP_CTRL_OUTPUT_DIV_16		(8 << 0)

/* AD9523_PLL1_OUTPUT_CHANNEL_CTRL */
#define AD9523_PLL1_OUTP_CH_CTRL_OUTPUT_PWR_DOWN_EN	(1 << 7)
#define AD9523_PLL1_OUTP_CH_CTRL_VCO_DIV_SEL_CH9_M2	(1 << 6)
#define AD9523_PLL1_OUTP_CH_CTRL_VCO_DIV_SEL_CH8_M2	(1 << 5)
#define AD9523_PLL1_OUTP_CH_CTRL_VCO_DIV_SEL_CH7_M2	(1 << 4)
#define AD9523_PLL1_OUTP_CH_CTRL_VCXO_SRC_SEL_CH3	(1 << 3)
#define AD9523_PLL1_OUTP_CH_CTRL_VCXO_SRC_SEL_CH2	(1 << 2)
#define AD9523_PLL1_OUTP_CH_CTRL_VCXO_SRC_SEL_CH1	(1 << 1)
#define AD9523_PLL1_OUTP_CH_CTRL_VCXO_SRC_SEL_CH0	(1 << 0)

/* AD9523_READBACK_0 */
#define AD9523_READBACK_0_STAT_PLL2_REF_CLK		(1 << 7)
#define AD9523_READBACK_0_STAT_PLL2_FB_CLK		(1 << 6)
#define AD9523_READBACK_0_STAT_VCXO			(1 << 5)
#define AD9523_READBACK_0_STAT_REF_TEST			(1 << 4)
#define AD9523_READBACK_0_STAT_REFB			(1 << 3)
#define AD9523_READBACK_0_STAT_REFA			(1 << 2)
#define AD9523_READBACK_0_STAT_PLL2_LD			(1 << 1)
#define AD9523_READBACK_0_STAT_PLL1_LD			(1 << 0)

/* AD9523_READBACK_1 */
#define AD9523_READBACK_1_HOLDOVER_ACTIVE		(1 << 3)
#define AD9523_READBACK_1_AUTOMODE_SEL_REFB		(1 << 2)
#define AD9523_READBACK_1_VCO_CALIB_IN_PROGRESS		(1 << 0)

/* AD9523_STATUS_SIGNALS */
#define AD9523_STATUS_SIGNALS_SYNC_MAN_CTRL		(1 << 16)
#define AD9523_STATUS_MONITOR_01_PLL12_LOCKED		(0x302)
/* AD9523_POWER_DOWN_CTRL */
#define AD9523_POWER_DOWN_CTRL_PLL1_PWR_DOWN		(1 << 2)
#define AD9523_POWER_DOWN_CTRL_PLL2_PWR_DOWN		(1 << 1)
#define AD9523_POWER_DOWN_CTRL_DIST_PWR_DOWN		(1 << 0)

/* AD9523_IO_UPDATE */
#define AD9523_IO_UPDATE_EN				(1 << 0)

/* AD9523_EEPROM_DATA_XFER_STATUS */
#define AD9523_EEPROM_DATA_XFER_IN_PROGRESS		(1 << 0)

/* AD9523_EEPROM_ERROR_READBACK */
#define AD9523_EEPROM_ERROR_READBACK_FAIL		(1 << 0)

/* AD9523_EEPROM_CTRL1 */
#define AD9523_EEPROM_CTRL1_SOFT_EEPROM			(1 << 1)
#define AD9523_EEPROM_CTRL1_EEPROM_WRITE_PROT_DIS	(1 << 0)

/* AD9523_EEPROM_CTRL2 */
#define AD9523_EEPROM_CTRL2_REG2EEPROM			(1 << 0)

#define AD9523_NUM_CHAN					14
#define AD9523_NUM_CHAN_ALT_CLK_SRC			10

/* Helpers to avoid excess line breaks */
#define AD_IFE(_pde, _a, _b) ((pdata->_pde) ? _a : _b)
#define AD_IF(_pde, _a) AD_IFE(_pde, _a, 0)

#define AD9523_VCO_FREQ_MIN	2940000
#define AD9523_VCO_FREQ_MAX 3100000

enum {
	AD9523_STAT_PLL1_LD,
	AD9523_STAT_PLL2_LD,
	AD9523_STAT_REFA,
	AD9523_STAT_REFB,
	AD9523_STAT_REF_TEST,
	AD9523_STAT_VCXO,
	AD9523_STAT_PLL2_FB_CLK,
	AD9523_STAT_PLL2_REF_CLK,
	AD9523_SYNC,
	AD9523_EEPROM,
};

enum {
	AD9523_VCO1,
	AD9523_VCO2,
	AD9523_VCXO,
	AD9523_NUM_CLK_SRC,
};

struct ad9523_outputs {
	struct clk_hw hw;
	struct iio_dev *indio_dev;
	unsigned num;
	bool is_enabled;
};

#define to_ad9523_clk_output(_hw) container_of(_hw, struct ad9523_outputs, hw)

struct ad9523_state {
	struct spi_device		*spi;
	struct ad9523_platform_data	*pdata;
	struct ad9523_outputs		output[AD9523_NUM_CHAN];
	struct iio_chan_spec		ad9523_channels[AD9523_NUM_CHAN];
	struct gpio_desc		*pwrdown_gpio;
	struct gpio_desc		*reset_gpio;
	struct gpio_desc		*sync_gpio;
	struct clk_onecell_data		clk_data;
	struct clk 			*clks[AD9523_NUM_CHAN];

	unsigned long		vcxo_freq;
	unsigned long		vco_freq;
	unsigned long		vco_out_freq[AD9523_NUM_CLK_SRC];
	unsigned char		vco_out_map[AD9523_NUM_CHAN_ALT_CLK_SRC];

	/*
	 * Lock for accessing device registers. Some operations require
	 * multiple consecutive R/W operations, during which the device
	 * shouldn't be interrupted.  The buffers are also shared across
	 * all operations so need to be protected on stand alone reads and
	 * writes.
	 */
	struct mutex		lock;

	/*
	 * DMA (thus cache coherency maintenance) may require that
	 * transfer buffers live in their own cache lines.
	 */
	union {
		__be32 d32;
		u8 d8[4];
	} data[2] __aligned(IIO_DMA_MINALIGN);
};

static int ad9523_read(struct iio_dev *indio_dev, unsigned int addr)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	int ret;

	/* We encode the register size 1..3 bytes into the register address.
	 * On transfer we get the size from the register datum, and make sure
	 * the result is properly aligned.
	 */

	struct spi_transfer t[] = {
		{
			.tx_buf = &st->data[0].d8[2],
			.len = 2,
		}, {
			.rx_buf = &st->data[1].d8[4 - AD9523_TRANSF_LEN(addr)],
			.len = AD9523_TRANSF_LEN(addr),
		},
	};

	st->data[0].d32 = cpu_to_be32(AD9523_READ |
				      AD9523_CNT(AD9523_TRANSF_LEN(addr)) |
				      AD9523_ADDR(addr));

	ret = spi_sync_transfer(st->spi, t, ARRAY_SIZE(t));
	if (ret < 0)
		dev_err(&indio_dev->dev, "read failed (%d)", ret);
	else
		ret = be32_to_cpu(st->data[1].d32) & (0xFFFFFF >>
				  (8 * (3 - AD9523_TRANSF_LEN(addr))));

	return ret;
};

static int ad9523_write(struct iio_dev *indio_dev,
		unsigned int addr, unsigned int val)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	int ret;
	struct spi_transfer t[] = {
		{
			.tx_buf = &st->data[0].d8[2],
			.len = 2,
		}, {
			.tx_buf = &st->data[1].d8[4 - AD9523_TRANSF_LEN(addr)],
			.len = AD9523_TRANSF_LEN(addr),
		},
	};

	st->data[0].d32 = cpu_to_be32(AD9523_WRITE |
				      AD9523_CNT(AD9523_TRANSF_LEN(addr)) |
				      AD9523_ADDR(addr));
	st->data[1].d32 = cpu_to_be32(val);

	ret = spi_sync_transfer(st->spi, t, ARRAY_SIZE(t));

	if (ret < 0)
		dev_err(&indio_dev->dev, "write failed (%d)", ret);

	return ret;
}

static int ad9523_io_update(struct iio_dev *indio_dev)
{
	return ad9523_write(indio_dev, AD9523_IO_UPDATE, AD9523_IO_UPDATE_EN);
}

static int ad9523_vco_out_map(struct iio_dev *indio_dev,
			      unsigned int ch, unsigned int out)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	int ret;
	unsigned int mask;

	switch (ch) {
	case 0 ... 3:
		ret = ad9523_read(indio_dev, AD9523_PLL1_OUTPUT_CHANNEL_CTRL);
		if (ret < 0)
			break;
		mask = AD9523_PLL1_OUTP_CH_CTRL_VCXO_SRC_SEL_CH0 << ch;
		if (out) {
			ret |= mask;
			out = AD9523_VCXO;
		} else {
			ret &= ~mask;
		}
		ret = ad9523_write(indio_dev,
				   AD9523_PLL1_OUTPUT_CHANNEL_CTRL, ret);
		break;
	case 4 ... 6:
		ret = ad9523_read(indio_dev, AD9523_PLL1_OUTPUT_CTRL);
		if (ret < 0)
			break;
		mask = AD9523_PLL1_OUTP_CTRL_VCO_DIV_SEL_CH4_M2 << (ch - 4);
		if (out)
			ret |= mask;
		else
			ret &= ~mask;
		ret = ad9523_write(indio_dev, AD9523_PLL1_OUTPUT_CTRL, ret);
		break;
	case 7 ... 9:
		ret = ad9523_read(indio_dev, AD9523_PLL1_OUTPUT_CHANNEL_CTRL);
		if (ret < 0)
			break;
		mask = AD9523_PLL1_OUTP_CH_CTRL_VCO_DIV_SEL_CH7_M2 << (ch - 7);
		if (out)
			ret |= mask;
		else
			ret &= ~mask;
		ret = ad9523_write(indio_dev,
				   AD9523_PLL1_OUTPUT_CHANNEL_CTRL, ret);
		break;
	default:
		return 0;
	}

	st->vco_out_map[ch] = out;

	return ret;
}

static int ad9523_set_clock_provider(struct iio_dev *indio_dev,
			      unsigned int ch, unsigned long freq)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	long tmp1, tmp2;
	bool use_alt_clk_src;

	switch (ch) {
	case 0 ... 3:
		use_alt_clk_src = (freq == st->vco_out_freq[AD9523_VCXO]);
		break;
	case 4 ... 9:
		tmp1 = st->vco_out_freq[AD9523_VCO1] / freq;
		tmp2 = st->vco_out_freq[AD9523_VCO2] / freq;
		tmp1 *= freq;
		tmp2 *= freq;
		use_alt_clk_src = (abs(tmp1 - freq) > abs(tmp2 - freq));
		break;
	default:
		/* Ch 10..14: No action required, return success */
		return 0;
	}

	return ad9523_vco_out_map(indio_dev, ch, use_alt_clk_src);
}

static int ad9523_store_eeprom(struct iio_dev *indio_dev)
{
	int ret, tmp;

	ret = ad9523_write(indio_dev, AD9523_EEPROM_CTRL1,
			   AD9523_EEPROM_CTRL1_EEPROM_WRITE_PROT_DIS);
	if (ret < 0)
		return ret;
	ret = ad9523_write(indio_dev, AD9523_EEPROM_CTRL2,
			   AD9523_EEPROM_CTRL2_REG2EEPROM);
	if (ret < 0)
		return ret;

	tmp = 4;
	do {
		msleep(20);
		ret = ad9523_read(indio_dev,
				  AD9523_EEPROM_DATA_XFER_STATUS);
		if (ret < 0)
			return ret;
	} while ((ret & AD9523_EEPROM_DATA_XFER_IN_PROGRESS) && tmp--);

	ret = ad9523_write(indio_dev, AD9523_EEPROM_CTRL1, 0);
	if (ret < 0)
		return ret;

	ret = ad9523_read(indio_dev, AD9523_EEPROM_ERROR_READBACK);
	if (ret < 0)
		return ret;

	if (ret & AD9523_EEPROM_ERROR_READBACK_FAIL) {
		dev_err(&indio_dev->dev, "Verify EEPROM failed");
		ret = -EIO;
	}

	return ret;
}

static int ad9523_sync(struct iio_dev *indio_dev)
{
	int ret, tmp;

	ret = ad9523_read(indio_dev, AD9523_STATUS_SIGNALS);
	if (ret < 0)
		return ret;

	tmp = ret;
	tmp |= AD9523_STATUS_SIGNALS_SYNC_MAN_CTRL;

	ret = ad9523_write(indio_dev, AD9523_STATUS_SIGNALS, tmp);
	if (ret < 0)
		return ret;

	ad9523_io_update(indio_dev);
	tmp &= ~AD9523_STATUS_SIGNALS_SYNC_MAN_CTRL;

	ret = ad9523_write(indio_dev, AD9523_STATUS_SIGNALS, tmp);
	if (ret < 0)
		return ret;

	return ad9523_io_update(indio_dev);
}

static ssize_t ad9523_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	struct ad9523_state *st = iio_priv(indio_dev);
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret < 0)
		return ret;

	if (!state)
		return len;

	mutex_lock(&st->lock);
	switch ((u32)this_attr->address) {
	case AD9523_SYNC:
		ret = ad9523_sync(indio_dev);
		break;
	case AD9523_EEPROM:
		ret = ad9523_store_eeprom(indio_dev);
		break;
	default:
		ret = -ENODEV;
	}
	mutex_unlock(&st->lock);

	return ret ? ret : len;
}

static ssize_t ad9523_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	struct ad9523_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	ret = ad9523_read(indio_dev, AD9523_READBACK_0);
	if (ret >= 0) {
		ret = sysfs_emit(buf, "%d\n", !!(ret & (1 <<
			(u32)this_attr->address)));
	}
	mutex_unlock(&st->lock);

	return ret;
}

static IIO_DEVICE_ATTR(pll1_locked, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_PLL1_LD);

static IIO_DEVICE_ATTR(pll2_locked, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_PLL2_LD);

static IIO_DEVICE_ATTR(pll1_reference_clk_a_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_REFA);

static IIO_DEVICE_ATTR(pll1_reference_clk_b_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_REFB);

static IIO_DEVICE_ATTR(pll1_reference_clk_test_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_REF_TEST);

static IIO_DEVICE_ATTR(vcxo_clk_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_VCXO);

static IIO_DEVICE_ATTR(pll2_feedback_clk_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_PLL2_FB_CLK);

static IIO_DEVICE_ATTR(pll2_reference_clk_present, S_IRUGO,
			ad9523_show,
			NULL,
			AD9523_STAT_PLL2_REF_CLK);

static IIO_DEVICE_ATTR(sync_dividers, S_IWUSR,
			NULL,
			ad9523_store,
			AD9523_SYNC);

static IIO_DEVICE_ATTR(store_eeprom, S_IWUSR,
			NULL,
			ad9523_store,
			AD9523_EEPROM);

static struct attribute *ad9523_attributes[] = {
	&iio_dev_attr_sync_dividers.dev_attr.attr,
	&iio_dev_attr_store_eeprom.dev_attr.attr,
	&iio_dev_attr_pll2_feedback_clk_present.dev_attr.attr,
	&iio_dev_attr_pll2_reference_clk_present.dev_attr.attr,
	&iio_dev_attr_pll1_reference_clk_a_present.dev_attr.attr,
	&iio_dev_attr_pll1_reference_clk_b_present.dev_attr.attr,
	&iio_dev_attr_pll1_reference_clk_test_present.dev_attr.attr,
	&iio_dev_attr_vcxo_clk_present.dev_attr.attr,
	&iio_dev_attr_pll1_locked.dev_attr.attr,
	&iio_dev_attr_pll2_locked.dev_attr.attr,
	NULL,
};

static const struct attribute_group ad9523_attribute_group = {
	.attrs = ad9523_attributes,
};

static int ad9523_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long m)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	unsigned int code;
	int ret;

	mutex_lock(&st->lock);
	ret = ad9523_read(indio_dev, AD9523_CHANNEL_CLOCK_DIST(chan->channel));
	mutex_unlock(&st->lock);

	if (ret < 0)
		return ret;

	switch (m) {
	case IIO_CHAN_INFO_RAW:
		*val = !(ret & AD9523_CLK_DIST_PWR_DOWN_EN);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_FREQUENCY:
		*val = st->vco_out_freq[st->vco_out_map[chan->channel]] /
			AD9523_CLK_DIST_DIV_REV(ret);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PHASE:
		code = (AD9523_CLK_DIST_DIV_PHASE_REV(ret) * 3141592) /
			AD9523_CLK_DIST_DIV_REV(ret);
		*val = code / 1000000;
		*val2 = code % 1000000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
};

static int ad9523_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val,
			    int val2,
			    long mask)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	unsigned int reg;
	int ret, tmp, code;

	mutex_lock(&st->lock);
	ret = ad9523_read(indio_dev, AD9523_CHANNEL_CLOCK_DIST(chan->channel));
	if (ret < 0)
		goto out;

	reg = ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val)
			reg &= ~AD9523_CLK_DIST_PWR_DOWN_EN;
		else
			reg |= AD9523_CLK_DIST_PWR_DOWN_EN;

		st->output[chan->channel].is_enabled = !!val;

		break;
	case IIO_CHAN_INFO_FREQUENCY:
		if (val <= 0) {
			ret = -EINVAL;
			goto out;
		}
		ret = ad9523_set_clock_provider(indio_dev, chan->channel, val);
		if (ret < 0)
			goto out;
		tmp = DIV_ROUND_CLOSEST(st->vco_out_freq[st->vco_out_map[
				chan->channel]], val);
		tmp = clamp(tmp, 1, 1024);
		reg &= ~(0x3FF << 8);
		reg |= AD9523_CLK_DIST_DIV(tmp);
		break;
	case IIO_CHAN_INFO_PHASE:
		code = val * 1000000 + val2 % 1000000;
		tmp = (code * AD9523_CLK_DIST_DIV_REV(ret)) / 3141592;
		tmp = clamp(tmp, 0, 63);
		reg &= ~AD9523_CLK_DIST_DIV_PHASE(~0);
		reg |= AD9523_CLK_DIST_DIV_PHASE(tmp);
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = ad9523_write(indio_dev, AD9523_CHANNEL_CLOCK_DIST(chan->channel),
			   reg);
	if (ret < 0)
		goto out;

	ad9523_io_update(indio_dev);
out:
	mutex_unlock(&st->lock);
	return ret;
}

static int ad9523_reg_access(struct iio_dev *indio_dev,
			      unsigned int reg, unsigned int writeval,
			      unsigned int *readval)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	if (readval == NULL) {
		ret = ad9523_write(indio_dev, reg | AD9523_R1B, writeval);
		ad9523_io_update(indio_dev);
	} else {
		ret = ad9523_read(indio_dev, reg | AD9523_R1B);
		if (ret < 0)
			goto out_unlock;
		*readval = ret;
		ret = 0;
	}

out_unlock:
	mutex_unlock(&st->lock);

	return ret;
}

static const struct iio_info ad9523_info = {
	.read_raw = &ad9523_read_raw,
	.write_raw = &ad9523_write_raw,
	.debugfs_reg_access = &ad9523_reg_access,
	.attrs = &ad9523_attribute_group,
};

static bool ad9523_pll2_valid_div(unsigned int div)
{
	if (div < 16)
		return false;

	switch (div) {
	case 18:
	case 19:
	case 23:
	case 27:
		return false;
	}

	return true;
}

static int ad9523_calc_dividers(unsigned int vcxo_freq, unsigned int m1_freq,
	unsigned int m2_freq, struct ad9523_platform_data *pdata)
{
	unsigned long n2[2], r2[2];
	unsigned int fpfd;
	unsigned int m_freq, vco_freq;
	unsigned int m, m1, m2;

	m1_freq /= 1000;
	m2_freq /= 1000;
	vcxo_freq /= 1000;

	if (m1_freq != 0)
		m_freq = m1_freq;
	else
		m_freq = m2_freq;

	for (m = 3; m <= 5; m++) {
		vco_freq = m_freq * m;
		if (AD9523_VCO_FREQ_MIN <= vco_freq &&
		    AD9523_VCO_FREQ_MAX >= vco_freq)
			break;
	}

	if (m == 6)
		return -EINVAL;

	if (m1_freq != 0) {
		m1 = m;
		if (m2_freq != 0) {
			m2 = vco_freq / m2_freq;
			if (m2 < 3 || m2 > 5 || vco_freq % m2_freq > 1)
				return -EINVAL;
		} else {
			m2 = 3;
		}
	} else {
		m1 = 3;
		m2 = m;
	}

	rational_best_approximation(vco_freq, vcxo_freq, 255, 31,
		&n2[0], &r2[0]);

	pdata->pll2_freq_doubler_en = false;

	if (vco_freq != vcxo_freq * n2[0] / r2[0]) {
		rational_best_approximation(vco_freq, vcxo_freq*2, 255, 31,
			&n2[1], &r2[1]);

		if (abs((int)(vco_freq / n2[0]) - (int)(vcxo_freq / r2[0])) >
		    abs((int)(vco_freq / n2[1] / 2) - (int)(vcxo_freq / r2[1]))) {
			n2[0] = n2[1];
			r2[0] = r2[1];
			pdata->pll2_freq_doubler_en = true;
		}
	}

	fpfd = vcxo_freq * (pdata->pll2_freq_doubler_en ? 2 : 1) / r2[0];

	while (fpfd > 259000 || !ad9523_pll2_valid_div(n2[0])) {
		fpfd /= 2;
		n2[0] *= 2;
		r2[0] *= 2;
	}

	pdata->pll2_r2_div = r2[0];
	pdata->pll2_vco_div_m1 = m1;
	pdata->pll2_vco_div_m2 = m2;
	pdata->pll2_ndiv_a_cnt = n2[0] % 4;
	pdata->pll2_ndiv_b_cnt = n2[0] / 4;

	return 0;
}

static long ad9523_get_clk_attr(struct clk_hw *hw, long mask)
{
	struct iio_dev *indio_dev = to_ad9523_clk_output(hw)->indio_dev;
	int val, ret;
	struct iio_chan_spec chan;

	chan.channel = to_ad9523_clk_output(hw)->num;

	ret = ad9523_read_raw(indio_dev, &chan, &val, NULL, mask);

	if (ret == IIO_VAL_INT)
		return val;

	return ret;
}

static unsigned long ad9523_clk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	return ad9523_get_clk_attr(hw, IIO_CHAN_INFO_FREQUENCY);
}

static int ad9523_clk_is_enabled(struct clk_hw *hw)
{
	return to_ad9523_clk_output(hw)->is_enabled;
}

static long ad9523_set_clk_attr(struct clk_hw *hw, long mask, unsigned long val)
{
	struct iio_dev *indio_dev = to_ad9523_clk_output(hw)->indio_dev;
	struct iio_chan_spec chan;

	chan.channel = to_ad9523_clk_output(hw)->num;

	return ad9523_write_raw(indio_dev, &chan, val, 0, mask);
}

static int ad9523_clk_prepare(struct clk_hw *hw)
{
	return ad9523_set_clk_attr(hw, IIO_CHAN_INFO_RAW, 1);
}

static void ad9523_clk_unprepare(struct clk_hw *hw)
{
	ad9523_set_clk_attr(hw, IIO_CHAN_INFO_RAW, 0);
}

static long ad9523_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *prate)
{
	struct iio_dev *indio_dev = to_ad9523_clk_output(hw)->indio_dev;
	struct ad9523_state *st = iio_priv(indio_dev);
	unsigned long clk, tmp1, tmp2;

	if (!rate)
		return 0;

	switch (to_ad9523_clk_output(hw)->num) {
	case 0 ... 3:
		if (rate == st->vco_out_freq[AD9523_VCXO])
			clk = st->vco_out_freq[AD9523_VCXO];
		else
			clk = st->vco_out_freq[AD9523_VCO1];
		break;
	case 4 ... 9:
		tmp1 = st->vco_out_freq[AD9523_VCO1] / rate;
		tmp2 = st->vco_out_freq[AD9523_VCO2] / rate;
		tmp1 *= rate;
		tmp2 *= rate;
		if (abs(tmp1 - rate) > abs(tmp2 - rate))
			clk = st->vco_out_freq[AD9523_VCO2];
		else
			clk = st->vco_out_freq[AD9523_VCO1];
		break;
	default:
		clk = st->vco_out_freq[AD9523_VCO1];
		/* Ch 10..14: No action required, return success */
	}

		tmp1 = DIV_ROUND_CLOSEST(clk, rate);
		tmp1 = clamp(tmp1, 1UL, 1024UL);

	return clk / tmp1;
}

static int ad9523_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long prate)
{
	return ad9523_set_clk_attr(hw, IIO_CHAN_INFO_FREQUENCY, rate);
}

static const struct clk_ops ad9523_clk_ops = {
	.recalc_rate = ad9523_clk_recalc_rate,
	.is_enabled = ad9523_clk_is_enabled,
	.prepare = ad9523_clk_prepare,
	.unprepare = ad9523_clk_unprepare,
	.set_rate = ad9523_clk_set_rate,
	.round_rate = ad9523_clk_round_rate,
};

static struct clk *ad9523_clk_register(struct iio_dev *indio_dev, unsigned num,
				bool is_enabled)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	struct clk_init_data init;
	struct ad9523_outputs *output = &st->output[num];
	struct clk *clk;
	char name[SPI_NAME_SIZE + 8];

	sprintf(name, "%s_out%d", indio_dev->name, num);

	init.name = name;
	init.ops = &ad9523_clk_ops;

	init.num_parents = 0;
	init.flags = 0;
	output->hw.init = &init;
	output->indio_dev = indio_dev;
	output->num = num;
	output->is_enabled = is_enabled;

	/* register the clock */
	clk = clk_register(&st->spi->dev, &output->hw);
	st->clk_data.clks[num] = clk;

	return clk;
}

static int ad9523_setup(struct iio_dev *indio_dev)
{
	struct ad9523_state *st = iio_priv(indio_dev);
	struct ad9523_platform_data *pdata = st->pdata;
	struct ad9523_channel_spec *chan;
	unsigned long active_mask = 0;
	int ret, i;

	ret = ad9523_write(indio_dev, AD9523_SERIAL_PORT_CONFIG,
			   AD9523_SER_CONF_SOFT_RESET |
			  ((st->spi->mode & SPI_3WIRE || pdata->spi3wire) ? 0 :
			  AD9523_SER_CONF_SDO_ACTIVE));
	if (ret < 0)
		return ret;


	ret = ad9523_write(indio_dev, AD9523_READBACK_CTRL,
			  AD9523_READBACK_CTRL_READ_BUFFERED);
	if (ret < 0)
		return ret;

	ret = ad9523_io_update(indio_dev);
	if (ret < 0)
		return ret;

	i = ad9523_read(indio_dev, AD9523_EEPROM_CUSTOMER_VERSION_ID);
	if (i < 0)
		return i;

	ret = ad9523_write(indio_dev, AD9523_EEPROM_CUSTOMER_VERSION_ID, 0xAD95);
	if (ret < 0)
		return ret;

	ret = ad9523_read(indio_dev, AD9523_EEPROM_CUSTOMER_VERSION_ID);
	if (ret < 0)
		return ret;

	if (ret != 0xAD95) {
		dev_err(&indio_dev->dev, "SPI Read Verify failed (0x%X)\n", ret);
		return -EIO;
	}

	ret = ad9523_write(indio_dev, AD9523_EEPROM_CUSTOMER_VERSION_ID, i);
	if (ret < 0)
		return ret;


	/*
	 * PLL1 Setup
	 */
	ret = ad9523_write(indio_dev, AD9523_PLL1_REF_A_DIVIDER,
		pdata->refa_r_div);
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_REF_B_DIVIDER,
		pdata->refb_r_div);
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_FEEDBACK_DIVIDER,
		pdata->pll1_feedback_div);
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_CHARGE_PUMP_CTRL,
		AD_IFE(pll1_bypass_en, AD9523_PLL1_CHARGE_PUMP_TRISTATE,
		AD9523_PLL1_CHARGE_PUMP_CURRENT_nA(pdata->
			pll1_charge_pump_current_nA) |
		AD9523_PLL1_CHARGE_PUMP_MODE_NORMAL |
		AD9523_PLL1_BACKLASH_PW_MIN));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_INPUT_RECEIVERS_CTRL,
		AD_IFE(pll1_bypass_en, AD9523_PLL1_REFA_REFB_PWR_CTRL_EN |
		AD_IF(osc_in_diff_en, AD9523_PLL1_OSC_IN_DIFF_EN) |
		AD_IF(osc_in_cmos_neg_inp_en, AD9523_PLL1_OSC_IN_CMOS_NEG_INP_EN),
		AD_IF(refa_diff_rcv_en, AD9523_PLL1_REFA_RCV_EN) |
		AD_IF(refb_diff_rcv_en, AD9523_PLL1_REFB_RCV_EN) |
		AD_IF(osc_in_diff_en, AD9523_PLL1_OSC_IN_DIFF_EN) |
		AD_IF(osc_in_cmos_neg_inp_en,
		      AD9523_PLL1_OSC_IN_CMOS_NEG_INP_EN) |
		AD_IF(refa_diff_rcv_en, AD9523_PLL1_REFA_DIFF_RCV_EN) |
		AD_IF(refb_diff_rcv_en, AD9523_PLL1_REFB_DIFF_RCV_EN)));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_REF_CTRL,
		AD_IFE(pll1_bypass_en, AD9523_PLL1_BYPASS_FEEDBACK_DIV_EN |
		AD9523_PLL1_ZERO_DELAY_MODE_INT,
		AD_IF(zd_in_diff_en, AD9523_PLL1_ZD_IN_DIFF_EN) |
		AD_IF(zd_in_cmos_neg_inp_en,
		      AD9523_PLL1_ZD_IN_CMOS_NEG_INP_EN) |
		AD_IF(zero_delay_mode_internal_en,
		      AD9523_PLL1_ZERO_DELAY_MODE_INT) |
		AD_IF(osc_in_feedback_en, AD9523_PLL1_OSC_IN_PLL_FEEDBACK_EN) |
		AD_IF(refa_cmos_neg_inp_en, AD9523_PLL1_REFA_CMOS_NEG_INP_EN) |
		AD_IF(refb_cmos_neg_inp_en, AD9523_PLL1_REFB_CMOS_NEG_INP_EN)));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_MISC_CTRL,
		AD9523_PLL1_REFB_INDEP_DIV_CTRL_EN |
		AD9523_PLL1_REF_MODE(pdata->ref_mode));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL1_LOOP_FILTER_CTRL,
		AD9523_PLL1_LOOP_FILTER_RZERO(pdata->pll1_loop_filter_rzero));
	if (ret < 0)
		return ret;
	/*
	 * PLL2 Setup
	 */

	ret = ad9523_write(indio_dev, AD9523_PLL2_CHARGE_PUMP,
		AD9523_PLL2_CHARGE_PUMP_CURRENT_nA(pdata->
			pll2_charge_pump_current_nA));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL2_FEEDBACK_DIVIDER_AB,
		AD9523_PLL2_FB_NDIV_A_CNT(pdata->pll2_ndiv_a_cnt) |
		AD9523_PLL2_FB_NDIV_B_CNT(pdata->pll2_ndiv_b_cnt));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL2_CTRL,
		AD9523_PLL2_CHARGE_PUMP_MODE_NORMAL |
		AD9523_PLL2_BACKLASH_CTRL_EN |
		AD_IF(pll2_freq_doubler_en, AD9523_PLL2_FREQ_DOUBLER_EN));
	if (ret < 0)
		return ret;

	st->vco_freq = div_u64((unsigned long long)pdata->vcxo_freq *
			       (pdata->pll2_freq_doubler_en ? 2 : 1) *
			       AD9523_PLL2_FB_NDIV(pdata->pll2_ndiv_a_cnt,
						   pdata->pll2_ndiv_b_cnt),
			       pdata->pll2_r2_div);

	ret = ad9523_write(indio_dev, AD9523_PLL2_VCO_CTRL,
		AD9523_PLL2_VCO_CALIBRATE);
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL2_VCO_DIVIDER,
		AD9523_PLL2_VCO_DIV_M1(pdata->pll2_vco_div_m1) |
		AD9523_PLL2_VCO_DIV_M2(pdata->pll2_vco_div_m2) |
		AD_IFE(pll2_vco_div_m1, 0,
		       AD9523_PLL2_VCO_DIV_M1_PWR_DOWN_EN) |
		AD_IFE(pll2_vco_div_m2, 0,
		       AD9523_PLL2_VCO_DIV_M2_PWR_DOWN_EN));
	if (ret < 0)
		return ret;

	if (pdata->pll2_vco_div_m1)
		st->vco_out_freq[AD9523_VCO1] =
			st->vco_freq / pdata->pll2_vco_div_m1;

	if (pdata->pll2_vco_div_m2)
		st->vco_out_freq[AD9523_VCO2] =
			st->vco_freq / pdata->pll2_vco_div_m2;

	st->vco_out_freq[AD9523_VCXO] = pdata->vcxo_freq;

	ret = ad9523_write(indio_dev, AD9523_PLL2_R2_DIVIDER,
		AD9523_PLL2_R2_DIVIDER_VAL(pdata->pll2_r2_div));
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_PLL2_LOOP_FILTER_CTRL,
		AD9523_PLL2_LOOP_FILTER_CPOLE1(pdata->cpole1) |
		AD9523_PLL2_LOOP_FILTER_RZERO(pdata->rzero) |
		AD9523_PLL2_LOOP_FILTER_RPOLE2(pdata->rpole2) |
		AD_IF(rzero_bypass_en,
		      AD9523_PLL2_LOOP_FILTER_RZERO_BYPASS_EN));
	if (ret < 0)
		return ret;

	st->clk_data.clks = st->clks;
	st->clk_data.clk_num = AD9523_NUM_CHAN;

	for (i = 0; i < pdata->num_channels; i++) {
		chan = &pdata->channels[i];
		if (chan->channel_num < AD9523_NUM_CHAN) {
			__set_bit(chan->channel_num, &active_mask);
			ret = ad9523_write(indio_dev,
				AD9523_CHANNEL_CLOCK_DIST(chan->channel_num),
				AD9523_CLK_DIST_DRIVER_MODE(chan->driver_mode) |
				AD9523_CLK_DIST_DIV(chan->channel_divider) |
				AD9523_CLK_DIST_DIV_PHASE(chan->divider_phase) |
				(chan->sync_ignore_en ?
					AD9523_CLK_DIST_IGNORE_SYNC_EN : 0) |
				(chan->divider_output_invert_en ?
					AD9523_CLK_DIST_INV_DIV_OUTPUT_EN : 0) |
				(chan->low_power_mode_en ?
					AD9523_CLK_DIST_LOW_PWR_MODE_EN : 0) |
				(chan->output_dis ?
					AD9523_CLK_DIST_PWR_DOWN_EN : 0));
			if (ret < 0)
				return ret;

			ret = ad9523_vco_out_map(indio_dev, chan->channel_num,
					   chan->use_alt_clock_src);
			if (ret < 0)
				return ret;

			st->ad9523_channels[i].type = IIO_ALTVOLTAGE;
			st->ad9523_channels[i].output = 1;
			st->ad9523_channels[i].indexed = 1;
			st->ad9523_channels[i].channel = chan->channel_num;
			st->ad9523_channels[i].extend_name =
				chan->extended_name;
			st->ad9523_channels[i].info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PHASE) |
				BIT(IIO_CHAN_INFO_FREQUENCY);
		}
	}

	for_each_clear_bit(i, &active_mask, AD9523_NUM_CHAN) {
		ret = ad9523_write(indio_dev,
			     AD9523_CHANNEL_CLOCK_DIST(i),
			     AD9523_CLK_DIST_DRIVER_MODE(TRISTATE) |
			     AD9523_CLK_DIST_PWR_DOWN_EN);
		if (ret < 0)
			return ret;
	}

	ret = ad9523_write(indio_dev, AD9523_POWER_DOWN_CTRL, 0);
	if (ret < 0)
		return ret;

	ret = ad9523_write(indio_dev, AD9523_STATUS_SIGNALS,
			   AD9523_STATUS_MONITOR_01_PLL12_LOCKED);
	if (ret < 0)
		return ret;

	ret = ad9523_io_update(indio_dev);
	if (ret < 0)
		return ret;

	ret = ad9523_sync(indio_dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < pdata->num_channels; i++) {
		struct clk *clk;

		chan = &pdata->channels[i];
		if (chan->channel_num >= AD9523_NUM_CHAN)
			continue;

		clk = ad9523_clk_register(indio_dev, chan->channel_num,
					  !chan->output_dis);
		if (IS_ERR(clk))
			return PTR_ERR(clk);
	}

	of_clk_add_provider(st->spi->dev.of_node,
			    of_clk_src_onecell_get, &st->clk_data);

	return 0;
}

#ifdef CONFIG_OF
static struct ad9523_platform_data *ad9523_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node, *chan_np;
	struct ad9523_platform_data *pdata;
	struct ad9523_channel_spec *chan;
	unsigned int tmp, cnt = 0;
	const char *str;
	u32 m1_freq, m2_freq;
	int ret;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->spi3wire = of_property_read_bool(np, "adi,spi-3wire-enable");

	tmp = 0;
	of_property_read_u32(np, "adi,vcxo-freq", &tmp);
	pdata->vcxo_freq = tmp;

	/* Differential/ Single-Ended Input Configuration */
	pdata->refa_diff_rcv_en = of_property_read_bool(np, "adi,refa-diff-rcv-enable");
	pdata->refb_diff_rcv_en = of_property_read_bool(np, "adi,refb-diff-rcv-enable");
	pdata->zd_in_diff_en = of_property_read_bool(np, "adi,zd-in-diff-enable");
	pdata->osc_in_diff_en = of_property_read_bool(np, "adi,osc-in-diff-enable");

	/*
	 * Valid if differential input disabled
	 * if false defaults to pos input
	 */
	pdata->refa_cmos_neg_inp_en =
		of_property_read_bool(np, "adi,refa-cmos-neg-inp-enable");
	pdata->refb_cmos_neg_inp_en =
		of_property_read_bool(np, "adi,refb-cmos-neg-inp-enable");
	pdata->zd_in_cmos_neg_inp_en =
		of_property_read_bool(np, "adi,zd-in-cmos-neg-inp-enable");
	pdata->osc_in_cmos_neg_inp_en =
		of_property_read_bool(np, "adi,osc-in-cmos-neg-inp-enable");

	/* PLL1 Setting */
	tmp = 1;
	of_property_read_u32(np, "adi,refa-r-div", &tmp);
	pdata->refa_r_div = tmp;
	tmp = 1;
	of_property_read_u32(np, "adi,refb-r-div", &tmp);
	pdata->refb_r_div = tmp;
	of_property_read_u32(np, "adi,pll1-feedback-div", &tmp);
	pdata->pll1_feedback_div = tmp;
	of_property_read_u32(np, "adi,pll1-charge-pump-current-nA", &tmp);
	pdata->pll1_charge_pump_current_nA = tmp;
	of_property_read_u32(np, "adi,pll1-loopfilter-rzero", &tmp);
	pdata->pll1_loop_filter_rzero = tmp;

	pdata->zero_delay_mode_internal_en =
		of_property_read_bool(np, "adi,zero-delay-mode-internal-enable");
	pdata->osc_in_feedback_en =
		of_property_read_bool(np, "adi,osc-in-feedback-enable");

	pdata->pll1_bypass_en = of_property_read_bool(np, "adi,pll1-bypass-enable");

		/* Reference */
	of_property_read_u32(np, "adi,ref-mode", &tmp);
	pdata->ref_mode = tmp;

	/* PLL2 Setting */
	of_property_read_u32(np, "adi,pll2-charge-pump-current-nA",
			     &pdata->pll2_charge_pump_current_nA);

	m1_freq = m2_freq = 0;
	of_property_read_u32(np, "adi,pll2-m1-freq", &m1_freq);
	of_property_read_u32(np, "adi,pll2-m2-freq", &m2_freq);

	if (m1_freq == 0 && m2_freq == 0) {
		of_property_read_u32(np, "adi,pll2-ndiv-a-cnt", &tmp);
		pdata->pll2_ndiv_a_cnt = tmp;
		of_property_read_u32(np, "adi,pll2-ndiv-b-cnt", &tmp);
		pdata->pll2_ndiv_b_cnt = tmp;

		pdata->pll2_freq_doubler_en =
			of_property_read_bool(np, "adi,pll2-freq-doubler-enable");

		tmp = 1;
		of_property_read_u32(np, "adi,pll2-r2-div", &tmp);
		pdata->pll2_r2_div = tmp;
		tmp = 3;
		of_property_read_u32(np, "adi,pll2-vco-diff-m1", &tmp);
		of_property_read_u32(np, "adi,pll2-vco-div-m1", &tmp);
		pdata->pll2_vco_div_m1 = tmp;
		tmp = 3;
		of_property_read_u32(np, "adi,pll2-vco-diff-m2", &tmp);
		of_property_read_u32(np, "adi,pll2-vco-div-m2", &tmp);
		pdata->pll2_vco_div_m2 = tmp;
	} else {
		ad9523_calc_dividers(pdata->vcxo_freq, m1_freq, m2_freq, pdata);
	}

	if (pdata->pll2_ndiv_b_cnt < 3 || pdata->pll2_ndiv_b_cnt > 63) {
		dev_err(dev, "PLL2 B divider must be in the range 3-63\n");
		return ERR_PTR(-EINVAL);
	}

	switch (pdata->pll2_ndiv_b_cnt) {
	case 3:
		if (pdata->pll2_ndiv_a_cnt > 0) {
			dev_err(dev, "When PLL2 B counter == 3 A counter must be == 0\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	case 4:
		if (pdata->pll2_ndiv_a_cnt > 1) {
			dev_err(dev, "When PLL2 B counter == 4 A counter must be <= 1\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	case 5:
	case 6:
		if (pdata->pll2_ndiv_a_cnt > 2) {
			dev_err(dev, "When PLL2 B counter == %d A counter must be <= 2\n",
				pdata->pll2_ndiv_b_cnt);
			return ERR_PTR(-EINVAL);
		}
		break;
	default:
		if (pdata->pll2_ndiv_a_cnt > 3) {
			dev_err(dev, "A counter must be <= 3\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	}

	if (pdata->pll2_r2_div < 1 || pdata->pll2_r2_div > 31) {
		dev_err(dev, "PLL2 R2 divider must be in the range of 1-31\n");
		return ERR_PTR(-EINVAL);
	}

	if (pdata->pll2_vco_div_m1 < 3 || pdata->pll2_vco_div_m1 > 5) {
		dev_err(dev, "PLL2 M1 divider must be in the range of 3-5\n");
		return ERR_PTR(-EINVAL);
	}

	if (pdata->pll2_vco_div_m2 < 3 || pdata->pll2_vco_div_m2 > 5) {
		dev_err(dev, "PLL2 M2 divider must be in the range of 3-5\n");
		return ERR_PTR(-EINVAL);
	}

	/* Loop Filter PLL2 */

	of_property_read_u32(np, "adi,rpole2", &tmp);
	pdata->rpole2 = tmp;
	of_property_read_u32(np, "adi,rzero", &tmp);
	pdata->rzero = tmp;
	of_property_read_u32(np, "adi,cpole1", &tmp);
	pdata->cpole1 = tmp;

	pdata->rzero_bypass_en = of_property_read_bool(np, "adi,rzero-bypass-enable");

	/* Output Channel Configuration */

	strncpy(&pdata->name[0], np->name, SPI_NAME_SIZE - 1);

	for_each_child_of_node(np, chan_np)
		cnt++;

	pdata->num_channels = cnt;
	pdata->channels = devm_kzalloc(dev, sizeof(*chan) * cnt, GFP_KERNEL);
	if (!pdata->channels)
		return ERR_PTR(-ENOMEM);

	cnt = 0;
	for_each_child_of_node(np, chan_np) {
		of_property_read_u32(chan_np, "reg",
				     &pdata->channels[cnt].channel_num);
		pdata->channels[cnt].divider_output_invert_en =
			of_property_read_bool(chan_np, "adi,divider-output-invert-enable");
		pdata->channels[cnt].sync_ignore_en =
			of_property_read_bool(chan_np, "adi,sync-ignore-enable");
		pdata->channels[cnt].low_power_mode_en =
			of_property_read_bool(chan_np, "adi,low-power-mode-enable");
		pdata->channels[cnt].use_alt_clock_src =
			of_property_read_bool(chan_np, "adi,use-alt-clock-src");
		pdata->channels[cnt].output_dis =
			of_property_read_bool(chan_np, "adi,output-dis");

		of_property_read_u32(chan_np, "adi,driver-mode", &tmp);
		pdata->channels[cnt].driver_mode = tmp;
		of_property_read_u32(chan_np, "adi,divider-phase", &tmp);
		pdata->channels[cnt].divider_phase = tmp;
		of_property_read_u32(chan_np, "adi,channel-divider", &tmp);
		pdata->channels[cnt].channel_divider = tmp;
		ret = of_property_read_string(chan_np, "adi,extended-name", &str);
		if (ret >= 0)
			strscpy(pdata->channels[cnt].extended_name,
				str, sizeof(pdata->channels[cnt].extended_name));

		cnt++;
	}

	return pdata;
}
#else
static
struct ad9523_platform_data *ad9523_parse_dt(struct device *dev)
{
	return NULL;
}
#endif

static int ad9523_probe(struct spi_device *spi)
{
	struct ad9523_platform_data *pdata;
	struct iio_dev *indio_dev;
	struct ad9523_state *st;
	int ret;

	if (spi->dev.of_node) {
		pdata = ad9523_parse_dt(&spi->dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	} else {
		pdata = spi->dev.platform_data;
	}

	if (!pdata) {
		dev_err(&spi->dev, "no platform data?\n");
		return -EINVAL;
	}

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	mutex_init(&st->lock);

	ret = devm_regulator_get_enable(&spi->dev, "vcc");
	if (ret)
		return ret;

	st->pwrdown_gpio = devm_gpiod_get_optional(&spi->dev, "powerdown",
		GPIOD_OUT_HIGH);
	if (IS_ERR(st->pwrdown_gpio))
		return PTR_ERR(st->pwrdown_gpio);

	st->reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset",
		GPIOD_OUT_LOW);
	if (IS_ERR(st->reset_gpio))
		return PTR_ERR(st->reset_gpio);

	if (st->reset_gpio) {
		udelay(1);
		gpiod_direction_output(st->reset_gpio, 1);
	}

	mdelay(10);

	st->sync_gpio = devm_gpiod_get_optional(&spi->dev, "sync",
		GPIOD_OUT_HIGH);
	if (IS_ERR(st->sync_gpio))
		return PTR_ERR(st->sync_gpio);

	spi_set_drvdata(spi, indio_dev);
	st->spi = spi;
	st->pdata = pdata;

	indio_dev->name = (pdata->name[0] != 0) ? pdata->name :
			  spi_get_device_id(spi)->name;
	indio_dev->info = &ad9523_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = st->ad9523_channels;
	indio_dev->num_channels = pdata->num_channels;

	ret = ad9523_setup(indio_dev);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct spi_device_id ad9523_id[] = {
	{"ad9523-1", 9523},
	{}
};
MODULE_DEVICE_TABLE(spi, ad9523_id);

static struct spi_driver ad9523_driver = {
	.driver = {
		.name	= "ad9523",
	},
	.probe		= ad9523_probe,
	.id_table	= ad9523_id,
};
module_spi_driver(ad9523_driver);

MODULE_AUTHOR("Michael Hennerich <michael.hennerich@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD9523 CLOCKDIST/PLL");
MODULE_LICENSE("GPL v2");
