# Hardware

Facts about the FiiO Snowsky Disc that matter for discfw. They come from the stock V2.57 device tree and kernel, from reads on a running player, and from the [diskOS](https://github.com/b0hemia/diskOS) notes. **Open** marks a point that is not confirmed.

## Parts

| Part | Chip | Bus, address | Stock kernel interface |
|---|---|---|---|
| SoC | Ingenic X2000 | | |
| RAM | Winbond W63AH6NKB LPDDR3, 128 MiB | | 117.5 MiB for Linux |
| Flash | SPI NAND, 256 MiB (XTX XT26G02C on the unit we read) | SFC | `/dev/mtd0`…`7`, `/dev/mtdblock_bbt_ro2` |
| DAC | 2× Cirrus CS43131, one per channel | I2C-3 0x30 and 0x32, 400 kHz | `/dev/cs43131`, `/dev/cs43131c` |
| Charger | SG Micro SGM41513 | I2C-2 0x1a | `/dev/sgm41513` |
| Fuel gauge | CellWise CW2217 | I2C-2 0x64 | `/sys/class/power_supply/cw221X-bat` |
| USB-C controller | Awinic AW35615 (Type-C and PD) | I2C-2 0x22 | `/dev/aw35615`, sysfs |
| Screen | ST77916, 360×360 round, MIPI DSI | DPU + DSI | `/dev/fb0`, `/dev/lcd_st77916` |
| Backlight | PWM1 on PC1 | | `/sys/class/backlight/backlight`, levels 0–40 |
| Touch | Hynitron CST816T | I2C-1 0x15 | `/dev/input/event1`, `/dev/cst816t` |
| Buttons | GPIO | | `/dev/input/event0` (`x2000_key`) |
| Wi-Fi | AP6212 module = Broadcom BCM43438 | SDIO | `wlan0` (`bcmdhd.ko`) |
| Bluetooth | same chip | UART0 (`ttyS0`), 3 Mbaud | `hci0` |
| microSD | | MSC2, 4-bit | `/dev/mmcblk0p1` |
| USB | USB-C, DWC2 OTG | | gadget (`configfs`) |
| Console | UART3 (`ttyS3`), 115200 baud | | kernel console |

## SoC

- Ingenic X2000, XBurst II. Linux sees 2 CPUs; they are two hardware threads of one core. 1.2 GHz.
- FPU with 64-bit registers (FP64) and IEEE 754-2008 NaN (NaN2008). MSA 128-bit SIMD. L1 32 KiB + 32 KiB, L2 512 KiB (16-way). PRId `0x00132000`.
- The CPU runs MIPS32r5 user code. The kernel-only instruction `eretnc` is missing, so a kernel must be built for MIPS32r2.
- All user programs must use the o32 hard-float FP64 NaN2008 ABI. The build checks every ELF file (`board/disc/check-abi.sh`).
- Clocks from a 24 MHz crystal: APLL 1200 MHz (CPU), MPLL 1500 MHz, EPLL 1080 MHz (audio).
- No GPU, no video decoder, no JPEG decoder: all drawing is done by the CPU.
- The stock kernel has no cpufreq, no cpuidle and no thermal zones. It idles at 1.2 GHz. No hardware on X2000 boards scales the voltage.

## Flash layout

| mtd | Name | Size | Content | discfw |
|---|---|---|---|---|
| mtd0 | uboot | 2 MiB | FiiO boot loader (SPL with the LPDDR3 settings) | never written |
| mtd1 | kernel | 8 MiB | FiiO Linux 4.4.94 kernel | never written |
| mtd2 | rootfs | 128 MiB | root file system, squashfs | **the discfw image (96 MiB)** |
| mtd3 | kernel2 | 8 MiB | recovery kernel | never written |
| mtd4 | rootfs2 | 25 MiB | recovery root file system | never written |
| mtd5 | ota | 1 MiB | update state | never written |
| mtd6 | mac | 1 MiB | data for this device | never written; **never erase it** |
| mtd7 | userdata | 83 MiB | UBIFS, mounted at `/usr/data` | discfw state in `/usr/data/discfw` |

- NAND: 2 KiB + 128 B pages, 128 KiB blocks, 2048 blocks, ECC on the chip. FiiO's tables also list a HeYangTek HYF2GQ4U.
- The stock kernel reads mtd2 through `mtdblock_bbt_ro`, which skips bad blocks. The flasher skips bad blocks in the same way, so the image appears in one piece.
- The flasher writes 768 blocks (96 MiB) from the start of mtd2. The discfw squashfs is about 19 MiB; the rest is padding.

## Boot chain

1. **Mask ROM** in the SoC. If Volume Down is held at power-on and USB is connected, it waits for a USB boot (`a108:eaef`). It checks no signatures.
2. **FiiO's boot loader** in mtd0. It reads the update state in mtd5 and loads the kernel from mtd1 (or the recovery kernel from mtd3) to `0x80F00000`, without a checksum or signature check. **Open:** whether a full U-Boot stage runs, or the SPL starts the kernel directly.
3. **Kernel** with the device tree built in and this command line: `console=ttyS3,115200n8 quiet mem=128M@0x0 init=/linuxrc flashtype=nand root=/dev/mtdblock_bbt_ro2 rootfstype=squashfs ro`
4. **`/linuxrc`** from mtd2 (discfw's script), then BusyBox init.

FiiO's update files are signed (ECDSA P-256), so the normal updater does not install a custom image. Their content is encrypted with a fixed key (AES-256-CBC, PBKDF2, password `fo123`), which is how the build unpacks them.

## Mask-ROM mode and the power button

- **Enter:** turn the player off, hold Volume Down, plug in USB. The screen stays black. It works whatever is in the flash.
- **Leave:** with USB connected, Power does nothing. Unplug USB, then hold Power for about 6 seconds (the RTC's long-press reset).
- Holding Power for about 1 second wakes the board from off (RTC wake). A power-off that happens while Power is still held can start the board again at once, so discfw powers off after the button is released.
- After a kernel panic, the stock kernel restarts after 10 seconds.

## Audio

- Path: AS-DMA channel 3 → I2S3 (BAIC3) → both DACs. ALSA device `hw:0,3`, `S32_LE`, 2 channels. The route `LO3_MUX = LI11` must be set (all routes are unused at boot).
- The SoC is the I2S master: BCLK = 64 × sample rate, running only during a stream. Each DAC makes its own master clock from a 22.5792 MHz crystal with its PLL.
- The two CS43131 are in mono mode: one chip per channel, each driving its A/B outputs as + and −. This gives the 4.4 mm balanced output; the 3.5 mm output is single-ended. The stock software detects which plug is in with the SoC's ADC.
- S/PDIF (coaxial) out: the SoC's own transmitter on PC14 shares the 3.5 mm jack; it is usable while the DAC amps are off.
- DAC GPIOs: PA12 reset, PB05 analog mute, PB08 DAC power, PB09 and PB12 output amplifier enable. The stock driver owns them.
- PCM from 32 kHz to 384 kHz. The DACs also support DSD; discfw does not use it.

## Screen and touch

- ST77916 panel, 360×360, round. The stock kernel drives it through the MIPI DSI host in video mode (1 data lane); the display engine (DPU) refreshes it at about 60 Hz. The panel is mounted upside down, so discui rotates its output by 180°.
- `/dev/fb0` is 360×360, 32 bits per pixel (XRGB8888).
- Pins: PB18 panel power, PB25 panel reset, PC10 tearing signal (unused), PB19 panel vendor ID (two vendors, same init sequence).
- Backlight: PWM1 on PC1, levels 0–40.
- Touch: CST816T, IRQ PC21, reset PC22. The kernel holds touch firmware for two panel vendors and updates the touch chip at boot if needed.

## Buttons and other GPIOs

| Pin | Use |
|---|---|
| PE31 | Power button |
| PB13, PB14 | Volume up, volume down |
| PB15 | Play |
| PB06 | microSD card detect |
| PB20 | 3.5 mm plug detect |
| PB00 | charger interrupt |
| PB01 | USB-C controller interrupt |
| PB02 | fuel gauge interrupt |
| PB27 | the charger driver sets it: 1 = USB host power (OTG boost), 0 = charge. **Open:** the device tree also lists PB27 as the panel's tearing input. |
| PB24 | set low by stock at start. **Open:** purpose. |

## Power

- SGM41513 charger. Stock profile: charge current 1.32 A, end voltage 4.35 V, safety timer 16 h. The charger sets its input current limit itself on plug-in (500 mA on a PC port).
- CW2217 fuel gauge: capacity, voltage, current, temperature, cycle count.
- AW35615 USB-C controller with the full Type-C and Power Delivery logic in the stock kernel.
- Battery capacity in mAh: **open**.
- The stock kernel has no working suspend. discfw's standby is a clean power-off; it restores the queue, position and screen on the next start.
- Register details: [STOCK_KERNEL.md](STOCK_KERNEL.md#charger-sgm41513).

## Radio

- AP6212 module = Broadcom BCM43438 (also called BCM4343A1).
- Wi-Fi: 2.4 GHz only (802.11 b/g/n), over SDIO. Out-of-band IRQ on PD00, power on PD01.
- Bluetooth: on UART0 (`ttyS0`) at 3 Mbaud. BT power PD20, wake lines PD21 and PD22.
- Wi-Fi and Bluetooth share one radio. Without Wi-Fi power save, Bluetooth audio drops out (see [STOCK_KERNEL.md](STOCK_KERNEL.md#other-workarounds)).

## microSD

- MSC2, 4-bit bus, high speed (about 47 MHz), no UHS. Reads of about 5.8 MiB/s were measured on a 1 TB exFAT card. That is enough: 384 kHz, 32-bit stereo needs 3 MB/s.
- The stock kernel has FiiO's exFAT driver and vfat. discfw mounts the card read-only.

## USB

- USB-C with a DWC2 OTG controller. discfw uses it as a USB gadget: a serial port (the root shell), plus the microSD card in USB storage mode, or a UAC2 sound card alone in USB DAC mode.
- In mask-ROM mode the SoC appears as `a108:eaef`.
