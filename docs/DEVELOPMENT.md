# Development

How discfw is built and how to work on it. To build and flash for the first time, see the [README](../README.md).

## Source layout

```
Makefile                  downloads and patches Buildroot, then runs it (output in output/)
configs/disc_defconfig    the Buildroot configuration
package/                  our Buildroot packages: libdisc, discd, discsys, discui, lvgl, disc-stock-blobs
src/common/               libdisc: settings, key/value protocol, paths, text helpers, unit tests
src/discd/                discd (DAC, charger), discctl, the ALSA hook libdisc_hook.so, discreset
src/discsys/              discsys (system policy), the discfw command
src/discui/               discui (LVGL UI)
board/disc/               rootfs overlay (init scripts, helpers), post-build and post-image scripts, ABI check
patches/buildroot/        patches for Buildroot itself
patches/packages/         patches for Buildroot packages (MPD, mdnsd, glib, linux-headers, brcm-patchram-plus)
scripts/                  PC tools: flash, USB shell, test without flashing, Wi-Fi, Bluetooth, host build
scripts/device/           test scripts installed on the player in /usr/libexec/discfw-test/
docs/                     this documentation
```

## Build

- `make` downloads Buildroot 2026.02.3 into `dl/`, checks its SHA-256, applies `patches/buildroot/`, loads `configs/disc_defconfig` and builds into `output/`. All downloads go to `dl/`, which you can keep between builds.
- `make br-<target>` runs any Buildroot target, for example `make br-discui-rebuild`, `make br-menuconfig`, `make br-legal-info`. `make menuconfig` and `make savedefconfig` work as usual.
- ccache is on (`~/.buildroot-ccache`).
- `board/disc/check-abi.sh` fails the build if any ELF file is not NaN2008 + FP64.
- `board/disc/post-image.sh` pads `rootfs.squashfs` to the writer's fixed 96 MiB (`disc-rootfs-96M.bin`) and writes `sha256sums.txt`.

Patches to packages:

