/*
 * copyright (c) 2013 Freescale Semiconductor, Inc.
 * Freescale IMX AHCI SATA platform driver
 *
 * based on the AHCI SATA platform driver by Jeff Garzik and Anton Vorontsov
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/ahci_platform.h>
#include <linux/of_device.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/libata.h>
#include <linux/busfreq-imx6.h>
#include "ahci.h"

enum {
	HOST_TIMER1MS = 0xe0,			/* Timer 1-ms */
	PORT_PHY_CTL = 0x178,			/* Port0 PHY Control */
	PORT_PHY_CTL_PDDQ_LOC = 0x100000,	/* PORT_PHY_CTL bits */
	/* PORT_PHY_CTL bits */
	PORT_PHY_CTL_CAP_ADR_LOC = 0x10000,
	PORT_PHY_CTL_CAP_DAT_LOC = 0x20000,
	PORT_PHY_CTL_WRITE_LOC = 0x40000,
	PORT_PHY_CTL_READ_LOC = 0x80000,
	/* Port0 PHY Status */
	PORT_PHY_SR = 0x17c,
	/* PORT_PHY_SR */
	PORT_PHY_STAT_DATA_LOC = 0,
	PORT_PHY_STAT_ACK_LOC = 18,

	SATA_PHY_CR_CLOCK_RESET = 0x7F3F,
	SATA_PHY_CR_RESET_EN = 0x0001,
	SATA_PHY_CR_LANE0_OUT_STAT = 0x2003,
	SATA_PHY_CR_LANE0_RX_STABLE = 0x0002,
};

struct imx_ahci_priv {
	struct platform_device *ahci_pdev;
	struct clk *sata_ref_clk;
	struct clk *ahb_clk;
	struct regmap *gpr;
	bool no_device;
	bool first_time;
};

static int ahci_imx_hotplug;
module_param_named(hotplug, ahci_imx_hotplug, int, 0644);
MODULE_PARM_DESC(hotplug, "AHCI IMX hot-plug support (0=Don't support, 1=support)");

static void ahci_imx_error_handler(struct ata_port *ap)
{
	u32 reg_val;
	struct ata_device *dev;
	struct ata_host *host = dev_get_drvdata(ap->dev);
	struct ahci_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->mmio;
	struct imx_ahci_priv *imxpriv = dev_get_drvdata(ap->dev->parent);

	ahci_error_handler(ap);

	if (!(imxpriv->first_time) || ahci_imx_hotplug)
		return;

	imxpriv->first_time = false;

	ata_for_each_dev(dev, &ap->link, ENABLED)
		return;
	/*
	 * Disable link to save power.  An imx ahci port can't be recovered
	 * without full reset once the pddq mode is enabled making it
	 * impossible to use as part of libata LPM.
	 */
	reg_val = readl(mmio + PORT_PHY_CTL);
	writel(reg_val | PORT_PHY_CTL_PDDQ_LOC, mmio + PORT_PHY_CTL);
	regmap_update_bits(imxpriv->gpr, IOMUXC_GPR13,
			IMX6Q_GPR13_SATA_MPLL_CLK_EN,
			!IMX6Q_GPR13_SATA_MPLL_CLK_EN);
	clk_disable_unprepare(imxpriv->sata_ref_clk);
	release_bus_freq(BUS_FREQ_HIGH);
	imxpriv->no_device = true;
}

static struct ata_port_operations ahci_imx_ops = {
	.inherits	= &ahci_platform_ops,
	.error_handler	= ahci_imx_error_handler,
};

static const struct ata_port_info ahci_imx_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_imx_ops,
};

static int write_phy_ctl_ack_polling(u32 data, void __iomem *mmio,
		int max_iterations, u32 exp_val)
{
	u32 i, val;

	writel(data, mmio + PORT_PHY_CTL);

	for (i = 0; i < max_iterations + 1; i++) {
		val = readl(mmio + PORT_PHY_SR);
		val =  (val >> PORT_PHY_STAT_ACK_LOC) & 0x1;
		if (val == exp_val)
			return 0;
		if (i == max_iterations) {
			pr_err("Wait for CR ACK error!\n");
			return 1;
		}
		usleep_range(100, 200);
	}
	return 0;
}

