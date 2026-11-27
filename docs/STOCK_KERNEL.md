# The stock kernel: interfaces, quirks, blobs

discfw runs on FiiO's Linux 4.4.94 kernel from firmware V2.57 (`xImage`, SHA-256 `1a9484ef…0ed2`, in `mtd1`). This page lists the kernel interfaces discfw uses, the kernel bugs it works around, and the FiiO files it needs. Hardware facts: [HARDWARE.md](HARDWARE.md).

## Kernel facts

- `4.4.94 SMP PREEMPT`, HZ = 100, built with Ingenic's toolchain (gcc 7.2). Module vermagic `4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT`, no module versions.
- No swap (no `CONFIG_SWAP`, so no zram), no kexec, no initrd, no IPv6, no cpufreq, no cpuidle.
- Built in: squashfs (LZO), UBIFS, FiiO's exFAT driver, vfat, configfs USB gadget (ACM, mass storage, UAC2), Bluetooth (`btbcm`, `hci_uart`), `cfg80211`.
- The RTC (32.768 kHz crystal) keeps counting while the player is off. FiiO's driver initializes it on the first open of `/dev/rtc0` and resets it to 2020-03-01 unless its marker (`HSPR` = `RTCV`) and divider are set; it keeps Unix time in UTC. Like stock, discfw reads it at boot (`hwclock -s`) and writes it after an NTP sync and at shutdown.
- Kernel messages go to UART3 (`ttyS3`, 115200 baud). The kernel restarts 10 seconds after a panic.

## Why the user space is built the way it is

- **uClibc-ng with 32-bit `time_t`.** Kernel 4.4 lacks the 64-bit-time ioctls for ALSA and input. musl always uses 64-bit time on 32-bit targets, and glibc with NaN2008 needs kernel headers 4.5 or newer. Kernel headers: 4.4.302.
- **`BR2_mips_32r5` + NaN2008 + FP64.** The FPU only does NaN2008, and the stock user space is FP64/NaN2008. `board/disc/check-abi.sh` fails the build if any ELF file differs.

## FiiO files discfw uses

All of them come from FiiO's official V2.57 update file, `https://fiio-firmware.fiio.net/DISC/SNOWSKY_DISC_update_20260909_v257.zip` (SHA-256 `2bfad000609440bcbf492468e9e5f8f3d5e0bade797323431a15b5755688bfcf`). `package/disc-stock-blobs` downloads it, decrypts the parts, checks the kernel and the root file system against their SHA-256, and copies these files:

| File | License | Use in discfw | Loaded by |
|---|---|---|---|
| `utils.ko`, `soc_utils.ko` | GPL | Ingenic module helpers | `S11modules` |
| `soc_gpio.ko` | GPL | `/dev/gpio` | `S11modules` |
| `soc_msc.ko` | GPL v2 | SD card and SDIO (Wi-Fi) host | `S11modules` |
| `soc_adc.ko` | GPL | ADC (`/dev/jz_adc_aux_*`) | `S11modules` |
| `bcmdhd.ko` | GPL and additional rights | Wi-Fi, and the power line of the Bluetooth half | `S35radio` |
| `soc_watchdog.ko` | GPL | reset at reboot | `discreset` |
| `soc_efuse.ko` | GPL | not used | — |
| `*.sh` next to the modules | | FiiO's module parameters, used as they are | |
| `wifi_bcm/fw_bcm43438a1.bin` | Broadcom, proprietary | Wi-Fi firmware | `bcmdhd` |
| `wifi_bcm/nvram_ap6212a.txt` | proprietary | board calibration for the radio | `bcmdhd` |
| `bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd` | Broadcom, proprietary | Bluetooth patch | `brcm_patchram_plus` |

discfw replaces `wifi_bcm/config.txt` with its own (`PM=2`, see below). The build also keeps FiiO's kernel (`xImage-stock-v257`, not flashed) and the stock root file system (`stock-rootfs-v257-96M.bin`, for `disc-flash stock`).

The kernel and the modules are GPL, but FiiO has not published their source. FiiO's open-source page is `https://fiio.com/opensourcelicenses` (written offer, `support@fiio.net`). Do not publish images built from this tree: they contain these files (`DISC_STOCK_BLOBS_REDISTRIBUTE = NO`).

## DAC: CS43131 driver

FiiO's own driver, not ALSA. Two nodes: `/dev/cs43131` (chip A, I2C 0x30; it also controls both chips for mute and power) and `/dev/cs43131c` (chip C, 0x32). ioctl type `'M'`, payload 1 byte. Header: [`src/discd/cs43131_ioctl.h`](../src/discd/cs43131_ioctl.h).

