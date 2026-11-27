# Mainline Linux port (parked)

One goal of discfw is to run on the newest mainline Linux kernel instead of FiiO's 4.4.94. A port to **Linux 7.4** was done on 2026-11-25/26 and reached almost the same features as discfw on the stock kernel. It was **parked** on 2026-11-26 because boots from NAND were not reliable. The work is on the branch **`kernel-74`** of this repository.

The branch is older than `master`: it starts from the first discfw version, so it does not have the current UI, discsys or the battery work. Its own README still links to research notes that are not published; this page replaces them.

## What worked

- **Boot:** FiiO's boot loader starts the stock kernel (mtd1 is never written). `/linuxrc` loads `discjump`, a small module for the stock kernel that loads Linux 7.4 and jumps to it. If 7.4 does not confirm a good boot (watchdog reset), the next boot starts a rescue shell on the stock kernel.
- **Kernel:** kernel.org v7.4-rc4 plus 57 patches in `package/disc-linux/`. Platform code from [datel-os/k1-linux](https://github.com/datel-os/k1-linux) (Creality K1, also X2000).
- **NAND:** a mainline SFC driver (from the HiBy R1 X1600 work) plus XT26G02C support read every partition byte for byte like the stock kernel. UBI/UBIFS user data mounted read-write; writes were tested on a scratch area.
- **Audio:** a new `x2000-as` ASoC driver (AS-DMA → BAIC3) with fractional I2S clocks, and the mainline `cs43130` driver in dual-mono mode. 32 kHz–384 kHz worked; register values equal the stock ones; MPD used 1.5 % CPU for FLAC.
- **Display and input:** new X2000 DPU, MIPI DSI glue, ST77916 panel and PWM backlight drivers; `cst816x` touch; `gpio-keys`.
- **Radios:** Wi-Fi with `brcmfmac` and Cypress firmware from linux-firmware (only the board calibration comes from FiiO); Bluetooth with `hci_bcm` at 3 Mbaud; A2DP to headphones.
- **Power:** both CPU threads, cpufreq 200–1200 MHz, clocks and power rails off when idle, suspend to idle with RTC and Power wake. Battery current: −17.4 mA idle with the screen off and Wi-Fi on, −35 mA playing with the screen on.
- **Fewer blobs:** no FiiO kernel modules and no stock user space. Only the boot loader, the stock kernel (as first stage), the Wi-Fi calibration and the Bluetooth patch are FiiO's.

## Open issues

1. **Black screen on a cold boot from battery.** Power on without USB: the logo, then a black screen with the backlight on. The system runs, but the UI's main loop does not. Plugging in USB brings the UI within seconds. Not diagnosed.
2. Some fixes were never verified on the device: power-hold on battery, SD on repeated cold boots, the rescue shell's power key, rescue only after a watchdog reset.
3. Bad blocks in mtd2 are not mapped (7.4 reads `/dev/mtdblock2` directly); fine only while no bad block sits inside the image.
4. The stock kernel still runs first and sets up the charger, gauge and USB-C chip. To drop it, these need drivers and 7.4 must go into mtd1, which needs a new writer and a tested way back.
5. Not done: long playback and skip stress tests, a suspend policy in user space.

## Build and resume

All inputs are downloaded and checked with SHA-256: Buildroot, the kernel.org v7.4-rc4 snapshot, linux-firmware 20261124 (Cypress files), FiiO's V2.57 update file, and diskOS for flashing.

```
git checkout kernel-74
rm -rf output
make
scripts/disc-flash rootfs
```

`rm -rf output` is needed because the two branches build different packages. Then test issue 1 first. `scripts/kernel-tree` creates a kernel git tree from the patches for development; `scripts/disc-ml` builds a kernel and boots it on the player without flashing (it needs the branch image on NAND).

## What concerns the stock kernel too

- Holding Power for about 1 second wakes the board (RTC). A power-off that runs while Power is still held restarts the board at once, so power off after the release.
- To leave mask-ROM mode: unplug USB, hold Power about 6 s.
- The CPU is two threads of one core. MIPS32r5 kernels fail (`eretnc` is missing): build the kernel for MIPS32r2 with `MIPS_O32_FP64_SUPPORT`.
- The watchdog runs from the RTC's 32768 Hz clock.

## Credits and related work

- [datel-os/k1-linux](https://github.com/datel-os/k1-linux): mainline kernel for the X2000 in the Creality K1 (platform, clocks, interrupts, SMP, UART, MMC, I2C, watchdog, RTC).
- [mgcrea/hiby-r1-linux](https://github.com/mgcrea/hiby-r1-linux): mainline kernel for the HiBy R1 (X1600E) with the same CS43131, CST816 and AP6212 chips, and the SFC driver.
- [goldelico/letux-kernel](https://github.com/goldelico/letux-kernel): `x2000.dtsi` and upstream X2000 branches.
- OpenWrt's XT26G02C SPI NAND patch.
