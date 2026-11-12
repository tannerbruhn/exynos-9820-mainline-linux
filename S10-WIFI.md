# Wi-Fi for the Galaxy S10 on mainline Linux

Working Wi-Fi for the **Samsung Galaxy S10 (SM-G973F, `beyond1lte`, Exynos 9820)** on the
mainline Linux port that postmarketOS / Nura uses
([chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux)).
Until now the port had no Wi-Fi.

> **This code was written by AI** and tested on real hardware by a human: one SM-G973F, on one
> home network. It is not reviewed by the upstream maintainers. Use it at your own risk.

> **⚠️ Known issue (affects the stock port kernel too): zram swap crashes this kernel.**
> On the 7.2.0-rc2 kernel used by this port, the zram swap that Nura enables by default
> (`postmarketos-zram-swap`, with `vm.swappiness=180`) oopses in `zs_malloc` under memory
> pressure — e.g. large downloads filling the page cache, or running an LLM. Symptoms: random
> process crashes, corrupted files, then a hard hang. Reproduced on the stock pmOS kernel as
> well, so it is not caused by the Wi-Fi changes. Fix — disable zram swap:
> ```sh
> sudo swapoff -a
> sudo systemctl mask postmarketos-zram-swap.service dev-zram0.swap
> ```
> With zram disabled, the same workload ran clean. (An earlier version of this note blamed the
> Wi-Fi receive path; that was wrong.)

## What works

| | Status |
|---|---|
| PCIe link to the Wi-Fi chip (Broadcom BCM4375) | Working: Gen1 x1, legacy INTx interrupts |
| WPA2-PSK on 5 GHz | Working: about 150–200 Mbit/s in 2–3 minute download tests |
| WPA2-PSK on 2.4 GHz | Working: slow on a congested channel in testing (single-digit Mbit/s); not compared against stock Android |
| Connects automatically at boot | Working |
| Unloading and reloading the driver | Working |
| Wi-Fi Direct / P2P interface | Not working (creating the P2P device times out; normal Wi-Fi is unaffected) |
| WPA3 / SAE, access-point mode, monitor mode | Untested |
| Bluetooth | Not part of this work |

## What changed

Kernel, branch [`s10-wifi`](../../tree/s10-wifi) (8 code commits on top of the port's `62fdd500d8ca`, plus this file):

- **New PCIe host driver** `pcie-exynos9820` for the Wi-Fi root port, with INTx demultiplexing.
  The SoC's SysMMU is left in bypass.
- **New PHY driver** `phy-exynos9820-pcie` for the channel-0 PCIe PHY.
- **Device tree bindings** for both.
- **brcmfmac changes:**
  - BCM4375 rev 5 support (chip ID, RAM base, firmware names, random seed block)
  - PCIe IPC negotiation (`host_cap`), without which received data never reaches the driver
  - receive buffers sized as the firmware requests
  - D2H sync checks
  - a watchdog reset fix so the driver can be reloaded
  - the `roamoff` parameter applied early

U-Boot (this port boots through U-Boot, which supplies the device tree), branch
[`s10-wifi`](https://github.com/tannerbruhn/u-boot/tree/s10-wifi): adds the Wi-Fi nodes for the
PCIe controller, PHY, power-enable regulator (GPG0-5), PERST/CLKREQ, and the host-wake pin.

## Requirements

- An SM-G973F already running the postmarketOS / Nura **edge** port, kernel `7.2.0-rc2`. That
  means U-Boot in the boot partition and the OS on userdata, as described on the device's wiki
  page.
- The **Samsung vendor partition must still be intact.** The Wi-Fi firmware is proprietary and
  is **not** distributed here. The installer copies it from your own phone's vendor partition
  (read-only).
- USB networking or a keyboard, so you can run commands on the phone.

## Install

1. Download the release tarball and copy it to the phone. Then, on the phone:
   ```sh
   tar xf s10-wifi-*.tar.gz && cd s10-wifi-*/
   sudo ./s10-wifi-install.sh
   sudo reboot
   ```
   The installer does four things:
   - copies the firmware from the vendor partition
   - backs up your boot partition to `/root/`, then writes U-Boot with the Wi-Fi device tree
   - installs the kernel and modules as a **one-time trial boot entry**
   - sets `brcmfmac roamoff=1`

   Your stock kernel entry is not touched. If the trial boot goes wrong, reboot again and the
   stock kernel comes back.
2. After a good boot (`ip link show wlan0` lists the interface), make it permanent and connect:
   ```sh
   sudo ./s10-wifi-install.sh --confirm
   sudo nmcli --ask dev wifi connect "YOUR_SSID"
   ```
3. Optional, for routers with the same network name on 2.4 and 5 GHz:
   ```sh
   sudo ./s10-wifi-twoband.sh "YOUR_SSID"
   ```
   This makes 5 GHz preferred and keeps 2.4 GHz as a fallback. Without it, the phone tends to
   pick 2.4 GHz because that band often reports a slightly stronger signal, even when 5 GHz is
   many times faster.

To undo: `sudo ./s10-wifi-install.sh --uninstall`. The U-Boot with the extra device-tree nodes
is harmless for the stock kernel. To restore the old boot partition exactly:
`sudo dd if=/root/boot-partition-backup-*.img of=/dev/disk/by-partlabel/boot bs=1M`.

## Known issues

- **USB networking may not come back after a reboot** until the cable is unplugged and plugged
  back in. This happens with or without this work. Once Wi-Fi is set up, SSH over Wi-Fi works
  instead.
- **The first 5 GHz connection after a boot occasionally fails its WPA2 handshake.** The next
  automatic attempt succeeds.
- **Calibration file:** two radio module vendors shipped in the S10. The installer uses the
  Samsung module's calibration file (`nvram.txt_CS01_semco_b1`) by default. If Wi-Fi scans but
  performs badly, try the Murata one:
  `sudo NVRAM=nvram.txt_1rh_es43_b1 ./s10-wifi-install.sh`.
- **Firmware roaming is off** (`roamoff=1`); NetworkManager/wpa_supplicant roam instead. The
  firmware's built-in roaming fights NetworkManager when a profile is locked to one access
  point.

## Building from source

Build the kernel at branch `s10-wifi` with the port's `config-postmarketos-exynos9820.aarch64`,
merged with `s10-wifi.config` from the release:

```sh
scripts/kconfig/merge_config.sh -m .config s10-wifi.config
```

Use clang/LLVM, `LLVM=1`. The kernel release string must stay `7.2.0-rc2`, so that the stock
modules still load.

Build U-Boot at branch `s10-wifi` with `make exynos9820_defconfig && make`, then package it with
AOSP `mkbootimg --kernel u-boot.bin`.

## Credits

Built on the Exynos 9820 mainline port by chiffathefox (kernel and U-Boot) and the
postmarketOS / Nura device port. Hardware details were cross-checked against Samsung's GPL
kernel release (`bcmdhd_101_16`, `pcie-exynos-host`).

Code: GPL-2.0, as part of the kernel and U-Boot. The firmware is not included and remains
Samsung's / Broadcom's.