| ioctl | Node | Does |
|---|---|---|
| `_IO(0x2d)` / `_IO(0x2e)` | A | mute / unmute both chips (12 ms sleep each) |
| `_IOW(0x2d)`, `_IOW(0x42)` | A | volume of chip A, outputs A and B |
| `_IOW(0x2f)`, `_IOW(0x44)` | C | volume of chip C, outputs A and B |
| `_IOW(0x34)` | A | sample rate code (`SP_SRATE`) of chip A |
| `_IOW(0x36)` | C | sample rate code of chip C; then sleeps 150 ms and powers up both chips |
| `_IO(0x2f)` | A | low power: mute, power down, amplifiers off |
| `_IO(0x30)` | A | power down |
| `_IOW(0x3e)`, `_IOW(0x40)` | A, C | digital filter: 0 fast low-latency, 1 slow low-latency, 2 slow phase-compensated, 3 fast phase-compensated, 4 NOS, 5 wideband |
| `_IOW(0x46)` | A | DRE on/off |
| `_IO(0x28)`, `_IO(0x2b)` | A, C | health check; resets the chip if it is not healthy (A sleeps 500 ms) |
| `_IOW(0x38)`, `_IOW(0x3c)` | A, C | mode: 0 PCM, 1 DoP, 3 force a full re-init |

Sample rate codes (`SP_SRATE`, same on both chips):

| Rate (Hz) | 32000 | 44100 | 48000 | 88200 | 96000 | 176400 | 192000 | 352800 | 384000 |
|---|---|---|---|---|---|---|---|---|---|
| Code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |

Rules (`discd` follows them):

- Open both nodes once and keep them open. Opening runs a full chip init with about 0.5 s of sleeps.
- Rate change: chip A first, then chip C. Only `SP_SRATE` changes between rates; the clock setup is the same for the 44.1 kHz and 48 kHz families.
- Never set the same rate again: chip C's rate ioctl always sleeps 150 ms.
- Volume is attenuation in 0.5 dB steps: `0x00` = 0 dB, `0xFF` = most quiet.
- Measured cost: mute or unmute ≈ 50 ms, chip C rate change ≈ 160 ms.

## Audio: PCM and routing

- Play to `hw:0,3` (AS-DMA channel 3 → I2S3), `S32_LE`, 2 channels, at the track's rate. `S95audio` sets the route: `amixer -c 0 cset name=LO3_MUX LI11`.
- **Stale DMA pointer** (`sound/soc/ingenic/as-v2/as-dma.c`): `.pointer` returns a position that the period interrupt updates and only `open()` resets (no `.prepare`). After stop → prepare → start on the same open PCM, the first position read is the old one. If it equals the start threshold, the stream underruns 60 µs after every start, forever. Otherwise the first ~0.5 s are misplaced. **Rule: never restart a stopped PCM on the same handle; close and open it again** (≈ 1.5 ms). MPD patch `0002-alsa-reopen-instead-of-prepare-after-stop` does this after a skip or seek and in underrun recovery.
- **Buffer size steps:** the driver accepts only some buffer sizes, so `snd_pcm_hw_params_set_buffer_time_near()` fails at 44.1 kHz when it is called before the period is set. MPD patch `0001` sets the period first (as `aplay` does).
- The ALSA mixer shows only the SoC's internal codec and the routes; DAC controls exist only as the ioctls above.
- **S/PDIF out:** `hw:0,4` (AS-DMA channel 4) with `LO2_MUX` = `LI12`; the signal leaves on PC14 through the 3.5 mm jack. The transmitter takes 16- to 24-bit samples only (`S32_LE` fails with `EINVAL`, and an ALSA `plug` picks S32 unless its slave format is set). It also accepts 352.8 and 384 kHz, which receivers do not support; stock plays 44.1–192 kHz. Stock switches to it with no GPIO change: it only powers the CS43131 down (amps off) and plays to the other PCM.

## USB gadget: UAC2

