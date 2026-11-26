# discfw — Snowsky Disc firmware (Buildroot external tree)

Open userland for the FiiO Snowsky Disc, built from source with mainline Buildroot **2026.02.3 LTS** plus small
patches (`patches/`). v0 boots on the **stock kernel** (mtd1 untouched), replaces only the rootfs (mtd2),
shows a folder browser and plays music through MPD to the dual CS43131 DAC.

## Build

```
make            # fetches + verifies Buildroot, applies patches/buildroot, configures disc_defconfig, builds
make menuconfig / make savedefconfig / make br-<target>   # e.g. make br-discui-rebuild
```

Output `output/images/`:

- `disc-rootfs-96M.bin` — squashfs (LZO, 128 KiB blocks) padded to the writer's fixed 96 MiB; this is what gets flashed
- `rootfs.squashfs`, `xImage-stock-v257` (stock kernel, for future RAM-boot work), `sha256sums.txt`

Package patches (`patches/packages/`): MPD (period before buffer; reopen PCM after stop), linux-headers (host gcc
C23), libglib2 (link-test `statx`, which uClibc-ng declares but lacks on 4.4 headers). Radio userland is built in
(`iw`, `wpa_supplicant`, `brcm_patchram_plus`, BlueZ incl. `hciconfig`/`hcitool`) but not started at boot yet.

Inputs: FiiO's official OTA `research/firmware/SNOWSKY_DISC_update_20260909_v257.zip` (sha256-pinned; Kconfig
`BR2_PACKAGE_DISC_STOCK_BLOBS_OTA`) supplies the 8 GPL stock kernel modules and radio firmware. Everything else is
source. `board/disc/check-abi.sh` fails the build if any target ELF is not NaN2008 + FP64.

## Flash and roll back

Needs the diskOS checkout (`~/projects/github/diskOS`, or `DISKOS_DIR`) with its udev rule, and a verified diskOS
stock backup (`~/.local/share/diskos-installer/stock/`). `disc-flash` refuses to run without the backup.

```
scripts/disc-flash check     # image sanity + rollback readiness, no USB
scripts/disc-flash rootfs    # write disc-rootfs-96M.bin to mtd2 (~18 min)
scripts/disc-flash stock     # restore stock V2.57 rootfs (diskos-installer restore-stock, ~20 min)
```

