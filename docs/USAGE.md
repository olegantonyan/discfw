# Using discfw

How to use the player with discfw: the UI, buttons, settings, network, Bluetooth and the command-line tools. Commands marked "on the player" run in the player's shell (`scripts/disc-sh` over USB, or SSH). `scripts/...` commands run on the PC, inside this repository, with the player connected over USB.

## UI

- **Screens:** Home, Folders, Now playing, Queue, Playlists, Library (artists, albums, genres), Settings.
- **Back:** swipe right from the left edge, on every screen except the lock screen, also out of menus and dialogs. A bulge shows how far you are.
- **Shade:** pull down from the top edge: the current song (tap it for Now playing), player controls, Wi-Fi and Bluetooth toggles, volume, brightness, Settings. Hold the Wi-Fi or Bluetooth toggle to open its settings.
- **Lock screen:** comes on when the screen wakes (`lock.*` settings). Swipe up to unlock.
- **Folders:** a letter strip for fast scrolling. Each folder remembers its position. Hold a row for its menu: play, shuffle, add to queue, create playlist, add to playlist. Folders that are not in the library yet play at once and are scanned in the background.
- **Long names** scroll by themselves (`ui.marquee`: all, only the playing item, off). Hold a row and drag sideways to scroll its name by hand.
- **Now playing:** to seek, drag along the edge ring, or hold the cover and turn it (one turn = 1 minute). Cover style: none, static, rotating or vinyl (`ui.cover_mode`). ≡ opens the menu.
- **Accent color:** `ui.accent` (teal, cyan, blue, purple, pink, red, orange, amber, green). Changing it or `ui.font` restarts the UI.
- **Playlists:** MPD stored playlists, each with its own play order.
- **Settings:** made from the settings list below: battery, power profiles, library, buttons, screen, time zone, system.
- If the UI crashes twice within 10 s, it starts in a safe mode.

## Buttons

Defaults:

| Button | Press | Double press | Hold |
|---|---|---|---|
| Play | play / pause | next track | previous track |
| Volume + | volume up | next track | volume up, repeating |
| Volume − | volume down | previous track | volume down, repeating |
| Power | screen off / on | — | power off (after release) |

Every button × press type × screen state is a setting: `keys.<play|volup|voldown|power>.<short|double|long>.<on|off>` (`on` = screen on). Power has no `long` setting: holding it for `keys.power_long_ms` (1.5–4 s) always powers off. Timing: `keys.double_ms`, `keys.long_ms`, `keys.repeat_ms`. List them with `discfw list keys.` on the player.

## Settings

Settings are stored in `/usr/data/discfw/settings` (`key=value`, only values that differ from the default). The full list with ranges and choices is in [`src/common/settings_schema.c`](../src/common/settings_schema.c). Changes apply at once.

On the player:

```
discfw list [PREFIX]      # all settings, or those that start with PREFIX
discfw get KEY
discfw set KEY VALUE
discfw reset KEY
discfw cfg KEY            # read the file directly, without the daemon
```

These settings are shown but **do nothing yet**: `time.ntp`, `play.through_folders`, `audio.pause_bt_loss`, `lib.playlist_sort`, `bt.autoreconnect`, `power.<profile>.wifi_idle_min`, `power.<profile>.bt_idle_min`.

## Music and library

