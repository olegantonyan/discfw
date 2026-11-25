#!/bin/sh
# On-device: expose a read-only RAM disk as USB mass storage next to the ACM shell for N seconds, then restore ACM only.
# Run detached (the USB link drops during rebind): setsid sh msctest.sh 40 > /tmp/msctest.log 2>&1 < /dev/null &
G=/sys/kernel/config/usb_gadget/disc
F=$G/functions/mass_storage.0
secs=${1:-40}
udc=$(cat $G/UDC)
img=/tmp/msc.img
dd if=/dev/zero of=$img bs=1M count=8 2>/dev/null
echo "discfw msc test $(date)" > /tmp/msc-hello.txt
mkdir -p $F
echo 1 > $F/lun.0/ro
echo 1 > $F/lun.0/removable
echo $img > $F/lun.0/file
echo "" > $G/UDC
ln -s $F $G/configs/c.1/ 2>/dev/null || ln -s $F "$(ls -d $G/configs/*/ | head -1)"
echo "$udc" > $G/UDC
echo "bound with mass_storage: $(cat $G/UDC) $(ls $G/configs/*/)"
sleep "$secs"
echo "" > $G/UDC
rm -f $G/configs/*/mass_storage.0
rmdir $F
for i in $(seq 1 20); do echo "$udc" > $G/UDC 2>/dev/null && break; sleep 1; done
echo "restored: $(cat $G/UDC) $(ls $G/configs/*/)"
