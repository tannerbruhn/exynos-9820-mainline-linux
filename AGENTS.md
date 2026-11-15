# Working on this kernel fork

This fork is mainline Linux support for the Samsung Galaxy S10 (SM-G973F, `beyond1lte`, Exynos 9820) under
postmarketOS: Wi-Fi, the charger and the Mali-G76 GPU. Humans and AI coding agents are both welcome to write code
here. Upstream's process (Documentation/process/, including coding-assistants.rst and generated-content.rst)
applies when sending patches to the kernel mailing lists; follow it then. Install notes: S10-WIFI.md.

## Where the S10 work is
- Wi-Fi PCIe root port: `drivers/pci/controller/dwc/pcie-exynos9820.c`; PHY:
  `drivers/phy/samsung/phy-exynos9820-pcie.c`; bindings under `Documentation/devicetree/bindings/{pci,phy}/`.
- Wi-Fi (BCM4375): `drivers/net/wireless/broadcom/brcm80211/brcmfmac/`.
- Charger: `drivers/power/supply/max77705_charger.c`.
- GPU: `drivers/gpu/drm/panfrost/` (`samsung,exynos9820-mali`).
- The device tree is NOT in this repo: the phone boots through U-Boot, which supplies it
  (https://github.com/tannerbruhn/u-boot, `dts/upstream/src/arm64/exynos/exynos9820*`).

## Build
The port's postmarketOS config merged with `s10-wifi.config` from the release, built with clang:
```sh
cp config-postmarketos-exynos9820.aarch64 .config
scripts/kconfig/merge_config.sh -m .config s10-wifi.config
make LLVM=1 olddefconfig && make LLVM=1 -j"$(nproc)" Image modules
```

## Rules
1. **Test on the phone through a trial boot entry.** Install a new kernel as a one-time systemd-boot entry
   (`name+1.conf`), as the release installer does. Never replace the entry that boots today; the stock
   postmarketOS entry must stay as the fallback. A bad kernel must cost one reboot, nothing more.
2. **Keep the distro's modules loading.** The release ships only `Image` and the Wi-Fi modules; everything else
   comes from postmarketOS's module package for the same version. Before a release, compare exported symbol CRCs
   (`Module.symvers`) with the previous build: none may change.
3. **Memory is the dangerous part.** v1 shipped a corruption bug: Linux used RAM that the bootloader's debug
   logger keeps writing to. Anything touching memory maps, DMA or reserved regions needs a stress test under memory
   pressure (large downloads plus an LLM) before it ships.
4. **Hardware values come from Samsung.** Registers, clocks, voltages, timings and memory regions come from
   Samsung's GPL kernel release or the vendor device tree, never from guesses. Say where each value came from in
   the commit message.
5. **Keep authorship.** Work adapted from others keeps its author (the Panfrost Exynos 9820 change is Gregor
   Sprint's, from omnipha/linux-samsung-s10-exynos9820) and credits the source.
6. **Commits:** upstream style (`subsystem: summary`, a body that explains why, what was tested on which device),
   one logical change per commit. Commit as `tannerbruhn <tannerbruhn@users.noreply.github.com>`, without co-author
   or tool trailers.