- Put music on a microSD card (exFAT or FAT32), with a card reader or in [USB storage mode](#usb-storage-and-usb-dac). The card is mounted read-only at `/media/sd`.
- Formats: FLAC, MP3, Ogg Vorbis, Opus, WAV, AIFF, AAC, ALAC, APE, WavPack, WMA, TTA. PCM from 32 kHz to 384 kHz. Other rates are resampled to 44.1 kHz.
- The library is MPD's database in `/usr/data/discfw/mpd/`. When it is scanned depends on `lib.scan`: when the card changes (default), on every mount, or only by hand. Scans run at low priority and never while the card is out. On the player: `discfw library scan` (new and changed files), `discfw library rescan` (everything), `discfw library reset` (delete the database).
- Playback state (queue, position, current folder) survives power-off.

## Wi-Fi

- On the player: Settings → Wi-Fi → Networks. Scan, connect (a keyboard opens for the password), change the password, forget, hidden networks, status details. The password itself is never stored, only the WPA key made from it.
- The status page shows the IP address and the host name `snowsky-disk-XXXX` (the last 4 hex digits of the Wi-Fi MAC address). The player announces itself by mDNS as `snowsky-disk-XXXX.local` with SSH, SFTP and MPD services. The clock is set by NTP after each DHCP lease.
- From the PC over USB: `scripts/disc-wifi add "SSID"` (asks for the password), `scripts/disc-wifi remove SSID | list | status | on | off`.
- On the player: `discfw wifi status | scan | list | connect ssid=S [pass=P] [hidden=1] | forget ssid=S`.
- Turning Wi-Fi on or off restarts only Wi-Fi.

## SSH and SFTP

- Keys only, no passwords. `scripts/disc-ssh-key [PUBKEY]` installs your key (default: `~/.ssh/id_ed25519.pub`, `id_ecdsa.pub` or `id_rsa.pub`) over the USB shell.
- `ssh -4 root@snowsky-disk-XXXX.local`. Use `-4` (or `AddressFamily inet`) because the player has no IPv6.
- SFTP works with any client. Setting `net.ssh` turns the SSH server off.

## MPD

- MPD listens on TCP port 6600 (setting `net.mpd`, optional password `net.mpd_password`) and on `/run/mpd.sock`. Any MPD client can control the player.
- Volume is MPD's volume, so every client controls it. discsys maps it to the DAC's attenuation with `audio.dac_range` dB of range and `audio.vol_max`. It remembers the volume per output and per Bluetooth device.
- On the player: `mpc -h /run/mpd.sock status`.

## Bluetooth headphones

- On the player: Settings → Bluetooth → Devices. Search, pair (then trust and connect), connect, disconnect, forget.
- Headphone battery: shown in the status bar, on the lock screen and in the device list, with a warning at 20 %. Headphones report it over the hands-free link (HFP), so with `bt.battery` on (default) bluez-alsa also runs as an HFP audio gateway; the player itself never opens a call audio channel. Turn it off for A2DP only.
- Pairing is accepted only within 35 s after you start it on the player.
- Codecs: SBC, aptX, aptX HD (bluez-alsa). When headphones connect, MPD switches to them; when they disconnect, playback pauses, so no sound comes out of the jack by surprise. Paired headphones reconnect by themselves.
- From the PC over USB: `scripts/disc-bt on | off | scan [SECS] | pair MAC | connect MAC | disconnect [MAC] | forget MAC | list | status`.
- On the player: `discfw bt list | scan [SECS] | pair|connect|disconnect|forget MAC`.
- Headphone buttons (AVRCP): play, pause, next and previous track. discsys registers with BlueZ as a media player (`Media1.RegisterPlayer`), so no `uinput` is needed.

## USB storage and USB DAC

Besides Player, the player has two USB modes. It restarts into a mode, either **Once** (any restart or power-off returns to Player) or **Always** (it starts in that mode at every power-on until you leave with "Back to player").

- **Enter:** Settings → System → USB storage… or USB DAC…, or `discfw mode storage|usbdac [once|always]` on the player. Once is stored in `/usr/data/discfw/mode.next` and Always in `mode.default`; `discfw mode player` clears both.
- **USB storage:** the computer sees the whole microSD card as a removable drive, read-write, next to the serial shell. Nothing plays meanwhile. The player returns to Player with "Back to player" (eject on the computer first) and, in Once mode, also when the computer ejects the drive or the cable is unplugged. Then it scans the library (`lib.scan_after_usb`, on by default).
- **USB DAC:** a USB Audio Class 2 sound card "discfw USB DAC": stereo, 32-bit, 32–384 kHz. The player measures the rate from the incoming audio. The volume keys and the slider on its screen set the DAC volume; the computer's volume scales the signal. Output goes to the DAC, or to S/PDIF with `audio.spdif` (352.8 and 384 kHz are halved for S/PDIF). Clock drift between the computer and the player is absorbed by dropping or repeating single samples, at most 500 ppm. There is no USB serial shell in this mode (the kernel allows UAC2 only as the sole USB function), so use SSH. Leave with "Back to player". No DSD.
- While USB power is present, these modes never power off when idle.

## Battery and power

- The battery indicator is at the top of the screen. More details: Settings → Battery.
- Charge limit `battery.limit` (50–100 %). While the player stays plugged in, charging starts again when the charge falls by `battery.resume_drop` %. `battery.current`: normal (1.3 A) or reduced (0.6 A). `battery.temp_max`: stop charging above this battery temperature (0 = off). How it works: [STOCK_KERNEL.md](STOCK_KERNEL.md#charger-sgm41513).
- Low battery: a warning at `power.lowbat_warn` %, then a 30 s countdown and power-off at `power.lowbat_off` % or below `power.lowbat_mv`.
- Power profiles: `power.battery` and `power.charger` pick saver, balanced, performance or custom (`power.<profile>.*`: dim and off timeouts, idle power-off, animations).
- Standby is a clean power-off. The next start restores the queue, position, folder and screen.
- Sleep timer: `discfw sleep MIN` on the player; `play.sleep_action` chooses pause or power-off.
- On the player: `discctl battery` prints `cap mv ma temp limit state vbus stage cycles health typec ichg`. States: `battery`, `charging`, `full`, `held` (stopped at the limit), `hot`, `idle`.

## DAC

On the player, `discctl` talks to `discd`:

```
discctl status | rate HZ | mute | unmute | release | vol 0..255|+|- | volpct 0..100 | off | reload | watch | battery
```

`vol` is attenuation in 0.5 dB steps. `release` mutes after 500 ms unless a new stream unmutes first. `audio.balance`, `audio.filter` and `audio.dre` use the stock driver's ioctls (applied only after you change them from the default). `audio.dac_idle_off` powers the DAC down after that many seconds of idle (experimental).

Unplugging the 3.5 mm plug pauses playback (`audio.pause_jack_out`, on by default); plugging it back in does not resume. It applies to the jack output only, not to Bluetooth. The 4.4 mm jack is not detected yet. `discctl status` shows `jack=1` while a 3.5 mm plug is in.

ALSA test: `aplay -D disc FILE.wav`, `speaker-test -D disc -c 2 -r 44100 -F S32_LE`.

**S/PDIF:** `audio.spdif` sends coaxial S/PDIF out of the 3.5 mm jack instead of analog audio; connect it to an external DAC with a 3.5 mm to RCA coax cable. The internal DAC is powered down meanwhile, so the 4.4 mm jack is silent too. Unplug headphones first. Output is 24-bit, 44.1–192 kHz (MPD resamples higher rates), at a fixed level: volume does not change it. Changing the setting restarts MPD.

## Other commands on the player

```
discfw state | watch
discfw screen on|off|lock|unlock|toggle
discfw key KEY [short|double|long]      # act as if a button was pressed
discfw vol PCT|+N|-N
discfw toast TEXT
discfw poweroff | reboot
discfw mode player|storage|usbdac       # restart into a mode
discfw ui state | dump [top] | tap X Y | hold X Y [MS] | swipe X1 Y1 X2 Y2 [MS] | back | home | goto SCREEN | folder PATH | shot PATH
```

`discfw ui` talks to the UI's debug socket `/run/discui.sock` (`shot` saves a PNG). From the PC, `scripts/disc-shot FILE.png` takes a screenshot.

- **Time zone:** `time.zone` (list in `/usr/share/discfw/zones.tsv`). The clock shows `?` until NTP has set the time after boot.
- **Logs:** `logread` (a 256 KB ring in RAM).
- **State on the data partition** (`/usr/data/discfw/`, UBIFS, mounted `sync`; FiiO's files there stay untouched): `settings`, `wifi/`, `bluetooth/` (pairings), `bluealsa/`, `ssh/` (host keys, `root/authorized_keys`), `mpd/` (database, state, playlists), `hostname`, `clock`, `TZ`, `mode.next` (the mode for the next boot, used once), `mode.default` (the mode at every boot), `boot/` (one-time test boot, see [DEVELOPMENT.md](DEVELOPMENT.md#test-without-flashing)). If the partition does not mount, a RAM disk is used instead and nothing is formatted.
