// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9820 Wi-Fi root port, based on Samsung's host-v0 driver. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include "pcie-designware.h"

#define EXYNOS_IRQ_PULSE		0x000
#define EXYNOS_IRQ_EN_PULSE	0x00c
#define EXYNOS_IRQ_EN_LEVEL	0x010
#define EXYNOS_IRQ_EN_SPECIAL	0x014
#define EXYNOS_LTSSM_ENABLE	0x02c
#define EXYNOS_LINK_STATE	0x074
#define EXYNOS_EXIT_L1_MODE	0x0f4
#define EXYNOS_LINKDOWN_RESET	0x1b8
#define EXYNOS_CORE_RESET	0x1d0
#define EXYNOS_PCS_RESET		0x288
#define EXYNOS_PMA_RESET		0x28c
#define EXYNOS_MAC_RESET		0x290
#define EXYNOS_QCH_SEL		0x2c8
#define EXYNOS_INTX_ASSERT	(BIT(0) | BIT(2) | BIT(4) | BIT(6))
#define EXYNOS_AUX_CLK_FREQ	0xb40

struct exynos9820_pcie {
	struct dw_pcie pci;
	struct phy *phy;
	struct gpio_desc *perst;
	struct regulator *vpcie;
	struct clk_bulk_data *clks;
	void __iomem *sysmmu;
	struct regmap *sysreg;
	int num_clks;
	int irq;
	struct irq_domain *intx_domain;
	raw_spinlock_t irq_lock;
};

static struct exynos9820_pcie *to_exynos9820(struct dw_pcie *pci)
{
	return container_of(pci, struct exynos9820_pcie, pci);
}

static void exynos9820_reset_pulse(struct dw_pcie *pci, u32 reg)
{
	writel(1, pci->elbi_base + reg);
	usleep_range(10, 20);
	writel(0, pci->elbi_base + reg);
	usleep_range(10, 20);
	writel(1, pci->elbi_base + reg);
	usleep_range(10, 20);
}

static bool exynos9820_link_up(struct dw_pcie *pci)
{
	u32 state = readl(pci->elbi_base + EXYNOS_LINK_STATE) & 0x1f;

	return state >= 0x0d && state <= 0x14;
}

static int exynos9820_start_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	int ret;

	/* Complete the vendor CDR sequence after DBI setup, before LTSSM. */
	ret = phy_calibrate(ep->phy);
	if (ret)
		return ret;
	writel(1, pci->elbi_base + EXYNOS_LTSSM_ENABLE);
	ret = dw_pcie_wait_for_link(pci);
	if (ret)
		dev_err(pci->dev, "link failed, LTSSM=%#x\n",
			readl(pci->elbi_base + EXYNOS_LINK_STATE));
	return ret;
}

static void exynos9820_stop_link(struct dw_pcie *pci)
{
	writel(0, pci->elbi_base + EXYNOS_LTSSM_ENABLE);
}

static void exynos9820_intx_mask(struct irq_data *d)
{
	struct exynos9820_pcie *ep = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&ep->irq_lock, flags);
	val = readl(ep->pci.elbi_base + EXYNOS_IRQ_EN_PULSE);
	val &= ~BIT(2 * irqd_to_hwirq(d));
	writel(val, ep->pci.elbi_base + EXYNOS_IRQ_EN_PULSE);
	raw_spin_unlock_irqrestore(&ep->irq_lock, flags);
}

static void exynos9820_intx_unmask(struct irq_data *d)
{
	struct exynos9820_pcie *ep = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&ep->irq_lock, flags);
	val = readl(ep->pci.elbi_base + EXYNOS_IRQ_EN_PULSE);
	val |= BIT(2 * irqd_to_hwirq(d));
	writel(val, ep->pci.elbi_base + EXYNOS_IRQ_EN_PULSE);
	raw_spin_unlock_irqrestore(&ep->irq_lock, flags);
}

static const struct irq_chip exynos9820_intx_chip = {
	.name = "exynos9820-INTx",
	.irq_mask = exynos9820_intx_mask,
	.irq_unmask = exynos9820_intx_unmask,
};

static int exynos9820_intx_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &exynos9820_intx_chip, handle_level_irq);
	irq_set_status_flags(irq, IRQ_LEVEL);
	return 0;
}

static const struct irq_domain_ops exynos9820_intx_ops = {
	.map = exynos9820_intx_map,
};