Mask-ROM mode: power the Disc off, hold **Volume-Down**, plug USB (screen stays black). The writer is diskOS's
(`diskos_installer.flasher.flash`): it writes only mtd2 and first proves on the device that kernel, recovery kernel
and recovery rootfs are FiiO V2.57, so the same path always restores stock. U-Boot, kernel, recovery, `mac` and
`/usr/data` are never written by the writer; discfw keeps its state in `/usr/data/discfw/` and leaves stock's files
alone, so stock comes back with its data. Mask-ROM ignores Power while USB is connected; to leave it, unplug USB and
hold Power ~6 s (the RTC's long-press reset).

**If a build does not boot:** the stock kernel reboots 10 s after a panic. Hold Volume-Down and plug USB while it
reboots → mask-ROM → `scripts/disc-flash stock`. If the writer result is unknown, wait 25 minutes before retrying.

## On the device

- USB serial root shell: `/dev/serial/by-id/usb-discfw_discfw_Serial_discfw-if00` (also UART3 `ttyS3` 115200).
  `scripts/disc-sh 'cmd' ...`, `scripts/disc-sh --push local /tmp/file` (raw bytes, ~16 MB/s, sha256-checked).
- Boot a build once without flashing: `make && scripts/disc-tryboot` (stores `rootfs.squashfs` in
  `/usr/data/discfw/boot`, arms a one-shot flag, reboots; `/linuxrc` loop-mounts it, the next boot uses NAND again).
- Swap a build into the running system: `make && scripts/disc-tryimage` (pushes `rootfs.squashfs` to RAM, loop-mounts it, binds
  its `/bin /sbin /lib /usr /etc /var` over the running ones, restarts services; gone on reboot).
- Single files without flashing: `make br-<pkg>-rebuild` then `scripts/disc-hotpatch usr/bin/mpd usr/sbin/discd ...`
  (pushes each file from `output/target` and bind-mounts it over the squashfs copy; RAM only, gone on reboot).
  Restart the daemon afterwards (`killall mpd discd discui`; init respawns them).
- SD card: `/media/sd`, mounted **read-only** (exFAT/vfat via the stock kernel).
- Persistent state: `/usr/data/discfw/` on the stock `userdata` UBIFS (mtd7, mounted `sync`; stock files untouched; never
  formatted — if it will not mount, a tmpfs stands in). Holds `wifi/`, `bluetooth/` (BlueZ pairings via
  `/var/lib/bluetooth`), `ssh/` (host keys, `root/authorized_keys`), `mpd/` (state, playlists), `hostname`, `clock`,
  `charge_limit`.
- Wi-Fi: `scripts/disc-wifi add "SSID"` (prompts; derives the WPA key on the host, only the key reaches the player),
  `remove SSID | list | status | on | off`. Hostname `snowsky-disk-XXXX` (last 4 hex of the Wi-Fi MAC), announced by
  `mdnsd` as `snowsky-disk-XXXX.local` with `_ssh`, `_sftp-ssh`, `_mpd` services; NTP after each DHCP lease.
- SSH/SFTP (dropbear + gesftpserver), keys only: `scripts/disc-ssh-key [PUBKEY]`.
- MPD on TCP 6600 (LAN, no password) as well as `/run/mpd.sock`; resumes paused at the saved position after reboot.
- Bluetooth headphones: `scripts/disc-bt on | scan | pair MAC | connect MAC | forget MAC | list | status | off`.
  bluez-alsa A2DP source (SBC, aptX, aptX HD); `discbt` moves MPD to the `BT` output while a sink is connected and
  pauses when it disconnects (nothing leaks out of the jack); bonded headphones reconnect by themselves. Verified
  2026-11-25 with UGREEN Studio Pro (SBC). Headphone buttons (AVRCP) do not control playback yet: no `uinput`.
- DAC: `discctl status | rate <hz> | mute | unmute | release | vol <0..255>|+|- | off` (attenuation in 0.5 dB;
  default 0x50 = −40 dB, kept in `/run/discd.vol`). `release` = mute in 500 ms unless a new stream unmutes first.
- Battery: `discctl battery` (`cap mv ma temp limit state`; state `battery | charging | full | held | idle`),
  `discctl limit 100|80|60` (any 50–100). UI: indicator on top of every screen (bolt = charging, USB = powered, not
  charging); **Settings** (first entry of the SD root) shows details and the charge limit.
- MPD: `mpc -h /run/mpd.sock status`. ALSA: `aplay -D disc`, `speaker-test -D disc -c 2 -r 44100 -F S32_LE`.
- Logs: `logread` (syslogd 256 KB RAM ring).
- Tests (`/usr/libexec/discfw-test/`, sources `scripts/device/`): `skipstress.sh` (random next/prev bursts, checks
  DMA pace, MPD time, DAC state), `skiplat.sh` (skip latency), `ratetest.sh DIR` (every file: PCM rate, DMA pace,
  `SP_SRATE` on both chips), `transport.sh DIR` (gapless, seek, pause, stop), `soak.sh SECONDS`, `hwprobe.sh`,
  `loadtest.sh FILE` (playback under 2-core CPU + SD + memory load), `radiotest.sh` (Wi-Fi + BT bring-up as stock), `msctest.sh` (run detached: RAM-disk mass storage next to ACM, then
  reverts). `scripts/device/unsafe/uactest.sh` is not installed: tearing down an ACM+UAC2 composite after streaming
  took the stock kernel's USB gadget down for good (needs a power cycle).
- Keys: play = play/pause; vol± = volume; double vol+/double play = next; double vol− = previous; power short =
  screen off/on; power long (~3 s) = clean poweroff.

## Design

| Piece | Choice | Why |
|---|---|---|
| CPU flags | `BR2_mips_32r5` + NaN2008 + FP64, `-ffp-contract=off` | XBurst2 FPU is IEEE-2008; stock userland is FP64/NaN2008. Mainline Buildroot has no XBurst2 symbol; fallback if r5 code misbehaves: thingino's one-symbol `BR2_mips_xburst2` patch (`-march=mips32r2`). |
| libc | uClibc-ng 1.0.57, **32-bit time_t** | Kernel 4.4 lacks the time64 ALSA/input ABI. musl is always time64 on 32-bit (alsa-lib would use 5.6+ ioctls); glibc with NaN2008 needs headers ≥ 4.5. Buildroot disables uClibc time64 with headers < 5.1. |
| Kernel headers | 4.4.302 | Match the running kernel. `patches/packages/linux-headers` fixes a C23 build break with host gcc ≥ 15. |
| Init | BusyBox init + mdev; `/linuxrc` → `/sbin/init`, or, when a one-shot trial is armed, loop-mounts `/usr/data/discfw/boot/rootfs.sqfs` and `pivot_root`s into it (data partition moved along) | Stock U-Boot passes `init=/linuxrc root=/dev/mtdblock_bbt_ro2 rootfstype=squashfs`. A RAM copy of the rootfs was tried and dropped: no swap/zram, so its 17 MB (14.5 % of RAM) is pinned, while NAND page-in costs ~6 ms only under memory pressure the copy itself causes. Any trial failure before the pivot boots NAND. Per-process respawn, no kill-all supervisor. |
| Radios | `S35radio`: `bcmdhd` with stock parameters at every boot; Wi-Fi = wpa_supplicant + udhcpc; BT = `brcm_patchram_plus` on `ttyS0` + bluez-alsa | `bcmdhd` owns the BT power line too, and needs a 1 MB contiguous buffer (fails once memory fragments), so it loads early and stays loaded; the chip stays powered off until used. |
| Modules | stock `utils soc_utils soc_gpio soc_msc soc_adc` with stock parameters (`/lib/modules/4.4.94/fiio/*.sh`) | SD card + SDIO host, GPIO, ADC. Wi-Fi (`bcmdhd`) not loaded in v0. |
| Audio | MPD → ALSA `disc` (hooks PCM over `hw:0,3`) → `libdisc_hook.so` → `discd` | Hook sets DAC rate + unmute on `hw_params` (rate read from `/proc/asound/.../hw_params`; the hooks PCM is not set up yet) and sends `release` on `hw_free`/close, so any ALSA client gets correct DAC state and same-rate reopens never touch the DAC. `S95audio` routes DMA3 → I2S3 (`LO3_MUX=LI11`, as stock) and, first thing at shutdown, stops playback and powers the DAC down. MPD: `format "*:32:2"` + DAC rates in `allowed_formats` (unlisted rates resample to 44.1k), `period_time 40000`; two patches for the stock driver (below). |
| DAC | `discd` holds `/dev/cs43131` + `/dev/cs43131c` (stock driver ioctls), init once | Opening runs the full init; rate change = SRATE A then C (C sleeps 150 ms, powers both). |
| Charge limit | `discd` every 5 s: gauge sysfs + SGM41513 `REG08` status, toggles only `REG01` CHG_CONFIG over `/dev/i2c-2`; stop at ≥ limit, resume at limit − 5 while plugged | Stock's `0x28`/`0x29` ioctls also force a 2.4 A input limit on any source; one bit leaves the charger's detected limit alone. Unplugged → charging back on, so a dead `discd` never blocks charging. Details: `SYSTEM_CONTROL.md` §4.2. |
| Supervision | inittab `respawn` for `discd`, `mpd --no-daemon`, `discui` | A crashed daemon restarts alone; `discui` reconnects to MPD. |
| UI | `discui`, LVGL 9.6 (static), own fbdev flush with 180° rotation, `lv_evdev` touch, raw key reader, libmpdclient | Panel is mounted upside down; DejaVu Sans via tiny_ttf for accented names. |
| USB | configfs gadget, ACM only | Root shell for development. |

## Stock kernel quirks (4.4.94) and workarounds

- **AS DMA stale pointer** (`sound/soc/ingenic/as-v2/as-dma.c`): `.pointer` returns `prtd->hw_ptr`, updated only in
  the period IRQ and reset only at `open()` (no `.prepare`; `hw_params` keeps it). After stop → prepare → start on the
  same open PCM the first pointer read is the old position: if it equals the start threshold the stream xruns 60 µs
  after every start forever (no sound, MPD time races); otherwise the first ~0.5 s are misplaced. **Rule: never
  restart a stopped PCM on the same handle; close and reopen** (~1.5 ms). MPD patch
  `0002-alsa-reopen-instead-of-prepare-after-stop` does this after `snd_pcm_drop()` (skip/seek) and in xrun recovery.
- **Buffer size step**: the driver accepts only some buffer sizes, so `snd_pcm_hw_params_set_buffer_time_near()` fails
  at 44.1 kHz when called before the period; MPD patch `0001` sets the period first (as `aplay` does).
- **Routing**: DMA3 → I2S3 needs `LO3_MUX=LI11` (stock `mq_player` sets it; all `LOn_MUX` are `UNUSED` at boot).
- **Wi-Fi/BT coexistence** (one radio): with Wi-Fi power save off (stock `PM=0`), A2DP dropped ~1 s every ~4 s.
  Ours: `config.txt` `PM=2` + `iw dev wlan0 set power_save on` (after link-up and each DHCP lease) — clean.
- **BlueZ discovery** of dual-mode headsets: the default mixed scan only saw their LE side and paired over LE (no
  A2DP). `bt pair` scans `bredr` and turns `pairable on` first (else the pairing is not bonded and is lost).
- **No swap**: no `CONFIG_SWAP`, so no zram/zswap. RAM headroom comes from running lean.
- **bcmdhd** preallocates a 1 MB physically contiguous buffer at `insmod` (order-9); after hours of uptime that can
  fail with `-ENOMEM` and a failed load leaks a platform device. Load it at boot; fallback: drop caches + compact.
- **Rare NAND read glitch**: on the first cold boot after one flash, a read of rootfs block 37 came back with a
  16-byte burst shifted by one byte. Squashfs has no data checksums, the bad page stayed in the page cache and
  `mpd` segfaulted on every restart; the flash itself was intact (re-read after `drop_caches` matched). Not
  reproduced in 1.4 GB of re-reads under Wi-Fi/SD load or 8 reboots. Defences: `guard` (inittab) drops the page
  cache after ≥ 3 crashes in 10 s; `scripts/disc-verify` reads mtd2 back and compares every 128 KiB block with the
  image (`NODROP=1` checks what boot already cached).
- **Restart halts**: the kernel's restart path (`reboot(2)`) leaves the board dark and hung. `/sbin/reboot` is a
  wrapper: init runs the normal shutdown, then `/usr/sbin/discreset` issues the stock `soc_watchdog.ko` reset ioctl
  (`_IO('W', 0x10)`, `/dev/jz_watchdog`); back on USB in ~7 s. Power-off works as is.
- **No IPv6**: nothing answers AAAA for the player's name. `mdnsd` is patched to answer with an NSEC (RFC 6762 §6.1),
  which macOS/iOS/Windows honour; Avahi on Linux still waits 5 s for AAAA — use `ssh -4` or `AddressFamily inet`.
- **ISP DNS** (and many others) never answer PTR queries for private addresses, so dropbear's reverse lookup cost
  ~45 s per login; built with `BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS`.
- **DAC ioctls sleep**: mute/unmute ≈ 50 ms, `C_SET_SRATE` ≈ 160 ms (HZ=100). Skip latency to running stream: same
  rate ≈ 40–50 ms, rate change ≈ 300 ms.
- **exFAT (FiiO 4.4 driver) `stat` is slow cold**: ≈ 35 ms per entry on a 1 TB card; a 784-entry root took 27 s to
  `stat` cold vs 4 ms for `readdir` alone (`d_type` is filled). **Rule: list folders by `d_type`, never `stat` per
  entry.** 84 root entries (`*.BMK`, names cut to 20 chars) come back from `readdir` but fail lookup (`ENOENT`); skip.
- **SD bus**: 4-bit, 50 MHz high-speed (no UHS) ≈ 20 MB/s, ample for 384k/32-bit PCM (3 MB/s).

## Roadmap

1. **Any kernel**: ~~back up mtd0–mtd7 incl. `mac`~~ done 2026-11-25 (`../backup/nand-2026-11-25/`, no OOB);
   tethered RAM boot (usbboot SPL + jump stub + uImage at 0x80F00000, no NAND writes); a writer for mtd1 with a
   stock-kernel restore path; kernel from source (4.4.94 SDK mirror, decompiled board DT; later 5.10/6.6/mainline).
   Mainline Linux 7.4 was ported 2026-11-25/26 and parked (unreliable NAND boots): branch `kernel-74`,
   `../research/MAINLINE.md` § Status.
2. **Parity**: USB storage (configfs `mass_storage`, `lun.0/file=/dev/mmcblk0` after unmount), Wi-Fi + Bluetooth
   services (wpa_supplicant config, bluetoothd, bluez-alsa as a second MPD output), USB DAC mode (configfs `uac2`;
   FiiO's kernel has the multi-rate `u_audio` backport) + capture→`disc` forwarder, jack status, low-battery
   power-off, settings storage, DAC low-power when idle, library database, EQ. Battery indicator + charge limit:
   done 2026-11-26.

   Verified on device 2026-11-25 (`scripts/device/radiotest.sh`, stock procedure): `bcmdhd` + stock fw/nvram →
   `wlan0` (MAC from chip OTP, country CN by default), `iw` scan OK; `brcm_patchram_plus --enable_hci ... --baudrate
   3000000 --patchram BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0` → `hci0` UP (BCM43438A1, BT 5.2 fw 1026.1055).
   Kernel has `bluetooth`, `btbcm`, `hci_uart`, `n_hci`, `cfg80211` built in. Audio stayed clean (0 anomalies in a
   10-min soak) during repeated Wi-Fi scans + BT inquiries. Mass storage: ACM + `mass_storage` composite enumerates
   on the host (RO LUN) and reverts cleanly. UAC2 gadget: host sees S32_LE stereo, adaptive iso OUT, 32k–384k + a
   native-DSD altset, and accepted a 96k stream; the ACM function did not enumerate alongside it, and unbinding the
   composite afterwards left the device off USB (no panic reboot). USB-DAC mode must be its own gadget (as stock's
   `uac_demo`), switched with care; root-cause the teardown before shipping it. The charger's driver charges on its
   own; the gauge is `/sys/class/power_supply/cw221X-bat`. RTC does not tick (`rtc32k_init_on=0`), so time needs NTP.
   Full image re-verified 2026-11-25 from RAM (`disc-tryimage`): rates 32k–384k + soxr resampling, transport 8/8,
   60/60 skip bursts, skip 29–49 ms same rate / ~300 ms across rates, playback clean under 2-core CPU + SD + 24 MB
   tmpfs load at 48k and 96k (underruns only when overloaded ×2 with RAM nearly exhausted), dbus + bluetoothd →
   `bluetoothctl show` powered, Wi-Fi scan 10–11 networks, `S95audio stop` 0.35 s, `poweroff` leaves USB for good.
   RSS: mpd 12.6 MB, bluetoothd 2.9, discui 2.6, dbus 1.1, discd 0.4. Flashed (image `59e858c1…`) and re-run from a
   clean boot: rates 20/20 (+2 intended rejects), transport 8/8, skips 60/60, load test clean; services up ≈ 5 s
   after kernel start. By ear/hand: no pops (start, stop, skip, power-off), keys and touch direction OK, long-press power-off OK,
   Power wakes the screen after the 60 s timeout.

Background: `../research/FROM_SCRATCH.md`, `../research/re/blobs/BLOBS_ABI.md`, `../research/re/spike1_cs43131/`.