static int sata_phy_cr_addr(u32 addr, void __iomem *mmio)
{
	u32 temp_wr_data;

	/* write addr */
	temp_wr_data = addr;
	writel(temp_wr_data, mmio + PORT_PHY_CTL);

	/* capture addr */
	temp_wr_data |= PORT_PHY_CTL_CAP_ADR_LOC;

	/* wait for ack */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 1))
		return 1;

	/* deassert cap addr */
	temp_wr_data &= 0xffff;

	/* wait for ack de-assetion */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 0))
		return 1;

	return 0;
}

static int sata_phy_cr_write(u32 data, void __iomem *mmio)
{
	u32 temp_wr_data;

	/* write data */
	temp_wr_data = data;
	writel(temp_wr_data, mmio + PORT_PHY_CTL);

	/* capture data */
	temp_wr_data |= PORT_PHY_CTL_CAP_DAT_LOC;

	/* wait for ack */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 1))
		return 1;

	/* deassert cap data */
	temp_wr_data &= 0xffff;

	/* wait for ack de-assetion */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 0))
		return 1;

	/* assert wr signal */
	temp_wr_data |= PORT_PHY_CTL_WRITE_LOC;

	/* wait for ack */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 1))
		return 1;

	/* deassert wr _signal */
	temp_wr_data = 0x0;

	/* wait for ack de-assetion */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 0))
		return 1;

	return 0;
}

static int sata_phy_cr_read(u32 *data, void __iomem *mmio)
{
	u32 temp_rd_data, temp_wr_data;

	/* assert rd signal */
	temp_wr_data = PORT_PHY_CTL_READ_LOC;

	/* wait for ack */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 1))
		return 1;

	/* after got ack return data */
	temp_rd_data = readl(mmio + PORT_PHY_SR);
	*data = (temp_rd_data & 0xffff);

	/* deassert rd _signal */
	temp_wr_data = 0x0 ;

	/* wait for ack de-assetion */
	if (write_phy_ctl_ack_polling(temp_wr_data, mmio, 100, 0))
		return 1;

	return 0;
}

static int imx6q_sata_init(struct device *dev, void __iomem *mmio)
{
	int i, ret = 0;
	unsigned int reg_val;
	struct imx_ahci_priv *imxpriv = dev_get_drvdata(dev->parent);

	imxpriv->gpr =
		syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (IS_ERR(imxpriv->gpr)) {
		dev_err(dev, "failed to find fsl,imx6q-iomux-gpr regmap\n");
		return PTR_ERR(imxpriv->gpr);
	}

	ret = clk_prepare_enable(imxpriv->sata_ref_clk);
	if (ret < 0) {
		dev_err(dev, "prepare-enable sata_ref clock err:%d\n", ret);
		return ret;
	}
	request_bus_freq(BUS_FREQ_HIGH);

	/*
	 * set PHY Paremeters, two steps to configure the GPR13,
	 * one write for rest of parameters, mask of first write
	 * is 0x07fffffd, and the other one write for setting
	 * the mpll_clk_en.
	 */
	regmap_update_bits(imxpriv->gpr, 0x34, IMX6Q_GPR13_SATA_RX_EQ_VAL_MASK
			| IMX6Q_GPR13_SATA_RX_LOS_LVL_MASK
			| IMX6Q_GPR13_SATA_RX_DPLL_MODE_MASK
			| IMX6Q_GPR13_SATA_SPD_MODE_MASK
			| IMX6Q_GPR13_SATA_MPLL_SS_EN
			| IMX6Q_GPR13_SATA_TX_ATTEN_MASK
			| IMX6Q_GPR13_SATA_TX_BOOST_MASK
			| IMX6Q_GPR13_SATA_TX_LVL_MASK
			| IMX6Q_GPR13_SATA_TX_EDGE_RATE
			| IMX6Q_GPR13_SATA_MPLL_CLK_EN
			, IMX6Q_GPR13_SATA_RX_EQ_VAL_3_0_DB
			| IMX6Q_GPR13_SATA_RX_LOS_LVL_SATA2M
			| IMX6Q_GPR13_SATA_RX_DPLL_MODE_2P_4F
			| IMX6Q_GPR13_SATA_SPD_MODE_3P0G
			| IMX6Q_GPR13_SATA_MPLL_SS_EN
			| IMX6Q_GPR13_SATA_TX_ATTEN_9_16
			| IMX6Q_GPR13_SATA_TX_BOOST_3_33_DB
			| IMX6Q_GPR13_SATA_TX_LVL_1_104_V);
	regmap_update_bits(imxpriv->gpr, 0x34, IMX6Q_GPR13_SATA_MPLL_CLK_EN,
			IMX6Q_GPR13_SATA_MPLL_CLK_EN);
	usleep_range(100, 200);

	sata_phy_cr_addr(SATA_PHY_CR_CLOCK_RESET, mmio);
	sata_phy_cr_write(SATA_PHY_CR_RESET_EN, mmio);
	usleep_range(100, 200);
	/* waiting for the rx_pll is stable */
	for (i = 0; i <= 5; i++) {
		sata_phy_cr_addr(SATA_PHY_CR_LANE0_OUT_STAT, mmio);
		sata_phy_cr_read(&ret, mmio);
		if (ret & SATA_PHY_CR_LANE0_RX_STABLE) {
			pr_info("sata phy RX_PLL is stable!\n");
			break;
		} else if (i == 5)
			pr_info("Wating for RX_PLL lock time out\n");
		usleep_range(1000, 2000);
	}
	/*
	 * Configure the HWINIT bits of the HOST_CAP and HOST_PORTS_IMPL,
	 * and IP vendor specific register HOST_TIMER1MS.
	 * Configure CAP_SSS (support stagered spin up).
	 * Implement the port0.
	 * Get the ahb clock rate, and configure the TIMER1MS register.
	 */
	reg_val = readl(mmio + HOST_CAP);
	if (!(reg_val & HOST_CAP_SSS)) {
		reg_val |= HOST_CAP_SSS;
		writel(reg_val, mmio + HOST_CAP);
	}
	reg_val = readl(mmio + HOST_PORTS_IMPL);
	if (!(reg_val & 0x1)) {
		reg_val |= 0x1;
		writel(reg_val, mmio + HOST_PORTS_IMPL);
	}

	reg_val = clk_get_rate(imxpriv->ahb_clk) / 1000;
	writel(reg_val, mmio + HOST_TIMER1MS);

	return 0;
}