static int exynos9820_map_irq(const struct pci_dev *pdev, u8 slot, u8 pin)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);
	struct exynos9820_pcie *ep = dev_get_drvdata(bridge->dev.parent);
	int irq;

	if (!pin || pin > PCI_NUM_INTX)
		return 0;
	/* Replace the old DT's direct GIC map without a bootloader reflash. */
	irq = irq_create_mapping(ep->intx_domain, pin - 1);
	dev_dbg(ep->pci.dev, "%s INT%c -> IRQ %d, parent IRQ %d\n",
		pci_name(pdev), 'A' + pin - 1, irq, ep->irq);
	return irq;
}

static irqreturn_t exynos9820_irq(int irq, void *data)
{
	struct dw_pcie *pci = data;
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	u32 status, enabled;
	int pin;

	status = readl(pci->elbi_base + EXYNOS_IRQ_PULSE);
	enabled = readl(pci->elbi_base + EXYNOS_IRQ_EN_PULSE);
	status &= enabled & EXYNOS_INTX_ASSERT;

	if (!status)
		return IRQ_NONE;
	/* W1C the captured pulses before dispatch, preserving subsequent asserts. */
	writel(status, pci->elbi_base + EXYNOS_IRQ_PULSE);
	for (pin = 0; pin < PCI_NUM_INTX; pin++) {
		if (status & BIT(2 * pin))
			generic_handle_domain_irq(ep->intx_domain, pin);
	}
	return IRQ_HANDLED;
}

static int exynos9820_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie *ep = to_exynos9820(pci);
	u32 val;
	int ret;

	ret = clk_bulk_prepare_enable(ep->num_clks, ep->clks);
	if (ret)
		return ret;

	/* Use the noncoherent DMA API, matching the absence of dma-coherent. */
	ret = regmap_update_bits(ep->sysreg, 0x700, GENMASK(9, 8), 0);
	if (ret)
		goto disable_clks;

	/* SysMMU full disable is the documented downstream bypass mode. */
	writel(0, ep->sysmmu + 0x4);
	writel(0, ep->sysmmu);
	readl(ep->sysmmu);

	ret = regulator_enable(ep->vpcie);
	if (ret)
		goto disable_clks;
	ret = phy_init(ep->phy);
	if (ret)
		goto disable_regulator;

	writel(0, pci->elbi_base + EXYNOS_LTSSM_ENABLE);
	writel(0, pci->elbi_base + EXYNOS_CORE_RESET);
	usleep_range(20, 30);
	writel(1, pci->elbi_base + EXYNOS_CORE_RESET);
	gpiod_set_value_cansleep(ep->perst, 0);
	usleep_range(18000, 20000);

	val = readl(pci->elbi_base + EXYNOS_EXIT_L1_MODE);
	writel(val | BIT(0) | BIT(4), pci->elbi_base + EXYNOS_EXIT_L1_MODE);
	writel(BIT(1), pci->elbi_base + EXYNOS_LINKDOWN_RESET);
	val = readl(pci->elbi_base + EXYNOS_QCH_SEL);
	writel(val & ~GENMASK(11, 0), pci->elbi_base + EXYNOS_QCH_SEL);

	exynos9820_reset_pulse(pci, EXYNOS_PCS_RESET);
	ret = phy_power_on(ep->phy);
	if (ret)
		goto exit_phy;
	exynos9820_reset_pulse(pci, EXYNOS_MAC_RESET);
	exynos9820_reset_pulse(pci, EXYNOS_PMA_RESET);
	usleep_range(100, 110);

	/* The Samsung integration requires software to populate the RC IDs. */
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, PCI_VENDOR_ID_SAMSUNG);
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, 0xecec);
	dw_pcie_dbi_ro_wr_dis(pci);
	dw_pcie_writel_dbi(pci, EXYNOS_AUX_CLK_FREQ, 26);
	writel(EXYNOS_INTX_ASSERT, pci->elbi_base + EXYNOS_IRQ_PULSE);
	writel(0, pci->elbi_base + EXYNOS_IRQ_EN_PULSE);
	writel(0, pci->elbi_base + EXYNOS_IRQ_EN_LEVEL);
	writel(0, pci->elbi_base + EXYNOS_IRQ_EN_SPECIAL);
	ret = request_irq(ep->irq, exynos9820_irq, IRQF_SHARED,
			  dev_name(pci->dev), pci);
	if (ret) {
		phy_power_off(ep->phy);
		goto exit_phy;
	}
	pp->bridge->map_irq = exynos9820_map_irq;
	return 0;

