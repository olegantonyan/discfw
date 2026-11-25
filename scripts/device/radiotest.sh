#!/bin/sh
# On-device: Wi-Fi + BT bring-up exactly as stock (bcmdhd with stock GPIO params, brcm_patchram_plus on ttyS0), then scan.
# Tools may live in /tmp/radio (bin + lib) during development.
[ -d /tmp/radio ] && export PATH=/tmp/radio/bin:$PATH LD_LIBRARY_PATH=/tmp/radio/lib
up() { cut -d' ' -f1 /proc/uptime; }
mark=$(up)
klog() { dmesg | awk -v m="$mark" -F'[][]' '$2 + 0 >= m' | grep -viE 'period=|duty=|baic|as_dma|android_work' | tail -${1:-25}; }
echo "== bcmdhd"
if ! grep -q '^bcmdhd ' /proc/modules; then
	(cd /lib/modules/4.4.94/fiio && sh ./bcmdhd.sh) || echo "insmod failed"
fi
for i in $(seq 1 50); do [ -e /sys/class/net/wlan0 ] && break; sleep 0.2; done
klog 30
ls /sys/class/net
for r in /sys/class/rfkill/*; do [ -d "$r" ] && echo "rfkill $(cat $r/name) $(cat $r/type) soft=$(cat $r/soft)" && echo 0 > $r/soft; done
echo "== wlan0 scan"
ip link set wlan0 up 2>/dev/null || ifconfig wlan0 up
sleep 1
iw dev
iw dev wlan0 scan 2>&1 | awk '/^BSS/ { n++ } /signal:|SSID:/ { print "  " $0 }' | head -24
echo "networks seen: $(iw dev wlan0 scan dump 2>/dev/null | grep -c '^BSS')"
echo "== bluetooth"
pidof brcm_patchram_plus >/dev/null || brcm_patchram_plus --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 > /tmp/patchram.log 2>&1 &
for i in $(seq 1 75); do [ -e /sys/class/bluetooth/hci0 ] && break; sleep 0.2; done
sleep 2
cat /tmp/patchram.log | tail -5
hciconfig hci0 up && hciconfig -a hci0 | head -12
echo "== bt inquiry (8 s)"
hcitool -i hci0 scan --length=6 2>&1 | head -20
echo "== kernel log"
klog 20
