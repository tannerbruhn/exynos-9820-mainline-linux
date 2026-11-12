// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos9820 channel-0 PHY; calibration from Samsung pci-exynos9820_cal.c.
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct exynos9820_pcie_phy {
	void __iomem *pma;
	void __iomem *pcs;
	struct regmap *pmu;
	struct regmap *sysreg;
};

static const u8 exynos9820_cmn[] = {
	0x01, 0xe1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0c,
	0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xe3, 0xfc,
	0xd8, 0x05, 0xe6, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x60, 0x11, 0x00, 0xa0, 0x05, 0x04, 0x18, 0x88,
	0xc0, 0xff, 0x9b, 0x52, 0x22, 0x30, 0x4f, 0xdc,
	0x40, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x80,
};

static const u8 exynos9820_trsv[] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xc0, 0xff,
	0xff, 0x3f, 0x8c, 0xc8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x6c, 0x66, 0x09, 0x61, 0x42, 0x44,
	0xc6, 0x50, 0x0a, 0x33, 0x58, 0xe7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7e,
	0x00, 0x00, 0x55, 0x15, 0xac, 0xaa, 0x3e, 0x00,
	0x00, 0x00, 0x20, 0x3f, 0x00, 0x03, 0x01, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x05, 0x85, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int exynos9820_phy_init(struct phy *phy)
{
	struct exynos9820_pcie_phy *ep = phy_get_drvdata(phy);
	int ret;

	ret = regmap_update_bits(ep->pmu, 0x71c, BIT(0), BIT(0));
	if (ret)
		return ret;
	/* PCS_HIGH_SPEED, reference clock selection and lane 0 enable. */
	ret = regmap_update_bits(ep->sysreg, 0x1044, BIT(1), BIT(1));
	if (ret)
		goto isolate;
	ret = regmap_update_bits(ep->sysreg, 0x1050,
				 GENMASK(7, 1) | BIT(12), (3 << 2) | BIT(12));
	if (!ret)
		return 0;

isolate:
	regmap_update_bits(ep->pmu, 0x71c, BIT(0), 0);
	return ret;
}

static int exynos9820_phy_exit(struct phy *phy)
{
	struct exynos9820_pcie_phy *ep = phy_get_drvdata(phy);

	return regmap_update_bits(ep->pmu, 0x71c, BIT(0), 0);
}

static int exynos9820_phy_power_on(struct phy *phy)
{
	struct exynos9820_pcie_phy *ep = phy_get_drvdata(phy);
	int i;

	for (i = 0; i < ARRAY_SIZE(exynos9820_cmn); i++)
		writel(exynos9820_cmn[i], ep->pma + i * 4);
	for (i = 0; i < ARRAY_SIZE(exynos9820_trsv); i++)
		writel(exynos9820_trsv[i], ep->pma + (0x30 + i) * 4);

	writel(0x70, ep->pcs + 0xf8);
	writel(0x87, ep->pcs + 0x100);
	writel(0x50, ep->pcs + 0x104);
	writel((readl(ep->pcs + 0xc) & ~BIT(1)) | BIT(4), ep->pcs + 0xc);
	/* Ignore RX electrical idle while the host accesses DBI before link up. */
	writel(readl(ep->pcs + 0xec) | BIT(3), ep->pcs + 0xec);
	return 0;
}

static int exynos9820_phy_calibrate(struct phy *phy)
{
	struct exynos9820_pcie_phy *ep = phy_get_drvdata(phy);
	u32 val = readl(ep->pcs + 0xd0);

	/* CDR reset, after the host has released MAC and PMA resets. */
	writel(val | GENMASK(7, 6), ep->pcs + 0xd0);
	usleep_range(20, 30);
	val &= ~GENMASK(7, 6);
	writel(val | BIT(7), ep->pcs + 0xd0);
	writel(val, ep->pcs + 0xd0);
	writel(readl(ep->pcs + 0xec) & ~BIT(3), ep->pcs + 0xec);
	return 0;
}

static int exynos9820_phy_power_off(struct phy *phy)
{
	struct exynos9820_pcie_phy *ep = phy_get_drvdata(phy);

	writel(0xfe, ep->pma + 0x57 * 4);
	writel(0xc1, ep->pma + 0x20 * 4);
	writel(readl(ep->pcs + 0x100) | BIT(6), ep->pcs + 0x100);
	writel(readl(ep->pcs + 0x104) | BIT(7), ep->pcs + 0x104);
	return 0;
}

static const struct phy_ops exynos9820_phy_ops = {
	.init = exynos9820_phy_init,
	.exit = exynos9820_phy_exit,
	.power_on = exynos9820_phy_power_on,
	.power_off = exynos9820_phy_power_off,
	.calibrate = exynos9820_phy_calibrate,
	.owner = THIS_MODULE,
};

static int exynos9820_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos9820_pcie_phy *ep;
	struct phy_provider *provider;
	struct phy *phy;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;
	ep->pma = devm_platform_ioremap_resource_byname(pdev, "pma");
	if (IS_ERR(ep->pma))
		return PTR_ERR(ep->pma);
	ep->pcs = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(ep->pcs))
		return PTR_ERR(ep->pcs);
	ep->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,pmu-syscon");
	if (IS_ERR(ep->pmu))
		return dev_err_probe(dev, PTR_ERR(ep->pmu), "PMU lookup failed\n");
	ep->sysreg = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,sysreg");
	if (IS_ERR(ep->sysreg))
		return dev_err_probe(dev, PTR_ERR(ep->sysreg), "SYSREG lookup failed\n");
	phy = devm_phy_create(dev, NULL, &exynos9820_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, ep);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id exynos9820_phy_match[] = {
	{ .compatible = "samsung,exynos9820-pcie-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos9820_phy_match);

static struct platform_driver exynos9820_phy_driver = {
	.probe = exynos9820_phy_probe,
	.driver = {
		.name = "exynos9820-pcie-phy",
		.of_match_table = exynos9820_phy_match,
	},
};
module_platform_driver(exynos9820_phy_driver);
MODULE_DESCRIPTION("Exynos9820 channel-0 PCIe PHY");
MODULE_LICENSE("GPL");