exit_phy:
	gpiod_set_value_cansleep(ep->perst, 1);
	phy_exit(ep->phy);
disable_regulator:
	regulator_disable(ep->vpcie);
disable_clks:
	clk_bulk_disable_unprepare(ep->num_clks, ep->clks);
	return ret;
}

static void exynos9820_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie *ep = to_exynos9820(pci);

	writel(0, pci->elbi_base + EXYNOS_IRQ_EN_PULSE);
	free_irq(ep->irq, pci);
	exynos9820_stop_link(pci);
	gpiod_set_value_cansleep(ep->perst, 1);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	regulator_disable(ep->vpcie);
	clk_bulk_disable_unprepare(ep->num_clks, ep->clks);
}

static int exynos9820_msi_init(struct dw_pcie_rp *pp)
{
	/* This board and BCM4375 use INTx; do not create an MSI domain. */
	return 0;
}

static const struct dw_pcie_host_ops exynos9820_host_ops = {
	.init = exynos9820_host_init,
	.deinit = exynos9820_host_deinit,
	.msi_init = exynos9820_msi_init,
};

static const struct dw_pcie_ops exynos9820_ops = {
	.link_up = exynos9820_link_up,
	.start_link = exynos9820_start_link,
	.stop_link = exynos9820_stop_link,
};

static void exynos9820_remove_domain(void *data)
{
	struct exynos9820_pcie *ep = data;
	int pin;

	for (pin = 0; pin < PCI_NUM_INTX; pin++)
		irq_dispose_mapping(irq_find_mapping(ep->intx_domain, pin));
	irq_domain_remove(ep->intx_domain);
}

static int exynos9820_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos9820_pcie *ep;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;
	ep->pci.dev = dev;
	ep->pci.ops = &exynos9820_ops;
	ep->pci.pp.ops = &exynos9820_host_ops;
	ep->pci.elbi_base = devm_platform_ioremap_resource_byname(pdev, "elbi");
	if (IS_ERR(ep->pci.elbi_base))
		return PTR_ERR(ep->pci.elbi_base);
	ep->sysmmu = devm_platform_ioremap_resource_byname(pdev, "sysmmu");
	if (IS_ERR(ep->sysmmu))
		return PTR_ERR(ep->sysmmu);
	ep->sysreg = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,sysreg");
	if (IS_ERR(ep->sysreg))
		return dev_err_probe(dev, PTR_ERR(ep->sysreg), "SYSREG unavailable\n");
	ep->phy = devm_phy_get(dev, "pcie");
	if (IS_ERR(ep->phy))
		return dev_err_probe(dev, PTR_ERR(ep->phy), "PHY unavailable\n");
	ep->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ep->perst))
		return dev_err_probe(dev, PTR_ERR(ep->perst), "PERST unavailable\n");
	ep->vpcie = devm_regulator_get(dev, "vpcie");
	if (IS_ERR(ep->vpcie))
		return dev_err_probe(dev, PTR_ERR(ep->vpcie), "Wi-Fi supply unavailable\n");
	ep->num_clks = devm_clk_bulk_get_all(dev, &ep->clks);
	if (ep->num_clks < 0)
		return dev_err_probe(dev, ep->num_clks, "clocks unavailable\n");
	if (!ep->num_clks)
		return -EINVAL;
	ep->irq = platform_get_irq(pdev, 0);
	if (ep->irq < 0)
		return ep->irq;
	raw_spin_lock_init(&ep->irq_lock);
	/* Private domain: PCI map_irq supplies the pin; no new DT node needed. */
	ep->intx_domain = irq_domain_create_linear(NULL, PCI_NUM_INTX,
						   &exynos9820_intx_ops, ep);
	if (!ep->intx_domain)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, exynos9820_remove_domain, ep);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, ep);
	return dw_pcie_host_init(&ep->pci.pp);
}

static const struct of_device_id exynos9820_pcie_match[] = {
	{ .compatible = "samsung,exynos9820-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos9820_pcie_match);

static struct platform_driver exynos9820_pcie_driver = {
	.probe = exynos9820_pcie_probe,
	.driver = {
		.name = "exynos9820-pcie",
		.of_match_table = exynos9820_pcie_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(exynos9820_pcie_driver);
MODULE_DESCRIPTION("Exynos9820 Wi-Fi PCIe host");
MODULE_LICENSE("GPL");