- FiiO's `configfs_composite_bind` removes every other function from a configuration that contains `uac2`. So a USB DAC gadget is `uac2` alone: no serial port beside it. Removing `uac2` from such a config leaves it empty, and binding it again fails ("needs at least one function").
- `c_srate` takes one value. The rates the host sees are fixed in the kernel: 32, 44.1, 48, 88.2, 96, 176.4, 192, 352.8 and 384 kHz. Attributes can only be written before the function is linked into a config.
- Capture only (host → player), isochronous adaptive endpoint without feedback, no feature unit (no host volume). ALSA card `FIIO_UAC2_Gadget`, `S32_LE`, 2 channels; the capture PCM allows only the current `c_srate`.
- The kernel ignores the host's sample-rate request and guesses the rate from the size of the first packet, so a 44.1 kHz stream can be reported as 48 kHz. It then writes the guess to `c_srate` and sends a uevent on the misc device `usb_dac` (`UAC_STATE=START`, `UAC_SRATE`, `UAC_FORMAT=PCM|DSD`). Nothing is sent when the host stops a stream (alt setting 0). `discuac` therefore measures the rate from the incoming data.
- Unbinding while a stream runs queues work inside memory that `rmdir` frees, so discfw never tears the gadget down; leaving USB DAC mode is a reboot.

## Charger: SGM41513

`/dev/sgm41513` ioctls of FiiO's driver (type `'I'`):

| ioctl | Does |
|---|---|
| `_IO('I',0x28)` | charge off: `REG00=0x37`, `REG01=0x06` |
| `_IO('I',0x29)` | full charge profile: `REG00..07 = 37 16 AD FA 78 8F E6 4C`, PB27 = 0 |
| `_IO('I',0x2a)` | `REG02=0x21` (600 mA) |
| `_IO('I',0x2c)` | USB host power (OTG boost), PB27 = 1 |