| Patch | Why |
|---|---|
| `patches/buildroot/0001-package-mpd-do-not-require-headers-5.6` | MPD builds and works with 4.4 headers |
| `mpd/0001-alsa-set-period-time-before-buffer-time` | the stock driver accepts only some buffer sizes ([STOCK_KERNEL.md](STOCK_KERNEL.md#audio-pcm-and-routing)) |
| `mpd/0002-alsa-reopen-instead-of-prepare-after-stop` | the stock DMA driver keeps a stale pointer after stop |
| `mdnsd/0001-nsec-for-missing-aaaa` | answer AAAA queries with NSEC (no IPv6) |
| `libglib2/0001-meson-link-test-statx` | uClibc-ng declares `statx`, but the 4.4 headers lack it |
| `linux-headers/0001-scripts-unifdef-rename-constexpr` | host gcc 15 (C23) build fix |
| `brcm-patchram-plus/0001-wait-with-pause-not-sleep-uint-max` | wait with `pause()` instead of a long `sleep()` |

## Design

| Part | Choice | Why |
|---|---|---|
| CPU flags | `BR2_mips_32r5` + NaN2008 + FP64, `-ffp-contract=off` | The FPU is IEEE 754-2008; the stock user space is FP64/NaN2008. Buildroot has no XBurst2 option. |
| libc | uClibc-ng 1.0.57, **32-bit `time_t`** | Kernel 4.4 lacks the 64-bit-time ALSA and input ABI. musl is always 64-bit time; glibc with NaN2008 needs headers ≥ 4.5. |
| Kernel headers | 4.4.302 | match the running kernel |
| Init | BusyBox init + mdev. `/linuxrc` runs `/sbin/init`, or, when a one-time test boot is armed, loop-mounts `/usr/data/discfw/boot/rootfs.sqfs` and switches into it | The stock boot loader passes `init=/linuxrc`. A RAM copy of the whole root file system was tried and dropped: without swap its 17 MB (14.5 % of RAM) stay pinned. Any failure before the switch boots the NAND image. |
| Supervision | inittab `respawn` for `discd`, `discsys`, `mpd-run`, `discui`, `discbt`, `guard`. discsys pings the UI every 5 s and kills it after 3 missed answers. Two UI crashes in 10 s → safe mode. | A crashed daemon restarts alone and rebuilds its state from MPD, discd and sysfs. There is no "kill everything" supervisor like stock's. |
| System policy | `discsys` (GLib): buttons, screen power (backlight, `bl_power`, panel and touch sleep), power profiles, low battery, settings, volume, library scan policy, Wi-Fi, Bluetooth. Line protocol on `/run/discsys.sock`, command `discfw`. | Buttons, volume, power-off and screen timeouts keep working while the UI is dead or restarting. |
| Audio | MPD → ALSA `disc` (a `hooks` PCM over `hw:0,3`) → `libdisc_hook.so` → `discd` | The hook sets the DAC rate and unmutes on `hw_params`, and sends `release` on `hw_free`/close. So any ALSA client gets the right DAC state, and reopening at the same rate never touches the DAC. MPD: `format "*:32:2"`, DAC rates in `allowed_formats` (others resample to 44.1 kHz), `period_time 40000`. |
| DAC | `discd` keeps `/dev/cs43131` and `/dev/cs43131c` open, initialises once | Opening runs the full init. Rate change = rate on A, then on C. |
| Shutdown | `S95audio stop` pauses MPD (position kept), stops it, then powers the DAC down | no pop at power-off |
| Charge limit | `discd` every 5 s: gauge sysfs + charger `REG08`, writes only `REG01` bit 4 and the `REG02` current bits over `/dev/i2c-2` | The stock ioctls also force a 2.4 A input limit on any source. Unplugged → charging on, so a dead `discd` never blocks charging. |
| Radios | `S35radio`: `bcmdhd` with stock parameters at every boot. Wi-Fi = wpa_supplicant + udhcpc; Bluetooth = `brcm_patchram_plus` on `ttyS0` + bluetoothd + bluez-alsa | `bcmdhd` also owns the Bluetooth power line and needs a 1 MB contiguous buffer, so it loads early and stays loaded. The chip stays off until used. |
| Modules | stock `utils soc_utils soc_gpio soc_msc soc_adc` with stock parameters | SD card, SDIO, GPIO, ADC |
| UI | `discui`, LVGL 9.6 (static), own fbdev flush with 180° rotation, own touch reader and gesture layer, a `poll()` loop that sleeps while the screen is off, recycled list rows, cover art in a worker thread (libjpeg-turbo DCT scaling, libpng). DejaVu Sans through tiny_ttf for accented names. | The panel is mounted upside down. |
| Formats | MPD: FLAC, MP3 (mpg123), Ogg/Opus, WAV/AIFF (libsndfile), and ffmpeg with only the AAC, ALAC, APE, WavPack, WMA and TTA decoders | the stock formats without a full ffmpeg |
| USB | configfs gadget, ACM only | root shell for development |
| Reboot | `/sbin/reboot` wrapper → `discreset` (watchdog reset) | the kernel's restart path hangs |

## Test without flashing

All of these need the player running discfw and connected over USB.

- **USB shell:** `scripts/disc-sh 'cmd' ...` runs commands. `scripts/disc-sh --push LOCAL /tmp/FILE` copies a file (about 16 MB/s, SHA-256 checked). The device is `/dev/serial/by-id/usb-discfw_discfw_Serial_discfw-if00` (change it with `DISC_TTY`). Your user needs access to serial ports (often the `dialout` or `uucp` group).
- **Single files:** `make br-<pkg>-rebuild`, then `scripts/disc-hotpatch usr/bin/mpd usr/sbin/discd ...`. It copies each file from `output/target` into RAM and bind-mounts it over the original. Restart the daemon after that (`killall discd discsys discui mpd`; init starts them again). Gone after a reboot.
- **Whole image in the running system:** `make && scripts/disc-tryimage`. It copies `rootfs.squashfs` into RAM, mounts it, binds its `/bin /sbin /lib /usr /etc /var` over the running ones and restarts the services. Gone after a reboot.
- **Boot an image once:** `make && scripts/disc-tryboot`. It stores `rootfs.squashfs` in `/usr/data/discfw/boot`, arms a one-time flag and reboots. `/linuxrc` loop-mounts it; the next boot uses the NAND image again.
- **Check the flash:** `scripts/disc-verify` reads `mtd2` back and compares every 128 KiB block with the image. `NODROP=1` checks what is already in the page cache.
- **Screenshot:** `scripts/disc-shot FILE.png`.

## Host build

`scripts/host-build [test|discsys|discui|all]` builds into `output/host-discfw`:

- `test`: libdisc and its unit tests (they run on every call).
- `discsys`: `discsys` with ASan and UBSan. `DISC_FAKE_HW=1` (or `--fake-hw`) runs it without the player's hardware.
- `discui`: a headless `discui --host` (or `DISCUI_HOST=1`) with ASan and UBSan, drawing into memory. Drive it through its debug socket; `shot` saves screenshots.
- `all`: all of the above plus `discd` and `discctl`.

Environment: `DISC_RUN_DIR` (sockets, default `/run`), `DISC_DATA_DIR` (settings and state, default `/usr/data/discfw`), `DISC_MUSIC_ROOT`, `DISCUI_FONT`, `DISCUI_ROTATE=0` (no 180° rotation). Point them at a host MPD to test with real music.

Needs: a host C compiler with ASan/UBSan, make, cmake, ninja, pkg-config, rsync, and the development packages of glib-2.0 (with gio-unix), libmpdclient, libpng, libjpeg and zlib. The LVGL source is fetched with `make br-lvgl-source` if it is missing.

## Tests on the player

Installed in `/usr/libexec/discfw-test/` (source in `scripts/device/`). Run them over `scripts/disc-sh` or SSH.

| Script | Tests |
|---|---|
| `skipstress.sh` | random next/previous bursts; checks DMA pace, MPD time, DAC state |
| `skiplat.sh` | skip latency |
| `ratetest.sh DIR` | every file in DIR: PCM rate, DMA pace, `SP_SRATE` on both chips |
| `transport.sh DIR` | gapless, seek, pause, stop |
| `soak.sh SECONDS` | long playback |
| `loadtest.sh FILE` | playback under CPU, SD and memory load |
| `radiotest.sh` | Wi-Fi and Bluetooth bring-up, as stock does it |
| `uitest.sh [DIR]` | discsys + discui + MPD through their sockets (plays DIR, then pauses) |
| `hwprobe.sh` | read-only hardware overview |
| `msctest.sh` | USB mass storage next to the serial port, from a RAM disk; run it detached, it reverts by itself |

`scripts/device/unsafe/uactest.sh` is not installed: it takes the stock kernel's USB gadget down until a power cycle.

## Measurements

From 2026-11-25, before the current UI and discsys. They need to be measured again.

| What | Result |
|---|---|
| Skip, same sample rate (from `mpc next` until the PCM runs again, ~10 ms resolution) | 29–49 ms |
| Skip, rate change | about 300 ms |
| Services up after kernel start | about 5 s |
| Reboot until USB is back | about 7 s |
| `S95audio stop` | 0.35 s |
| Memory use (RSS) | mpd 12.6 MB, bluetoothd 2.9 MB, discui 2.6 MB, dbus 1.1 MB, discd 0.4 MB |
| Stability | rates 20/20, transport 8/8, skip bursts 60/60, load test clean, no pops at start, stop, skip or power-off |

## Roadmap

- **USB mass storage:** configfs `mass_storage` with `lun.0/file=/dev/mmcblk0` after unmounting the card (`msctest.sh` works).
- **USB DAC mode:** configfs `uac2` (FiiO's kernel has the multi-rate backport) and a capture → `disc` forwarder. It must be its own gadget, switched with care; find out first why removing the ACM + UAC2 gadget breaks USB.
- **Headphone buttons (AVRCP):** need `uinput`, which the stock kernel lacks.
- **Jack status** from the ADC; equalizer; the settings that do nothing yet ([USAGE.md](USAGE.md#settings)).
- **Power:** radios off when not used, DAC low power when idle (`audio.dac_idle_off`, experimental), measure the battery current.
- **Measure again** skip latency, boot time and memory with the current UI.
- **Ideas the open user space allows:** audio streaming over the network (RTP), more Bluetooth codecs (bluez-alsa supports AAC and LDAC, among others), more MPD plugins.
- **Mainline kernel:** [MAINLINE.md](MAINLINE.md).
