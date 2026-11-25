#!/bin/sh
# On-device: add a UAC2 (USB audio) function next to the ACM shell for N seconds, then restore ACM only.
# Run detached: setsid sh uactest.sh 90 > /tmp/uactest.log 2>&1 < /dev/null &
G=/sys/kernel/config/usb_gadget/disc
F=$G/functions/uac2.a
secs=${1:-90}
udc=$(cat $G/UDC)
mkdir -p $F || { echo "no uac2 function"; exit 1; }
for a in c_srate c_ssize c_chmask p_srate p_ssize p_chmask req_number product_name; do [ -e $F/$a ] && echo "default $a=$(cat $F/$a)"; done
echo "44100,48000,88200,96000,176400,192000,352800,384000" > $F/c_srate 2>/dev/null && echo "multi-rate c_srate accepted" || echo "multi-rate c_srate rejected"
echo 4 > $F/c_ssize
echo 3 > $F/c_chmask
[ -e $F/product_name ] && echo "discfw USB DAC" > $F/product_name
for a in c_srate c_ssize c_chmask; do echo "set $a=$(cat $F/$a)"; done
echo "" > $G/UDC
ln -s $F $G/configs/c.1/
echo "$udc" > $G/UDC && echo "bound with uac2"
sleep 2
cat /proc/asound/cards
sleep "$secs"
echo "" > $G/UDC
rm -f $G/configs/c.1/uac2.a
rmdir $F
for i in $(seq 1 20); do echo "$udc" > $G/UDC 2>/dev/null && break; sleep 1; done
echo "restored: $(cat $G/UDC) $(ls $G/configs/c.1/)"
