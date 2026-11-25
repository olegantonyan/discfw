#!/bin/sh
# On-device: play each file via MPD; verify PCM rate, DMA pace, SP_SRATE on both CS43131s, DAC unmuted. Usage: ratetest.sh DIR
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0
dir=${1:-/tmp/corpus}
pass=0
fail=0
code() {
	case $1 in
	32000) echo 0x00 ;; 44100) echo 0x01 ;; 48000) echo 0x02 ;; 88200) echo 0x03 ;; 96000) echo 0x04 ;;
	176400) echo 0x05 ;; 192000) echo 0x06 ;; 352800) echo 0x07 ;; 384000) echo 0x08 ;; *) echo none ;;
	esac
}
srate() { i2ctransfer -f -y 3 w4@$1 0x01 0x00 0x0b 0x00 r1 2>/dev/null; }
$M -q repeat off
$M -q single on
for f in "$dir"/*; do
	name=${f##*/}
	$M -q clear
	$M -q add "file://$f"
	$M -q play 1
	sleep 0.8
	h1=$(sed -n 's/^hw_ptr *: //p' $S/status); t1=$(cut -d' ' -f1 /proc/uptime)
	sleep 0.6
	h2=$(sed -n 's/^hw_ptr *: //p' $S/status); t2=$(cut -d' ' -f1 /proc/uptime)
	st=$(sed -n 's/^state: //p' $S/status)
	rate=$(sed -n 's/^rate: \([0-9]*\).*/\1/p' $S/hw_params)
	fmt=$(sed -n 's/^format: //p' $S/hw_params)
	a=$(srate 0x30); c=$(srate 0x32); want=$(code "$rate")
	dac=$(discctl status)
	mpdst=$($M status | sed -n 's/^\[\([a-z]*\)\].*/\1/p')
	err=$($M status | sed -n 's/^ERROR: //p')
	v=$(awk -v a="$h1" -v b="$h2" -v s="$t1" -v e="$t2" -v r="$rate" -v st="$st" 'BEGIN {
		fps = (a == "" || b == "") ? 0 : (b - a) / (e - s)
		printf "%s %d", (st == "RUNNING" && r > 0 && fps > r * 0.85 && fps < r * 1.15) ? "ok" : "bad", fps }')
	if [ "${v%% *}" = ok ] && [ "$a" = "$want" ] && [ "$c" = "$want" ] && [ "$fmt" = S32_LE ] && echo "$dac" | grep -q "rate=$rate .*muted=0"; then
		r=PASS; pass=$((pass + 1))
	else
		r=FAIL; fail=$((fail + 1))
	fi
	echo "$r $name: mpd=$mpdst pcm=$st $fmt rate=$rate fps=${v#* } srate=$a/$c want=$want | $dac ${err:+| err=$err}"
	while [ "$($M status | sed -n 's/^\[\([a-z]*\)\].*/\1/p')" = playing ]; do sleep 0.3; done
	sleep 0.3
	echo "   after end: pcm=$(sed -n 's/^state: //p' $S/status) $(head -1 $S/status | grep -q closed && echo closed) | $(discctl status)"
done
$M -q single off
echo "RESULT pass=$pass fail=$fail"