static void imx6q_sata_exit(struct device *dev)
{
	struct imx_ahci_priv *imxpriv =  dev_get_drvdata(dev->parent);

	if (!imxpriv->no_device) {
		regmap_update_bits(imxpriv->gpr, 0x34,
				IMX6Q_GPR13_SATA_MPLL_CLK_EN,
				!IMX6Q_GPR13_SATA_MPLL_CLK_EN);
		clk_disable_unprepare(imxpriv->sata_ref_clk);
		release_bus_freq(BUS_FREQ_HIGH);
	}
}

static int imx_ahci_suspend(struct device *dev)
{
	struct imx_ahci_priv *imxpriv =  dev_get_drvdata(dev->parent);

	/*
	 * If no_device is set, The CLKs had been gated off in the
	 * initialization so don't do it again here.
	 */
	if (!imxpriv->no_device) {
		regmap_update_bits(imxpriv->gpr, IOMUXC_GPR13,
				IMX6Q_GPR13_SATA_MPLL_CLK_EN,
				!IMX6Q_GPR13_SATA_MPLL_CLK_EN);
		clk_disable_unprepare(imxpriv->sata_ref_clk);
		release_bus_freq(BUS_FREQ_HIGH);
	}

	return 0;
}

static int imx_ahci_resume(struct device *dev)
{
	struct imx_ahci_priv *imxpriv =  dev_get_drvdata(dev->parent);
	int i, ret;
	struct ata_host *host = dev_get_drvdata(dev);
	struct ahci_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->mmio;

	if (!imxpriv->no_device) {
		ret = clk_prepare_enable(imxpriv->sata_ref_clk);
		if (ret < 0) {
			dev_err(dev, "pre-enable sata_ref clock err:%d\n", ret);
			return ret;
		}
		request_bus_freq(BUS_FREQ_HIGH);

		regmap_update_bits(imxpriv->gpr, IOMUXC_GPR13,
				IMX6Q_GPR13_SATA_MPLL_CLK_EN,
				IMX6Q_GPR13_SATA_MPLL_CLK_EN);
		usleep_range(100, 200);

		sata_phy_cr_addr(SATA_PHY_CR_CLOCK_RESET, mmio);
		sata_phy_cr_write(SATA_PHY_CR_RESET_EN, mmio);
		usleep_range(100, 200);
		/* waiting for the rx_pll is stable */
		for (i = 0; i <= 5; i++) {
			sata_phy_cr_addr(SATA_PHY_CR_LANE0_OUT_STAT, mmio);
			sata_phy_cr_read(&ret, mmio);
			if (ret & SATA_PHY_CR_LANE0_RX_STABLE) {
				pr_info("sata phy rx_pll is stable!\n");
				break;
			} else if (i == 5)
				pr_info("wating for sata rx_pll lock time out\n");
			usleep_range(1000, 2000);
		}
	}

	return 0;
}

