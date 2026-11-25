#!/bin/sh
set -e
TARGET="$1"
BOARD="$(dirname "$0")"

mkdir -p "$TARGET/media/sd" "$TARGET/usr/share/discfw"
python3 -c 'import sys; sys.stdout.buffer.write(b"P6\n360 360\n255\n" + bytes((0, 96, 112)) * 360 * 360)' > "$TARGET/usr/share/discfw/splash.ppm"
{
	echo "discfw $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "buildroot $BR2_VERSION_FULL"
} > "$TARGET/etc/discfw-release"
rm -f "$TARGET/etc/init.d/S95mpd" "$TARGET/etc/init.d/S50crond" "$TARGET/etc/init.d/S50mdnsd"
install -d "$TARGET/usr/data" "$TARGET/oldroot"
rm -rf "$TARGET/var/lib/bluetooth" "$TARGET/var/lib/bluealsa" "$TARGET/etc/dropbear" "$TARGET/root/.ssh"
ln -s /usr/data/discfw/bluealsa "$TARGET/var/lib/bluealsa"
ln -s /usr/data/discfw/bluetooth "$TARGET/var/lib/bluetooth"
ln -s /usr/data/discfw/ssh "$TARGET/etc/dropbear"
ln -s /usr/data/discfw/ssh/root "$TARGET/root/.ssh"
rm -rf "$TARGET/usr/libexec/discfw-test"
install -d "$TARGET/usr/libexec/discfw-test"
install -m 0755 "$BOARD"/../../scripts/device/*.sh "$TARGET/usr/libexec/discfw-test/"
"$BOARD/check-abi.sh" "$TARGET" "$HOST_DIR"
