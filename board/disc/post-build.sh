#!/bin/sh
set -e
TARGET="$1"
BOARD="$(dirname "$0")"

mkdir -p "$TARGET/media/sd" "$TARGET/usr/share/discfw"
printf 'P6\n1 1\n255\n\0\0\0' > "$TARGET/usr/share/discfw/splash.ppm"
date +%s > "$TARGET/etc/discfw-build"
version=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
	echo "discfw $version"
	echo "buildroot $BR2_VERSION_FULL"
} > "$TARGET/etc/discfw-release"
printf 'type _discfw._tcp\nport 22\ntxt model=FiiO Snowsky Disc\ntxt version=%s\n' "$version" > "$TARGET/etc/mdns.d/discfw.service"
rm -f "$TARGET/etc/init.d/S95mpd" "$TARGET/etc/init.d/S50crond" "$TARGET/etc/init.d/S50mdnsd" "$TARGET/etc/mpd.conf" "$TARGET/etc/default/dropbear"
install -d "$TARGET/usr/data" "$TARGET/oldroot"
rm -rf "$TARGET/var/lib/bluetooth" "$TARGET/var/lib/bluealsa" "$TARGET/etc/dropbear" "$TARGET/root/.ssh"
ln -s /usr/data/discfw/bluealsa "$TARGET/var/lib/bluealsa"
ln -s /usr/data/discfw/bluetooth "$TARGET/var/lib/bluetooth"
ln -s /usr/data/discfw/ssh "$TARGET/etc/dropbear"
ln -s /usr/data/discfw/ssh/root "$TARGET/root/.ssh"
ln -sfn /media/sd "$TARGET/root/sd"
ln -sfn /usr/data/discfw "$TARGET/root/discfw"
ln -sfn /usr/data/discfw/TZ "$TARGET/etc/TZ"
rm -rf "$TARGET/usr/libexec/discfw-test"
install -d "$TARGET/usr/libexec/discfw-test"
install -m 0755 "$BOARD"/../../scripts/device/*.sh "$TARGET/usr/libexec/discfw-test/"
"$BOARD/check-abi.sh" "$TARGET" "$HOST_DIR"