Both `0x28` and `0x29` write `REG00=0x37`, a 2.4 A input limit on any power source. So discd does not use them: it reads and writes the registers directly over `/dev/i2c-2` (address 0x1a, `I2C_SLAVE_FORCE`; FiiO's driver stays bound). Datasheet: [SGM41513 (SG Micro)](https://www.sg-micro.com/rect/assets/58a4fe4d-da1d-49a3-b312-5664211d9016/SGM41513_SGM41513A_SGM41513D.pdf).

| Register | Meaning |
|---|---|
| `REG00` | input current limit, 100 mA + 100 mA × n. The charger sets it itself on plug-in (`0x24` = 500 mA on a PC port). |
| `REG01` | bit 4 `CHG_CONFIG`: 1 = charge. `0x16` = on, `0x06` = off. |
| `REG02` | charge current, a table: `0x2D` = 1320 mA (stock), `0x21` = 600 mA. |
| `REG04` | charge end voltage. `0x78` = special code 4.350 V (stock). Otherwise 3.856 V + 32 mV × n in bits 7:3. |
| `REG05` | `0x8F`: I2C watchdog off (so written bits stay), termination on, safety timer 16 h. |
| `REG07` | bit 5 `BATFET_DIS` = ship mode (not used). |
| `REG08` | status, read-only. Bits 7:5 power source (000 none, 001 USB port, 010 adapter, 111 OTG); bits 4:3 charge stage (00 not charging, 01 pre-charge, 10 fast, 11 done); bit 2 power good. |
| `REG09`, `REG0E` | clear on read: do not poll them. |

discd's charge control, every 5 s (`src/discd/power.c`):

- It reads the gauge and `REG08`, and changes only `REG01` bit 4 and the current bits of `REG02`.
- Charging stops at `battery.limit` %. While the player stays plugged in, it starts again when the charge falls by `battery.resume_drop` %. When you plug in, it charges if the battery is below the limit.
- `battery.current` selects 1.3 A or 0.6 A. `battery.temp_max` stops charging above that battery temperature (3 °C hysteresis).
- On battery, discd always turns charging back on. So if discd dies, charging is never blocked. When the player is off, the stock driver's settings apply.

Stock firmware has an 80 % "charging optimization" that stops at 80 % and starts again at 79 %, every second, through `0x28`/`0x29`.

## Fuel gauge: CW2217

`/sys/class/power_supply/cw221X-bat/`: `capacity` (%), `voltage_now` (µV), `current_now` (µA; 0 at 100 % on USB), `temp` (0.1 °C), `cycle_count`, `health`. There is no `status` or `online` file and no charger power supply, so discd takes the charging state from the charger's `REG08`.

## USB-C: AW35615

The kernel has a full Type-C and PD stack with sysfs files in `/sys/bus/i2c/devices/2-0022/control/`. `typec_state` is safe to read (`4 AttachedSink` on a PC port). **Trap:** only *reading* `control/reinitialize` re-initialises the chip. Never read every file in that directory (for example with `grep -r`).

## Screen, touch, backlight

| Interface | Does |
|---|---|
| `/dev/lcd_st77916` `_IO(0xEF,1)` / `_IO(0xEF,2)` | panel sleep in / sleep out |
| `/dev/cst816t` `_IO(0xEF,3)` / `_IO(0xEF,4)` | touch deep sleep / wake (reset pulse) |
| `/sys/class/backlight/backlight/brightness` | 0–40 |
| `/sys/class/backlight/backlight/bl_power` | backlight power |

**Trap:** `FBIOBLANK` with `FB_BLANK_POWERDOWN` powers the panel off, and unblank does not power it on again: the screen stays dark until reboot. discsys turns the screen off with the sleep ioctls and the backlight instead.

## Buttons

`x2000_key` on `/dev/input/event0`. The kernel times presses itself. Short and double presses arrive only after the gesture ends, as an instant press/release pair. A hold is live: press when the hold is detected, repeat about every 40 ms, release at the real release. Codes:

| Button | Press | Double | Hold |
|---|---|---|---|
| Play | 250 | 269 | 268 |
| Volume + | 251 | 266 | 263 |
| Volume − | 252 | 267 | 262 |
| Power | 265 (screen on), 259 (screen off) | — | 264 |

## ADC: jack and USB detection

`/dev/jz_adc_aux_0..5` (`soc_adc.ko`); `read()` returns one 32-bit sample, apparently in mV (idle: 0, 1641, 1242, 565, 432, 548). Stock thresholds: 4.4 mm plug on channel 1 at 1501 mV (hysteresis bands 149/199), 3.5 mm plug at 501/100, USB power at 80. Plugging a 3.5 mm plug changes none of the six channels: **the 3.5 mm plug is GPIO PB20** (high while a plug is in), which discd reads from the GPIO registers through `/dev/mem`. The CS43131's own headphone detect is off in PCM mode (`HP_DETECT=0x04`). **Open:** the 4.4 mm plug (channel 1, untested) and the other channels.

## Reboot: watchdog reset

The kernel's own restart path (`reboot(2)`) leaves the board dark and hung. So `/sbin/reboot` is a wrapper: init runs the normal shutdown, then `/usr/sbin/discreset` calls `_IO('W',0x10)` on `/dev/jz_watchdog` (`soc_watchdog.ko`): an immediate watchdog reset. The player is back on USB after about 7 s. Power-off works as it is.

## Other workarounds

- **Wi-Fi and Bluetooth share one radio.** With Wi-Fi power save off (stock `PM=0`), Bluetooth audio dropped for about 1 s every 4 s. discfw uses `PM=2` in `config.txt` and runs `iw dev wlan0 set power_save on` after link-up and after each DHCP lease.
- **`bcmdhd` needs a 1 MB physically contiguous buffer** when it loads. After hours of uptime that can fail (`-ENOMEM`), and a failed load leaks a platform device. So `S35radio` loads it at boot and never unloads it; the chip stays powered off until Wi-Fi or Bluetooth is turned on. Fallback: drop caches and compact memory, then retry.
- **BlueZ and dual-mode headsets:** the default mixed scan saw only the LE side and paired over LE (no A2DP). Pairing scans `bredr` only and turns `pairable on` first (otherwise the pairing is not stored).
- **Rare NAND read error:** on the first cold boot after a flash, a page of the root file system was read wrong once (16 bytes shifted by one byte). Squashfs has no data checksums, the bad page stayed in the page cache, and a program crashed on every start. The flash itself was correct. `guard` (started by init) drops the page cache if any crash or squashfs error is logged. `scripts/disc-verify` reads `mtd2` back over USB and compares every block with the image.
- **No IPv6:** nothing answers AAAA queries for the player's name. `mdnsd` is patched to answer with an NSEC record (RFC 6762 §6.1), which macOS, iOS and Windows respect. Avahi on Linux still waits 5 s for AAAA: use `ssh -4` or `AddressFamily inet`.
- **Reverse DNS:** many routers never answer PTR queries for private addresses, so dropbear's reverse lookup cost about 45 s per login. dropbear is built with `BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS`.
- **exFAT `stat` is slow on a cold cache:** about 35 ms per entry on a 1 TB card (a folder with 784 entries took 27 s, against 4 ms for `readdir` alone). **Rule: list folders with `readdir` and `d_type`, never `stat` each entry.** Some entries (`*.BMK`, names cut to 20 characters) come back from `readdir` but fail lookup with `ENOENT`; skip them.