static struct ahci_platform_data imx6q_sata_pdata = {
	.init = imx6q_sata_init,
	.exit = imx6q_sata_exit,
	.ata_port_info = &ahci_imx_port_info,
	.suspend = imx_ahci_suspend,
	.resume = imx_ahci_resume,
};

static const struct of_device_id imx_ahci_of_match[] = {
	{ .compatible = "fsl,imx6q-ahci", .data = &imx6q_sata_pdata},
	{},
};
MODULE_DEVICE_TABLE(of, imx_ahci_of_match);

static int imx_ahci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *mem, *irq, res[2];
	const struct of_device_id *of_id;
	const struct ahci_platform_data *pdata = NULL;
	struct imx_ahci_priv *imxpriv;
	struct device *ahci_dev;
	struct platform_device *ahci_pdev;
	int ret;

	imxpriv = devm_kzalloc(dev, sizeof(*imxpriv), GFP_KERNEL);
	if (!imxpriv) {
		dev_err(dev, "can't alloc ahci_host_priv\n");
		return -ENOMEM;
	}

	ahci_pdev = platform_device_alloc("ahci", -1);
	if (!ahci_pdev)
		return -ENODEV;

	ahci_dev = &ahci_pdev->dev;
	ahci_dev->parent = dev;

	imxpriv->no_device = false;
	imxpriv->first_time = true;
	imxpriv->ahb_clk = devm_clk_get(dev, "ahb");
	if (IS_ERR(imxpriv->ahb_clk)) {
		dev_err(dev, "can't get ahb clock.\n");
		ret = PTR_ERR(imxpriv->ahb_clk);
		goto err_out;
	}

	imxpriv->sata_ref_clk = devm_clk_get(dev, "sata_ref");
	if (IS_ERR(imxpriv->sata_ref_clk)) {
		dev_err(dev, "can't get sata_ref clock.\n");
		ret = PTR_ERR(imxpriv->sata_ref_clk);
		goto err_out;
	}

	imxpriv->ahci_pdev = ahci_pdev;
	platform_set_drvdata(pdev, imxpriv);

	of_id = of_match_device(imx_ahci_of_match, dev);
	if (of_id) {
		pdata = of_id->data;
	} else {
		ret = -EINVAL;
		goto err_out;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!mem || !irq) {
		dev_err(dev, "no mmio/irq resource\n");
		ret = -ENOMEM;
		goto err_out;
	}

	res[0] = *mem;
	res[1] = *irq;

	ahci_dev->coherent_dma_mask = DMA_BIT_MASK(32);
	ahci_dev->dma_mask = &ahci_dev->coherent_dma_mask;
	ahci_dev->of_node = dev->of_node;

	ret = platform_device_add_resources(ahci_pdev, res, 2);
	if (ret)
		goto err_out;

	ret = platform_device_add_data(ahci_pdev, pdata, sizeof(*pdata));
	if (ret)
		goto err_out;

	ret = platform_device_add(ahci_pdev);
	if (ret) {
err_out:
		platform_device_put(ahci_pdev);
		return ret;
	}

	return 0;
}

static int imx_ahci_remove(struct platform_device *pdev)
{
	struct imx_ahci_priv *imxpriv = platform_get_drvdata(pdev);
	struct platform_device *ahci_pdev = imxpriv->ahci_pdev;

	platform_device_unregister(ahci_pdev);
	return 0;
}

static struct platform_driver imx_ahci_driver = {
	.probe = imx_ahci_probe,
	.remove = imx_ahci_remove,
	.driver = {
		.name = "ahci-imx",
		.owner = THIS_MODULE,
		.of_match_table = imx_ahci_of_match,
	},
};
module_platform_driver(imx_ahci_driver);

MODULE_DESCRIPTION("Freescale i.MX AHCI SATA platform driver");
MODULE_AUTHOR("Richard Zhu <Hong-Xing.Zhu@freescale.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ahci:imx");
